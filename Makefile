# Makefile for pg_systemone PostgreSQL extension

EXTENSION = pg_systemone
DATA = pg_systemone--1.0.sql
MODULE_big = pg_systemone
OBJS = src/pg_systemone.o src/cJSON.o

PG_CPPFLAGS += -Isrc
SHLIB_LINK += -lcurl

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
