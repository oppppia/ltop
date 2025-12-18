all:
	gcc -O2 -Wall -Wextra -lncurses -pedantic ./src/main.c -o ltop
clean:
	rm -f ltop
start:
	./ltop
