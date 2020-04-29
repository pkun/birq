/*
 * birq
 *
 * Balance IRQ
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <time.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "birq.h"
#include "lub/log.h"
#include "lub/list.h"
#include "lub/ini.h"
#include "irq.h"
#include "numa.h"
#include "cpu.h"
#include "statistics.h"
#include "balance.h"
#include "pxm.h"

#ifndef VERSION
#define VERSION "1.2.0"
#endif

/* Signal handlers */
static volatile int sigterm = 0; /* Exit if 1 */
static void sighandler(int signo);

static volatile int sighup = 0; /* Re-read config file */
static void sighup_handler(int signo);

static void help(int status, const char *argv0);
static struct options *opts_init(void);
static void opts_free(struct options *opts);
static int opts_parse(int argc, char *argv[], struct options *opts);
static int parse_config(const char *fname, struct options *opts);

/* Command line options */
struct options {
	char *pidfile;
	char *cfgfile;
	int cfgfile_userdefined;
	char *pxm; /* Proximity config file */
	int debug; /* Don't daemonize in debug mode */
	int log_facility;
	float threshold;
	float load_limit;
	int verbose;
	int ht;
	int non_local_cpus;
	unsigned int long_interval;
	unsigned int short_interval;
	birq_choose_strategy_e strategy;
	cpumask_t exclude_cpus;
};

/*--------------------------------------------------------- */
int main(int argc, char **argv)
{
	int retval = -1;
	struct options *opts = NULL;
	int pidfd = -1;
	unsigned int interval;

	/* Signal vars */
	struct sigaction sig_act;
	sigset_t sig_set;

	/* IRQ list. It contain all found IRQs. */
	lub_list_t *irqs;
	/* IRQs need to be balanced */
	lub_list_t *balance_irqs;
	/* CPU list. It contain all found CPUs. */
	lub_list_t *cpus;
	/* NUMA list. It contain all found NUMA nodes. */
	lub_list_t *numas;
	/* Proximity list. */
	lub_list_t *pxms;

	/* Parse command line options */
	opts = opts_init();
	if (opts_parse(argc, argv, opts))
		goto err;

	/* Parse config file */
	if (!access(opts->cfgfile, R_OK)) {
		if (parse_config(opts->cfgfile, opts))
			goto err;
	} else if (opts->cfgfile_userdefined) {
		fprintf(stderr, "Error: Can't find config file %s\n",
			opts->cfgfile);
		goto err;
	}

	/* Initialize syslog */
	openlog(argv[0], LOG_CONS, opts->log_facility);
	syslog(LOG_ERR, "Start daemon.\n");

	/* Fork the daemon */
	if (!opts->debug) {
		/* Daemonize */
		if (daemon(0, 0) < 0) {
			syslog(LOG_ERR, "Can't daemonize\n");
			goto err;
		}

		/* Write pidfile */
		if ((pidfd = open(opts->pidfile,
			O_WRONLY | O_CREAT | O_EXCL | O_TRUNC,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) < 0) {
			syslog(LOG_WARNING, "Can't open pidfile %s: %s\n",
				opts->pidfile, strerror(errno));
		} else {
			char str[20];
			snprintf(str, sizeof(str), "%u\n", getpid());
			str[sizeof(str) - 1] = '\0';
			if (write(pidfd, str, strlen(str)) < 0)
				syslog(LOG_WARNING, "Can't write to %s: %s\n",
					opts->pidfile, strerror(errno));
			close(pidfd);
		}
	}

	/* Set signal handler */
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGTERM);
	sigaddset(&sig_set, SIGINT);
	sigaddset(&sig_set, SIGQUIT);

	sig_act.sa_flags = 0;
	sig_act.sa_mask = sig_set;
	sig_act.sa_handler = &sighandler;
	sigaction(SIGTERM, &sig_act, NULL);
	sigaction(SIGINT, &sig_act, NULL);
	sigaction(SIGQUIT, &sig_act, NULL);

	/* SIGHUP handler */
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGHUP);

	sig_act.sa_flags = 0;
	sig_act.sa_mask = sig_set;
	sig_act.sa_handler = &sighup_handler;
	sigaction(SIGHUP, &sig_act, NULL);

	/* Randomize */
	srand(time(NULL));

	/* Scan NUMA nodes */
	numas = lub_list_new(numa_list_compare);
	scan_numas(numas);
	if (opts->verbose)
		show_numas(numas);

	/* Scan CPUs */
	cpus = lub_list_new(cpu_list_compare);
	scan_cpus(cpus, opts->ht);
	if (opts->verbose)
		show_cpus(cpus);

	/* Prepare data structures */
	irqs = lub_list_new(irq_list_compare);
	balance_irqs = lub_list_new(irq_list_compare);

	/* Parse proximity file */
	pxms = lub_list_new(NULL);
	if (opts->pxm)
		parse_pxm_config(opts->pxm, pxms, numas);
	if (opts->verbose)
		show_pxms(pxms);

	/* Main loop */
	while (!sigterm) {
		lub_list_node_t *node;
		char outstr[10];
		time_t t;
		struct tm *tmp;

		t = time(NULL);
		tmp = localtime(&t);
		if (tmp) {
			strftime(outstr, sizeof(outstr), "%H:%M:%S", tmp);
			printf("----[ %s ]----------------------------------------------------------------\n", outstr);
		}

		/* Re-read config file on SIGHUP */
		if (sighup) {
			if (!access(opts->cfgfile, R_OK)) {
				syslog(LOG_ERR, "Re-reading config file\n");
				if (parse_config(opts->cfgfile, opts))
					syslog(LOG_ERR, "Error while config file parsing.\n");
			} else if (opts->cfgfile_userdefined)
				syslog(LOG_ERR, "Can't find config file.\n");
			sighup = 0;
		}

		/* Rescan PCI devices for new IRQs. */
		scan_irqs(irqs, balance_irqs, pxms);
		if (opts->verbose)
			irq_list_show(irqs);
		/* Link IRQs to CPUs due to real current smp affinity. */
		link_irqs_to_cpus(cpus, irqs);

		/* Gather statistics on CPU load and number of interrupts. */
		gather_statistics(cpus, irqs);
		show_statistics(cpus, opts->verbose);
		/* Choose IRQ to move to another CPU. */
		choose_irqs_to_move(cpus, balance_irqs,
			opts->threshold, opts->strategy, &opts->exclude_cpus);

		/* Balance IRQs */
		if (lub_list_len(balance_irqs) != 0) {
			/* Set short interval to make balancing faster. */
			interval = opts->short_interval;
			/* Choose new CPU for IRQs need to be balanced. */
			balance(cpus, balance_irqs, opts->load_limit,
				&opts->exclude_cpus, opts->non_local_cpus);
			/* Write new values to /proc/irq/<IRQ>/smp_affinity */
			apply_affinity(balance_irqs);
			/* Free list of balanced IRQs */
			while ((node = lub_list__get_tail(balance_irqs))) {
				lub_list_del(balance_irqs, node);
				lub_list_node_free(node);
			}
		} else {
			/* If nothing to balance */
			interval = opts->long_interval;
		}
		
		/* Wait before next iteration */
		sleep(interval);
	}

	/* Free data structures */
	irq_list_free(irqs);
	lub_list_free(balance_irqs);
	cpu_list_free(cpus);
	numa_list_free(numas);
	pxm_list_free(pxms);

	retval = 0;
