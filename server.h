#ifndef __SERVER_H__
#define __SERVER_H__

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "linklist.h"

typedef struct bridge_t {
    struct list_head list;
    struct sockaddr addr;
    socklen_t addrlen;
    int rc_time;    /// The last time the client receives data packets the time
    int st_time;    /// The last time the client sends the end data packet of the time
} bridge_t;

#endif
