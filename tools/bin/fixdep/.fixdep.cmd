savedcmd_fixdep/fixdep := gcc -Wp,-MMD,fixdep/.fixdep.d -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer -std=gnu11     -I ./fixdep   -o fixdep/fixdep .source/fixdep/fixdep.c   

source_fixdep/fixdep := .source/fixdep/fixdep.c

deps_fixdep/fixdep := \
    $(wildcard include/config/HIS_DRIVER) \
    $(wildcard include/config/MY_OPTION) \
    $(wildcard include/config/FOO) \

fixdep/fixdep: $(deps_fixdep/fixdep)

$(deps_fixdep/fixdep):