err:
	/* Remove pidfile */
	if (pidfd >= 0) {
		if (unlink(opts->pidfile) < 0) {
			syslog(LOG_ERR, "Can't remove pid-file %s: %s\n",
			opts->pidfile, strerror(errno));
		}
	}

	/* Free command line options */
	opts_free(opts);
	syslog(LOG_ERR, "Stop daemon.\n");

	return retval;
}

/*--------------------------------------------------------- */
/* Signal handler for temination signals (like SIGTERM, SIGINT, ...) */
static void sighandler(int signo)
{
	sigterm = 1;
	signo = signo; /* Happy compiler */
}

/*--------------------------------------------------------- */
/* Re-read config file on SIGHUP */
static void sighup_handler(int signo)
{
	sighup = 1;
	signo = signo; /* Happy compiler */
}

/*--------------------------------------------------------- */
/* Set defaults for options from config file (not command line) */
static void opts_default_config(struct options *opts)
{
	assert(opts);

	opts->threshold = BIRQ_DEFAULT_THRESHOLD;
	opts->load_limit = BIRQ_DEFAULT_LOAD_LIMIT;
	opts->ht = 1; /* It's 1 since 1.5.0 */
	opts->non_local_cpus = 0;
	opts->long_interval = BIRQ_LONG_INTERVAL;
	opts->short_interval = BIRQ_SHORT_INTERVAL;
	opts->strategy = BIRQ_CHOOSE_RND;
	cpus_clear(opts->exclude_cpus);
}
/*--------------------------------------------------------- */
/* Initialize option structure by defaults */
static struct options *opts_init(void)
{
	struct options *opts = NULL;

	// Allocate structures
	opts = malloc(sizeof(*opts));
	assert(opts);
	cpus_init(opts->exclude_cpus);

	// Set command line options defaults.
	// Don't set defaults for config file options here because it must be
	// done every time while config file re-read. See opts_default_config().
	// The parse_config() function must set it before parsing.
	opts->debug = 0; /* daemonize by default */
	opts->pidfile = strdup(BIRQ_PIDFILE);
	opts->cfgfile = strdup(BIRQ_CFGFILE);
	opts->cfgfile_userdefined = 0;
	opts->pxm = NULL;
	opts->log_facility = LOG_DAEMON;
	opts->verbose = 0;

