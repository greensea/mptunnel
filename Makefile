CC = gcc
CFLAGS = -g -Wall -I/usr/include/libev 
LDFLAGS = -g  -lev -lpthread

all: client


client: client.c net.c
	$(CC) $(LDFLAGS) $^  -o client

server: server.c
	$(CC) $(LDFLAGS) $^  -o server


SOURCE = $(wildcard *.c)
	sinclude $(SOURCE:.c=.d)
	
%.d: %.c
	$(CC) -MT "$*.o $*.d" -MM $(CFLAGS) $< > $@

 
clean:
	rm -f *.o
	rm -f *.d
	rm -f server
