#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include "mptunnel.h"

static int* g_packet_id = NULL;

static int* g_received_id = NULL;

/**
 * 组装一个数据包，组装出来的包需要调用 packet_free 进行释放
 */
packet_t* packet_make(enum packet_type type, const char* buf, int buflen) {
    static int packet_id = 0;
    packet_t* p;
    
    if (g_packet_id == NULL) {
        g_packet_id = &packet_id;
    }
    
    p = (packet_t*)malloc(sizeof(*p) + buflen);
    memset(p, 0x00, sizeof(*p));
    p->type = type;
    p->id = ++packet_id;
    p->buflen = buflen;
    memcpy(p + 1, buf, buflen);
    
    return p;
}


int packet_free(packet_t* p) {
    free(p);
    
    return 0;
}
    
    
    
int packet_send(int fd, char* buf, int buflen) {
    int sendb;
    
    packet_t* p = packet_make(PKT_TYPE_DATA, buf, buflen);

    sendb = send(fd, p, sizeof(*p) + p->buflen, MSG_DONTWAIT);
    if (sendb < 0) {
        LOGW("无法向发送 %d 字节数据: %s\n",  buflen, strerror(errno));
    }
    else if (sendb == 0){ 
        LOGW("fd=%d 可能断开了连接\n", fd);
    }
    else {
        LOGD("向发送了 %d 字节消息“%s”\n", sendb, (char*)(p + 1));
    }
    
    packet_free(p);
    
    return sendb;
}


int packet_received(int _id) {
    if (g_received_id == NULL) {
        g_received_id = malloc(sizeof(int));
    }
    
    *g_received_id = _id;
    
    return 0;
}

/**
 * 判断一个包是否曾经接收过，如果接收过返回 1,否则返回 0
 */
int packet_is_received(int _id) {
    if (g_received_id == NULL) {
        return 0;
    }
    else {
        if (_id > *g_received_id) {
            return 0;
        }
        else {
            return 1;
        }
    }
}
