savedcmd_kconfig/mnconf-common.o := gcc -Wp,-MMD,kconfig/.mnconf-common.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./kconfig -c -o kconfig/mnconf-common.o .source/kconfig/mnconf-common.c

source_kconfig/mnconf-common.o := .source/kconfig/mnconf-common.c

deps_kconfig/mnconf-common.o := \
  .source/kconfig/expr.h \
  .source/kconfig/list_types.h \
  .source/kconfig/list.h \
  .source/kconfig/mnconf-common.h \

kconfig/mnconf-common.o: $(deps_kconfig/mnconf-common.o)

$(deps_kconfig/mnconf-common.o):
