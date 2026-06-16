savedcmd_kconfig/util.o := gcc -Wp,-MMD,kconfig/.util.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/util.o .source/kconfig/util.c

source_kconfig/util.o := .source/kconfig/util.c

deps_kconfig/util.o := \
  .source/kconfig/hashtable.h \
  .source/kconfig/array_size.h \
  .source/kconfig/list.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/lkc_proto.h \

kconfig/util.o: $(deps_kconfig/util.o)

$(deps_kconfig/util.o):
