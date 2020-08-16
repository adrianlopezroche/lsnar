lsnar: main.o
	gcc -g main.o -o lsnar

main.o: main.c
	gcc -c -g main.c -o main.o

install: lsnar
	cp lsnar /usr/local/bin
	cp lsnar.1 /usr/local/man/man1

clean:
	rm -f lsnar
	rm *.o
