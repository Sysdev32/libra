savedcmd_kconfig/lxdialog/inputbox.o := gcc -Wp,-MMD,kconfig/lxdialog/.inputbox.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/lxdialog/inputbox.o .source/kconfig/lxdialog/inputbox.c

source_kconfig/lxdialog/inputbox.o := .source/kconfig/lxdialog/inputbox.c

deps_kconfig/lxdialog/inputbox.o := \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \

kconfig/lxdialog/inputbox.o: $(deps_kconfig/lxdialog/inputbox.o)

$(deps_kconfig/lxdialog/inputbox.o):
