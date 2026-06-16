savedcmd_kconfig/menu.o := gcc -Wp,-MMD,kconfig/.menu.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/menu.o .source/kconfig/menu.c

source_kconfig/menu.o := .source/kconfig/menu.c

deps_kconfig/menu.o := \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc_proto.h \
  .source/kconfig/internal.h \
  .source/kconfig/hashtable.h \
  .source/kconfig/array_size.h \
  .source/kconfig/list.h \

kconfig/menu.o: $(deps_kconfig/menu.o)

$(deps_kconfig/menu.o):
