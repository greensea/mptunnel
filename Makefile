CC = gcc
CFLAGS = -g -Wall -I/usr/include/libev
LDFLAGS = -g  -lev -lpthread

all: mpclient mpserver


mpclient: client.o net.o mptunnel.o rbtree.o
	$(CC) $(LDFLAGS) $^  -o mpclient

mpserver: server.c mptunnel.o net.o rbtree.o
	$(CC) $(LDFLAGS) $^  -o mpserver


SOURCE = $(wildcard *.c)
	sinclude $(SOURCE:.c=.d)
	
%.d: %.c
	$(CC) -MT "$*.o $*.d" -MM $(CFLAGS) $< > $@

 
clean:
	rm -f *.o
	rm -f *.d
	rm -f mpclient mpserver
