all:
	gcc -Wall -DAUTO_PROFILE -g -o main main.c 
	
clean:
	rm -f main main.o