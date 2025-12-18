all:
	gcc src/main.c -o ltop
clean:
	rm -f ltop
start:
	./ltop
