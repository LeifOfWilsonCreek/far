all: far
far: far.o trav.o
	$(CC) -o far far.o trav.o
far.o: far.h trav.h far.c
trav.o: trav.h trav.c
travtest: travtest.o trav.o
	$(CC) -o travtest travtest.o trav.o
install: far
	install -o 0 -g 0 -m 0755 far /usr/local/bin/far
clean:
	rm -f far.o trav.o travtest.o far travtest *.core core
