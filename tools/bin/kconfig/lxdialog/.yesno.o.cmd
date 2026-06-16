savedcmd_kconfig/lxdialog/yesno.o := gcc -Wp,-MMD,kconfig/lxdialog/.yesno.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/lxdialog/yesno.o .source/kconfig/lxdialog/yesno.c

source_kconfig/lxdialog/yesno.o := .source/kconfig/lxdialog/yesno.c

deps_kconfig/lxdialog/yesno.o := \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \

kconfig/lxdialog/yesno.o: $(deps_kconfig/lxdialog/yesno.o)

$(deps_kconfig/lxdialog/yesno.o):
