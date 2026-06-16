savedcmd_kconfig/lexer.lex.o := gcc -Wp,-MMD,kconfig/.lexer.lex.o.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11    -I .source/kconfig -I ./kconfig -c -o kconfig/lexer.lex.o kconfig/lexer.lex.c

source_kconfig/lexer.lex.o := kconfig/lexer.lex.c

deps_kconfig/lexer.lex.o := \
  .source/kconfig/lkc.h \
    $(wildcard include/config/prefix) \
  .source/kconfig/expr.h \
  .source/kconfig/list_types.h \
  .source/kconfig/lkc_proto.h \
  .source/kconfig/preprocess.h \
  kconfig/parser.tab.h \

kconfig/lexer.lex.o: $(deps_kconfig/lexer.lex.o)

$(deps_kconfig/lexer.lex.o):
