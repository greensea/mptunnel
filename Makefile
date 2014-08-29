CC = gcc
CFLAGS = -g -Wall -I/usr/include/libev 
LDFLAGS = -g  -lev -lpthread

all: client server


client: client.o net.o mptunnel.o rbtree.o
	$(CC) $(LDFLAGS) $^  -o client

server: server.c mptunnel.o net.o
	$(CC) $(LDFLAGS) $^  -o server


SOURCE = $(wildcard *.c)
	sinclude $(SOURCE:.c=.d)
	
%.d: %.c
	$(CC) -MT "$*.o $*.d" -MM $(CFLAGS) $< > $@

 
clean:
	rm -f *.o
	rm -f *.d
	rm -f server
