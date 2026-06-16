savedcmd_kconfig/lxdialog/util.o := gcc -Wp,-MMD,kconfig/lxdialog/.util.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/lxdialog/util.o .source/kconfig/lxdialog/util.c

source_kconfig/lxdialog/util.o := .source/kconfig/lxdialog/util.c

deps_kconfig/lxdialog/util.o := \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \

kconfig/lxdialog/util.o: $(deps_kconfig/lxdialog/util.o)

$(deps_kconfig/lxdialog/util.o):
