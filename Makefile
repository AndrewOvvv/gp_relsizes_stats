MODULE_big      = gp_relsizes_stats
OBJS            = ./src/gp_relsizes_stats.o
EXTENSION       = gp_relsizes_stats
EXTVERSION      = 1.0
#DATA            = $(wildcard sql/*--*.sql)
DATA            = sql/gp_relsizes_stats--1.0.sql
REGRESS         = gp_relsizes_stats
REGRESS_OPTS    = --inputdir=test/
PGFILEDESC      = "gp_relsizes_stats - an extension to track table on-disc sizes in greenplum"
PG_CXXFLAGS     += $(COMMON_CPP_FLAGS)
PG_CONFIG       = pg_config
PGXS            := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