	return opts;
}

/*--------------------------------------------------------- */
/* Free option structure */
static void opts_free(struct options *opts)
{
	if (opts->pidfile)
		free(opts->pidfile);
	if (opts->cfgfile)
		free(opts->cfgfile);
	if (opts->pxm)
		free(opts->pxm);
	cpus_free(opts->exclude_cpus);
	free(opts);
}

/* Parse y/n options */
static int opt_parse_y_n(const char *optarg, int *flag)
{
	assert(optarg);
	assert(flag);

	if (!strcmp(optarg, "y"))
		*flag = 1;
	else if (!strcmp(optarg, "yes"))
		*flag = 1;
	else if (!strcmp(optarg, "n"))
		*flag = 0;
	else if (!strcmp(optarg, "no"))
		*flag = 0;
	else {
		fprintf(stderr, "Error: Illegal flag value %s.\n", optarg);
		return -1;
	}
	return 0;
}

/* Parse 'strategy' option */
static int opt_parse_strategy(const char *optarg, birq_choose_strategy_e *strategy)
{
	assert(optarg);
	assert(strategy);

	if (!strcmp(optarg, "max"))
		*strategy = BIRQ_CHOOSE_MAX;
	else if (!strcmp(optarg, "min"))
		*strategy = BIRQ_CHOOSE_MIN;
	else if (!strcmp(optarg, "rnd"))
		*strategy = BIRQ_CHOOSE_RND;
	else {
		fprintf(stderr, "Error: Illegal strategy value %s.\n", optarg);
		return -1;
	}
	return 0;
}

/* Parse 'threshold' and 'load-limit' options */
static int opt_parse_threshold(const char *optarg, float *threshold)
{
	char *endptr;
	float thresh;

	assert(optarg);
	assert(threshold);

	thresh = strtof(optarg, &endptr);
	if (endptr == optarg) {
		fprintf(stderr, "Error: Illegal threshold/load-limit value %s.\n", optarg);
		return -1;
	}
	if (thresh > 100.00) {
		fprintf(stderr, "Error: The threshold/load-limit value %s > 100.\n", optarg);
		return -1;
	}
	*threshold = thresh;
	return 0;
}

/* Parse 'short-interval' and 'long-interval' options */
static int opt_parse_interval(const char *optarg, unsigned int *interval)
{
	char *endptr;
	unsigned long int val;

	assert(optarg);
	assert(interval);

	val = strtoul(optarg, &endptr, 10);
	if (endptr == optarg) {
		fprintf(stderr, "Error: Illegal interval value %s.\n", optarg);
		return -1;
	}
	*interval = val;
	return 0;
}

/*--------------------------------------------------------- */
/* Parse command line options */
static int opts_parse(int argc, char *argv[], struct options *opts)
{
	static const char *shortopts = "hp:c:dO:vx:";
#ifdef HAVE_GETOPT_H
	static const struct option longopts[] = {
		{"help",		0, NULL, 'h'},
		{"pid",			1, NULL, 'p'},
		{"conf",		1, NULL, 'c'},
		{"debug",		0, NULL, 'd'},
		{"facility",		1, NULL, 'O'},
		{"verbose",		0, NULL, 'v'},
		{"pxm",			1, NULL, 'x'},
		{NULL,			0, NULL, 0}
	};
#endif
	optind = 1;
	while(1) {
		int opt;
#ifdef HAVE_GETOPT_H
		opt = getopt_long(argc, argv, shortopts, longopts, NULL);
#else
		opt = getopt(argc, argv, shortopts);
#endif
		if (-1 == opt)
			break;
		switch (opt) {
		case 'p':
			if (opts->pidfile)
				free(opts->pidfile);
			opts->pidfile = strdup(optarg);
			break;
		case 'c':
			if (opts->cfgfile)
				free(opts->cfgfile);
			opts->cfgfile = strdup(optarg);
			opts->cfgfile_userdefined = 1;
			break;
		case 'x':
			if (opts->pxm)
				free(opts->pxm);
			opts->pxm = strdup(optarg);
			break;
		case 'd':
			opts->debug = 1;
			break;
		case 'v':
			opts->verbose = 1;
			break;
		case 'O':
			if (lub_log_facility(optarg, &(opts->log_facility))) {
				fprintf(stderr, "Error: Illegal syslog facility %s.\n", optarg);
				exit(-1);
			}
			break;
		case 'h':
			help(0, argv[0]);
			exit(0);
			break;
		default:
			help(-1, argv[0]);
			exit(-1);
			break;
		}
	}


	return 0;
}

