# pegasos2device -- Gigabit-Treiber fuer den Pegasos II (AmigaOS 4.1)
# Befehle:  make hwtest | make dev | make s2test | make all | make clean
.RECIPEPREFIX = >

CC       = ppc-amigaos-gcc
CFLAGS   = -O2 -Wall

# Hardwareschicht
HWSRC    = src
HWOBJS   = obj/mv64361.o obj/mv64361_test.o

# altes Testprogramm (liegt eine Ebene hoeher in src/)
TESTSRC ?= src/pegasos2-gbit-info.c

# Device
DEVSRC   = device
DEVOBJS  = obj/init.o obj/dev_unit.o obj/dev_io.o obj/dev_cmd.o obj/dev_log.o
DEVHWOBJS = obj/mv64361.o

all: hwtest dev s2test

obj:
> mkdir -p obj

$(HWOBJS): obj/%.o: $(HWSRC)/%.c $(HWSRC)/mv64361.h | obj
> $(CC) $(CFLAGS) -c -o $@ $<

$(DEVOBJS): obj/%.o: $(DEVSRC)/%.c $(wildcard $(DEVSRC)/*.h) | obj
> $(CC) $(CFLAGS) -I$(DEVSRC) -c -o $@ $<

# Testprogramm + Hardwareschicht
hwtest: $(HWOBJS)
> $(CC) -O2 -I$(HWSRC) -o pegasos2-gbit-info $(TESTSRC) $(HWOBJS) -lauto

# das Device selbst
dev: $(DEVOBJS) $(DEVHWOBJS)
> $(CC) -nostartfiles -o mv643xx.device $(DEVOBJS) $(DEVHWOBJS) -lauto

# SANA-II-Testclient
s2test: s2test.c
> $(CC) -O2 -Wall -o s2test s2test.c -lauto

clean:
> rm -rf obj pegasos2-gbit-info mv643xx.device s2test

.PHONY: all hwtest dev s2test clean
