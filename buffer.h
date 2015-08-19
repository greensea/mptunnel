#ifndef __BUFFER_H__
#define __BUFFER_H__

#include <stdint.h>

#include "linklist.h"

typedef struct buffers_t {
    struct list_head list;
    uint64_t id;
    char* buf;
    int buflen;
} buffers_t;



#endif
