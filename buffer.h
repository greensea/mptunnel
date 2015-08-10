#ifndef __BUFFER_H__
#define __BUFFER_H__

#include <stdint.h>
#include <ev.h>

#include "linklist.h"

typedef struct buffers_t {
    struct list_head list;
    uint64_t id;
    char* buf;
    int buflen;
} buffers_t;


typedef struct connections_t {
    struct list_head list;
    int fd;
    char* host;
    int port;
    ev_io *watcher;
} connections_t;

#endif
