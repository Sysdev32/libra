savedcmd_kconfig/lxdialog/menubox.o := gcc -Wp,-MMD,kconfig/lxdialog/.menubox.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/lxdialog/menubox.o .source/kconfig/lxdialog/menubox.c

source_kconfig/lxdialog/menubox.o := .source/kconfig/lxdialog/menubox.c

deps_kconfig/lxdialog/menubox.o := \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \

kconfig/lxdialog/menubox.o: $(deps_kconfig/lxdialog/menubox.o)

$(deps_kconfig/lxdialog/menubox.o):
