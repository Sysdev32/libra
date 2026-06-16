savedcmd_kconfig/lxdialog/textbox.o := gcc -Wp,-MMD,kconfig/lxdialog/.textbox.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/lxdialog/textbox.o .source/kconfig/lxdialog/textbox.c

source_kconfig/lxdialog/textbox.o := .source/kconfig/lxdialog/textbox.c

deps_kconfig/lxdialog/textbox.o := \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \

kconfig/lxdialog/textbox.o: $(deps_kconfig/lxdialog/textbox.o)

$(deps_kconfig/lxdialog/textbox.o):
