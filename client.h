#ifndef __CLIENT_H__
#define __CLIENT_H__

#include "buffer.h"

void* client_thread(void* ptr);

int reconnect_to_server(connections_t *c);

#endif
