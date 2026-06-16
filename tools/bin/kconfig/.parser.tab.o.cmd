savedcmd_kconfig/parser.tab.o := gcc -Wp,-MMD,kconfig/.parser.tab.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -I .source/kconfig -I ./kconfig -c -o kconfig/parser.tab.o kconfig/parser.tab.c

source_kconfig/parser.tab.o := kconfig/parser.tab.c

deps_kconfig/parser.tab.o := \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc_proto.h \
  .source/kconfig/internal.h \
  .source/kconfig/hashtable.h \
  .source/kconfig/array_size.h \
  .source/kconfig/list.h \
  .source/kconfig/preprocess.h \
  kconfig/parser.tab.h \

kconfig/parser.tab.o: $(deps_kconfig/parser.tab.o)

$(deps_kconfig/parser.tab.o):
