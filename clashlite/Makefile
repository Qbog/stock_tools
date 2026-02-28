CC=gcc
CFLAGS=-O2 -Wall -Wextra -pedantic -Iinclude
LDFLAGS=-lpthread

SRC=$(wildcard src/*.c)
OBJ=$(SRC:.c=.o)

all: clashlite

clashlite: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f $(OBJ) clashlite

.PHONY: all clean
