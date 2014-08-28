#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <ev.h>
#include "net.h"

#include "linklist.h"
#include "mptunnel.h"
#include "buffer.h"
#include "client.h"

static struct list_head g_buffers = LIST_HEAD_INIT(g_buffers);


int main(int argc, char** argv) {
    int ret, i, clientfd, listenfd;
    char errbuf[128] = {0};
    
    
    listenfd = net_bind("0.0.0.0", 3000, SOCK_DGRAM);
    if (listenfd < 0) {
        LOGE("无法开始监听：%s\n", strerror(errno));
        exit(0);
    }
    LOGD("成功开始监听\n");
    
    char clientip[16] = {0};
    while (1) {
        //clientfd = net_accept(listenfd, clientip);
        clientfd = listenfd;
        if (clientfd < 0) {
            LOGW("无法接受客户端连接请求(listenfd=%d): %s\n", listenfd, strerror(errno));
            continue;
        }
        
        pthread_t tid;
        int* ptr = malloc(sizeof(int));
        *ptr = clientfd;
        pthread_create(&tid, NULL, client_thread, ptr);
        pthread_detach(tid);
        
        while (1) {
            sleep(100);
        }
    }
    
    
    return 0;
}

int connect_to_server(struct list_head *list, char* host, int port) {
    int fd, ret;
    char errbuf[128];
    
    LOGD("connecting to %s:%d\n", host, port);
    
    fd = net_connect(host, port, SOCK_DGRAM);
    if (fd < 0) {
        LOGW("无法连接到 %s:%d: %s\n", host, port, strerror(errno));
        return -1;
    }
    else {
        LOGI("成功连接到 %s:%d\n", host, port);
    }
    
    connections_t* c = (connections_t*)malloc(sizeof(connections_t));
    memset(c, 0x00, sizeof(*c));
    c->host = strdup(host);
    c->port = port;
    c->fd = fd;
    
    list_add_tail(&c->list, list);
    
    return 0;
}


int connect_to_servers(struct list_head *list) {
    int i;
    char* hosts[] = {"nagisa.greensea.org", "kotomi.greensea.org", "azuna.greensea.org"};
    int ports[] = {3000, 3000, 3000};
    
    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        connect_to_server(list, hosts[i], ports[i]);
    }
}

void* client_thread(void* ptr) {
    int fd, readb, sendb, buflen;
    uint64_t id = 0;
    char* buf;
    
    buflen = 65536;
    
    fd = *(int*)ptr;
    free(ptr);
    
    buf = malloc(buflen);
    
    /// 连接到服务器
    struct list_head connections = LIST_HEAD_INIT(connections);
    connect_to_servers(&connections);
    
    
    while (1) {
        memset(buf, 0x00, sizeof(buflen));
        readb = recv(fd, buf, buflen, 0);
        if (readb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            else {
                LOGI("客户端断开了连接: %s\n", strerror(errno));
                break;
            }
        }
        else if (readb == 0) {
            LOGW("无法从客户端收取消息，客户端断开了连接\n");
            break;
        }
        else {
            /// 收到了数据，将数据转发出去
            struct list_head *pos;
            connections_t *c;
            list_for_each(pos, &connections) {
                c = list_entry(pos, connections_t, list);
                
                sendb = send(c->fd, buf, readb, MSG_DONTWAIT);
                if (sendb < 0) {
                    LOGW("无法向 %s:%d 发送 %d 字节数据: %s\n", c->host, c->port, buflen, strerror(errno));
                }
                else if (sendb == 0){ 
                    LOGW("%s:%d 可能断开了连接\n");
                }
                else {
                    LOGD("向 %s:%d 发送了 %d 字节消息“%s”\n", c->host, c->port, sendb, buf);
                }
            }
        }
    }
    
    free(buf);
    return NULL;
}

