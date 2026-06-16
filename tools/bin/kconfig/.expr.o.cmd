savedcmd_kconfig/expr.o := gcc -Wp,-MMD,kconfig/.expr.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/expr.o .source/kconfig/expr.c

source_kconfig/expr.o := .source/kconfig/expr.c

deps_kconfig/expr.o := \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc_proto.h \

kconfig/expr.o: $(deps_kconfig/expr.o)

$(deps_kconfig/expr.o):
