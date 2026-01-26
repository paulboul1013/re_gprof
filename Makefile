all:
	gcc -Wall -D_GNU_SOURCE -DAUTO_PROFILE -g -pthread -o main main.c

clean:
	rm -f main main.o