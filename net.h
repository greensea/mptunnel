#ifndef __NET_H__
#define __NET_H__

int net_bind(const char*, int, int);

int net_accept(int, void* ip);

int net_connect(const char* server_host, int server_port, int type);

#endif
