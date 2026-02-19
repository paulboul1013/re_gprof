all:
	gcc -Wall -D_GNU_SOURCE -DAUTO_PROFILE -g -pthread -rdynamic -no-pie -fno-pie -o main main.c -lm -ldl

clean:
	rm -f main main.o gmon.out