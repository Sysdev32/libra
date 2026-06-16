savedcmd_kconfig/conf.o := gcc -Wp,-MMD,kconfig/.conf.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/conf.o .source/kconfig/conf.c

source_kconfig/conf.o := .source/kconfig/conf.c

deps_kconfig/conf.o := \
  .source/kconfig/internal.h \
  .source/kconfig/hashtable.h \
  .source/kconfig/array_size.h \
  .source/kconfig/list.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/lkc_proto.h \

kconfig/conf.o: $(deps_kconfig/conf.o)

$(deps_kconfig/conf.o):