/*--------------------------------------------------------- */
/* Print help message */
static void help(int status, const char *argv0)
{
	const char *name = NULL;

	if (!argv0)
		return;

	/* Find the basename */
	name = strrchr(argv0, '/');
	if (name)
		name++;
	else
		name = argv0;

	if (status != 0) {
		fprintf(stderr, "Try `%s -h' for more information.\n",
			name);
	} else {
		printf("Version : %s\n", VERSION);
		printf("Usage   : %s [options]\n", name);
		printf("Daemon to balance IRQs.\n");
		printf("Options :\n");
		printf("\t-h, --help Print this help.\n");
		printf("\t-d, --debug Debug mode. Don't daemonize.\n");
		printf("\t-v, --verbose Be verbose.\n");
		printf("\t-r, --ht This option is obsoleted. The Hyper Threading is enabled by default.\n");
		printf("\t-p <path>, --pid=<path> File to save daemon's PID to (" BIRQ_PIDFILE ").\n");
		printf("\t-c <path>, --conf=<path> Config file (" BIRQ_CFGFILE ").\n");
		printf("\t-x <path>, --pxm=<path> Proximity config file.\n");
		printf("\t-O, --facility Syslog facility (DAEMON).\n");
		printf("\t-t <float>, --threshold=<float> Threshold to consider CPU is overloaded, in percents. Default threhold is %.2f.\n",
			BIRQ_DEFAULT_THRESHOLD);
		printf("\t-l <float>, --load-limit=<float> Don't move IRQs to CPUs loaded more than this limit, in percents. Default limit is %.2f.\n",
			BIRQ_DEFAULT_LOAD_LIMIT);
		printf("\t-i <sec>, --short-interval=<sec> Short iteration interval.\n");
		printf("\t-I <sec>, --long-interval=<sec> Long iteration interval.\n");
		printf("\t-s <strategy>, --strategy=<strategy> Strategy to choose IRQ to move (min/max/rnd).\n");
	}
}

/*--------------------------------------------------------- */
/* Parse config file */
static int parse_config(const char *fname, struct options *opts)
{
	int ret = -1; /* Pessimistic retval */
	lub_ini_t *ini = NULL;
	const char *tmp = NULL;
	cpumask_t use_cpus;

	// Set options defaults
	opts_default_config(opts);

	ini = lub_ini_new();
	if (lub_ini_parse_file(ini, fname)) {
		lub_ini_free(ini);
		return -1;
	}

	if ((tmp = lub_ini_find(ini, "strategy")))
		if (opt_parse_strategy(tmp, &opts->strategy) < 0)
			goto err;

	if ((tmp = lub_ini_find(ini, "threshold")))
		if (opt_parse_threshold(tmp, &opts->threshold))
			goto err;

	if ((tmp = lub_ini_find(ini, "load-limit")))
		if (opt_parse_threshold(tmp, &opts->load_limit))
			goto err;

	if ((tmp = lub_ini_find(ini, "short-interval")))
		if (opt_parse_interval(tmp, &opts->short_interval))
			goto err;

	if ((tmp = lub_ini_find(ini, "long-interval")))
		if (opt_parse_interval(tmp, &opts->long_interval))
			goto err;

	if ((tmp = lub_ini_find(ini, "exclude-cpus"))) {
		if (cpumask_parse_user(tmp, strlen(tmp), opts->exclude_cpus)) {
			fprintf(stderr, "Error: Can't parse exclude-cpus option \"%s\".\n", tmp);
			goto err;
		}
	}

	cpus_init(use_cpus);
	if ((tmp = lub_ini_find(ini, "use-cpus"))) {
		if (cpumask_parse_user(tmp, strlen(tmp), use_cpus)) {
			fprintf(stderr, "Error: Can't parse use-cpus option \"%s\".\n", tmp);
			goto err;
		}
	} else {
		cpus_setall(use_cpus);
	}

	/* The exclude-cpus option was implemented first. So the
	 * programm is based on it. The use-cpus options really
	 * says to exclude all the cpus that is not within bitmask.
	 * So invert use-cpus and we'll get exclude-cpus mask.
	 */
	cpus_complement(use_cpus, use_cpus);
	/* Calculate real exclude-cpu mask (consider use-cpus option).
	 * real-exclude-cpus = exclude-cpus | ~use-cpus
	 */
	cpus_or(opts->exclude_cpus, opts->exclude_cpus, use_cpus);
	cpus_free(use_cpus);

	if ((tmp = lub_ini_find(ini, "ht")))
		if (opt_parse_y_n(tmp, &opts->ht))
			goto err;

	if ((tmp = lub_ini_find(ini, "non-local-cpus")))
		if (opt_parse_y_n(tmp, &opts->non_local_cpus))
			goto err;

	ret = 0;
err:
	lub_ini_free(ini);
	return ret;
}
