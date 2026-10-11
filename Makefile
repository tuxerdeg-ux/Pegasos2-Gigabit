# pegasos2device -- Gigabit-Treiber fuer den Pegasos II (AmigaOS 4.1)
# Befehle:  make hwtest | make dev | make build | make s2test | make all | make clean
.RECIPEPREFIX = >

CC       = ppc-amigaos-gcc
CFLAGS   = -O2 -Wall

# Hardwareschicht
HWSRC    = src
HWOBJS   = obj/mv64361.o obj/mv64361_test.o

# altes Testprogramm (liegt eine Ebene hoeher in src/)
TESTSRC ?= src/pegasos2-gbit-info.c

# Device
DEVSRC       = device
DEVCOREOBJS  = obj/init.o obj/dev_unit.o obj/dev_io.o obj/dev_cmd.o
DEVOBJS      = $(DEVCOREOBJS) obj/dev_log.o
BUILDOBJS    = $(DEVCOREOBJS) obj/dev_log_stub.o
DEVHWOBJS = obj/mv64361.o

all: hwtest dev s2test

obj:
> mkdir -p obj

$(HWOBJS): obj/%.o: $(HWSRC)/%.c $(HWSRC)/mv64361.h | obj
> $(CC) $(CFLAGS) -c -o $@ $<

$(DEVCOREOBJS) obj/dev_log.o obj/dev_log_stub.o: obj/%.o: $(DEVSRC)/%.c $(wildcard $(DEVSRC)/*.h) | obj
> $(CC) $(CFLAGS) -I$(DEVSRC) -c -o $@ $<

# Testprogramm + Hardwareschicht
hwtest: $(HWOBJS)
> $(CC) -O2 -I$(HWSRC) -o pegasos2-gbit-info $(TESTSRC) $(HWOBJS) -lauto

# das Device selbst
dev: $(DEVOBJS) $(DEVHWOBJS)
> $(CC) -nostartfiles -o mv643xx_log.device $(DEVOBJS) $(DEVHWOBJS) -lauto

# Device ohne Dateilogger
build: $(BUILDOBJS) $(DEVHWOBJS)
> mkdir -p build
> $(CC) -nostartfiles -o build/mv643xx.device $(BUILDOBJS) $(DEVHWOBJS) -lauto

# SANA-II-Testclient
s2test: src/s2test.c
> $(CC) -O2 -Wall -o s2test $< -lauto

clean:
> rm -rf obj pegasos2-gbit-info mv643xx.device mv643xx_log.device s2test
> rm -f build/mv643xx.device

.PHONY: all hwtest dev build s2test clean
