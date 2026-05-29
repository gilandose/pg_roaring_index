MODULE_big = pg_roaring_index
OBJS = \
	src/pg_roaring_index.o \
	src/roaring_util.o \
	src/roaring_build.o \
	src/roaring_insert.o \
	src/roaring_scan.o \
	src/roaring_vacuum.o \
	src/roaring_cost.o \
	src/roaring_check.o \
	src/roaring_bgworker.o \
	src/roaring_stats.o \
	src/roaring_customscan.o \
	src/roaring_payload.o \
	src/vendor/croaring/roaring.o

EXTENSION = pg_roaring_index
DATA      = pg_roaring_index--1.0.sql
REGRESS   = roaring_basic roaring_lossy roaring_multicolumn roaring_types roaring_text roaring_stats roaring_check roaring_customscan roaring_include

PG_CPPFLAGS = -Iinclude -Isrc/vendor/croaring -DUSE_CROARING

# CRoaring amalgamation triggers several PG-standard warnings; suppress them
# for that translation unit only, not for our own code.
src/vendor/croaring/roaring.o: CFLAGS += \
	-Wno-declaration-after-statement \
	-Wno-missing-prototypes \
	-Wno-missing-variable-declarations

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

CROARING_H = src/vendor/croaring/roaring.h

ifeq (,$(wildcard $(CROARING_H)))
$(error CRoaring not found. Run: bash scripts/fetch-croaring.sh)
endif

# Recompile all objects when the shared header changes.
PG_ROARING_H = include/pg_roaring_index.h
$(filter-out src/vendor/%, $(OBJS)): $(PG_ROARING_H)

include $(PGXS)
