#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>
#include <ev.h>
#include "net.h"

#include "linklist.h"
#include "rbtree.h"

#include "mptunnel.h"
#include "buffer.h"
#include "client.h"

static struct ev_loop * g_ev_reactor = NULL;

static struct list_head g_buffers = LIST_HEAD_INIT(g_buffers);
//static struct rb_root g_received = RB_ROOT;

static int g_listen_fd;


/**
 * ev 处理线程
 */
void* ev_thread(void* ptr) {
    LOGD("开始 EV 处理线程\n");
    
    g_ev_reactor = ev_loop_new(EVFLAG_AUTO);
    while (1) {      
        ev_run(g_ev_reactor, 0);
    }
    
    LOGW("EV 处理线程退出\n");
}



int main(int argc, char** argv) {
    int clientfd, listenfd;
    
    LOGD("初始化 EV 处理线程\n");
    pthread_t tid;
    pthread_create(&tid, NULL, ev_thread, NULL);
    pthread_detach(tid);
    
    listenfd = net_bind("0.0.0.0", 3000, SOCK_DGRAM);
    if (listenfd < 0) {
        LOGE("无法开始监听：%s\n", strerror(errno));
        exit(0);
    }
    g_listen_fd = listenfd;
    LOGD("成功开始监听\n");
    
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


/**
 * 收到远程桥发来的数据时的处理函数
 */
void recv_remote_callback(struct ev_loop* reactor, ev_io* w, int events) {
    char* buf;
    int buflen = 65536;
    int readb;
    
    buf = malloc(buflen);
    memset(buf, 0x00, buflen);
    
    LOGD("收到从 %d 发来的数据\n", w->fd);
    
    readb = recv(w->fd, buf, buflen, 0);
    if (readb < 0) {
        LOGW("客户端可能断开了连接：%s\n", strerror(errno));
        free(buf);
        return;
    }
    else if (readb == 0) {
        LOGW("无法从客户端接收数据，客户端可能已经断开了连接\n");
        free(buf);
        return;
    }
    else {
        LOGD("从客户端收取了 %d 字节数据：%s\n", readb, (char*)buf + sizeof(packet_t));
    }
    
    packet_t* c;
    c = (packet_t*)buf;
    buf = buf + sizeof(packet_t);
    
    if (c->type == PKT_TYPE_CTL) {
        LOGD("收到控制包数据，丢弃\n");
        free(c);
        return;
    }
    else if (c->type != PKT_TYPE_DATA) {
        free(c);
        LOGE("数据包类型错误, type=%d\n", c->type);
        return;
    }
    
    LOGD("从远程桥收到 %d 字节数据(fd=%d): %s\n", c->buflen, w->fd, buf);
    
    /// 简单地丢弃已经收过的包
    packet_received(c->id);
    if (packet_is_received(c->id) != 0) {
        /// 已经收过包了
        LOGD("已经收取过 id=%d 的包了\n", c->id);
        free(c);
        return;
    }
    
    int sendb;
    sendb = send(g_listen_fd, buf, c->buflen, MSG_DONTWAIT);
    if (sendb < 0) {
        LOGW("无法向客户端发送数据：%s\n", strerror(errno));
    }
    else if (sendb == 0) {
        LOGW("客户端可能已经断开了连接\n");
    }
    else {
        LOGD("向客户端发送了 %d 字节数据：%s\n", c->buflen, buf);
    }
    
    free(c);
    return;
}


/**
 * 初始化一个接收器 ev，用来处理收到的数据
 */
int init_recv_ev(int fd) {
    ev_io *watcher = (ev_io*)malloc(sizeof(ev_io));
    memset(watcher, 0x00, sizeof(*watcher));
    
    ev_io_init(watcher, recv_remote_callback, fd, EV_READ);
    ev_io_start(g_ev_reactor, watcher);
    
    return 0;
}




int connect_to_server(struct list_head *list, char* host, int port) {
    int fd;
    
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
    
    init_recv_ev(c->fd);
    
    return 0;
}


int connect_to_servers(struct list_head *list) {
    int i;
    char* hosts[] = {"nagisa.greensea.org", "kotomi.greensea.org", "azuna.greensea.org"};
    int ports[] = {3000, 3000, 3000};
    
    for (i = 0; i < sizeof(ports) / sizeof(ports[0]); i++) {
        connect_to_server(list, hosts[i], ports[i]);
    }
    
    return 0;
}

void* client_thread(void* ptr) {
    int fd, readb, sendb, buflen;
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
                
                sendb = packet_send(c->fd, buf, readb);
                if (sendb < 0) {
                    LOGW("无法向 %s:%d 发送 %d 字节数据: %s\n", c->host, c->port, buflen, strerror(errno));
                }
                else if (sendb == 0){ 
                    LOGW("%s:%d 可能断开了连接\n", c->host, c->port);
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

