## Process this file with automake to generate Makefile.in

if DEBUG
  DEBUG_CFLAGS = -DDEBUG
endif

AUTOMAKE_OPTIONS = foreign nostdinc
ACLOCAL_AMFLAGS =
AM_CPPFLAGS = -I. -I$(top_srcdir)
AM_LD = $(CC)
AM_CFLAGS = -Wall -D_GNU_SOURCE $(DEBUG_CFLAGS)

sbin_PROGRAMS = birq
lib_LIBRARIES =

noinst_HEADERS = \
	birq.h \
	cpumask.h \
	irq.h \
	cpu.h \
	numa.h \
	statistics.h \
	balance.h \
	pxm.h \
	bit_array.h \
	bit_macros.h \
	hexio.h

birq_SOURCES = \
	birq.c \
	irq.c \
	cpu.c \
	numa.c \
	statistics.c \
	balance.c \
	pxm.c \
	bit_array.c \
	hexio.c

birq_LDADD = liblub.a
birq_DEPENDENCIES = liblub.a

EXTRA_DIST = \
	lub/module.am \
	doc/birq.md \
	examples/birq.conf \
	LICENCE \
	README

include $(top_srcdir)/lub/module.am
