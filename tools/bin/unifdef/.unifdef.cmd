savedcmd_unifdef/unifdef := gcc -Wp,-MMD,unifdef/.unifdef.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./unifdef   -o unifdef/unifdef .source/unifdef/unifdef.c   

source_unifdef/unifdef := .source/unifdef/unifdef.c

deps_unifdef/unifdef := \

unifdef/unifdef: $(deps_unifdef/unifdef)

$(deps_unifdef/unifdef):
