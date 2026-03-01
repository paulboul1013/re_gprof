CC := gcc
CFLAGS := -Wall -Wextra -D_GNU_SOURCE -DAUTO_PROFILE -g -pthread -rdynamic -no-pie -fno-pie -Iinclude
LDFLAGS := -lm -ldl
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

all: main

main: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f main $(OBJ) gmon.out
