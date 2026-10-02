CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
OBJS     = d11_core.o fakephy.o psm42.o ops.o snapshot.o chip.o d11sim.o

d11sim: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

d11_core.o: d11_core.c d11_core.h
fakephy.o:  fakephy.c fakephy.h
psm42.o:    psm42.c psm42.h d11_core.h
ops.o:      ops.c ops.h d11_core.h fakephy.h chip.h
chip.o:     chip.c chip.h
snapshot.o: snapshot.c snapshot.h d11_core.h
d11sim.o:   d11sim.c d11_core.h fakephy.h psm42.h ops.h snapshot.h chip.h

clean:
	rm -f $(OBJS) d11sim

.PHONY: clean
