savedcmd_kconfig/confdata.o := gcc -Wp,-MMD,kconfig/.confdata.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/confdata.o .source/kconfig/confdata.c

source_kconfig/confdata.o := .source/kconfig/confdata.c

deps_kconfig/confdata.o := \
    $(wildcard include/config/FOO) \
    $(wildcard include/config/X) \
  .source/kconfig/internal.h \
  .source/kconfig/hashtable.h \
  .source/kconfig/array_size.h \
  .source/kconfig/list.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/lkc_proto.h \

kconfig/confdata.o: $(deps_kconfig/confdata.o)

$(deps_kconfig/confdata.o):
