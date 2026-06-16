savedcmd_kconfig/symbol.o := gcc -Wp,-MMD,kconfig/.symbol.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/symbol.o .source/kconfig/symbol.c

source_kconfig/symbol.o := .source/kconfig/symbol.c

deps_kconfig/symbol.o := \
  .source/kconfig/internal.h \
  .source/kconfig/hashtable.h \
  .source/kconfig/array_size.h \
  .source/kconfig/list.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/lkc_proto.h \

kconfig/symbol.o: $(deps_kconfig/symbol.o)

$(deps_kconfig/symbol.o):
