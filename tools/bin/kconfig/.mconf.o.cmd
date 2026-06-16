savedcmd_kconfig/mconf.o := gcc -Wp,-MMD,kconfig/.mconf.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/mconf.o .source/kconfig/mconf.c

source_kconfig/mconf.o := .source/kconfig/mconf.c

deps_kconfig/mconf.o := \
  .source/kconfig/list.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/lkc_proto.h \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \
  .source/kconfig/mnconf-common.h \

kconfig/mconf.o: $(deps_kconfig/mconf.o)

$(deps_kconfig/mconf.o):
