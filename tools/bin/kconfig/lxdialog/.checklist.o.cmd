savedcmd_kconfig/lxdialog/checklist.o := gcc -Wp,-MMD,kconfig/lxdialog/.checklist.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -D_GNU_SOURCE -I/usr/include/ncursesw -I ./kconfig -c -o kconfig/lxdialog/checklist.o .source/kconfig/lxdialog/checklist.c

source_kconfig/lxdialog/checklist.o := .source/kconfig/lxdialog/checklist.c

deps_kconfig/lxdialog/checklist.o := \
  .source/kconfig/lxdialog/dialog.h \
  /usr/include/ncursesw/ncurses.h \
  /usr/include/ncursesw/ncurses_dll.h \
  /usr/include/ncursesw/unctrl.h \
  /usr/include/ncursesw/curses.h \

kconfig/lxdialog/checklist.o: $(deps_kconfig/lxdialog/checklist.o)

$(deps_kconfig/lxdialog/checklist.o):
