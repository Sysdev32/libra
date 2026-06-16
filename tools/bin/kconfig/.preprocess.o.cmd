savedcmd_kconfig/preprocess.o := gcc -Wp,-MMD,kconfig/.preprocess.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/preprocess.o .source/kconfig/preprocess.c

source_kconfig/preprocess.o := .source/kconfig/preprocess.c

deps_kconfig/preprocess.o := \
  .source/kconfig/array_size.h \
  .source/kconfig/internal.h \
  .source/kconfig/hashtable.h \
  .source/kconfig/list.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/lkc_proto.h \
  .source/kconfig/preprocess.h \

kconfig/preprocess.o: $(deps_kconfig/preprocess.o)

$(deps_kconfig/preprocess.o):
