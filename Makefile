CC=gcc
CFLAGS=-Wall -Iincludes -Wextra -std=c99 -ggdb -pthread
VPATH=src

all: rserver

rserver: rserver.c

clean:
	rm -rf *~ *.o test

.PHONY : clean all
