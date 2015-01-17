#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
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

static struct sockaddr g_client_addr;
static socklen_t g_client_addrlen = 0;

static int g_listen_fd;

static int* g_packet_id = NULL;

/**
 * ev 处理线程
 */
void* ev_thread(void* ptr) {
    LOGD("开始 EV 处理线程\n");
    
    ev_run(g_ev_reactor, 0);
    
    LOGW("EV 处理线程退出了\n");
    exit(0);
}



int main(int argc, char** argv) {
    if (argc <= 1) {
        fprintf(stderr, "Usage: <%s> <config_file>\n", argv[0]);
        exit(-1);
    }
    
    
    g_listen_fd = net_bind("0.0.0.0", 3000, SOCK_DGRAM);
    if (g_listen_fd < 0) {
        LOGE("无法开始监听：%s\n", strerror(errno));
        exit(0);
    }
    LOGD("成功开始监听\n");
    
    pthread_t tid;
    pthread_create(&tid, NULL, client_thread, strdup(argv[1]));
    pthread_detach(tid);
    
    while (1) {
        sleep(100);
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
    
    static received_t *received = NULL;
    if (received == NULL) {
        received = malloc(sizeof(*received));
        received_init(received);
    }
    
    
    readb = recv(w->fd, buf, buflen, 0);
    if (readb < 0) {
        LOGW("远程桥可能断开了连接：%s\n", strerror(errno));
        free(buf);
        return;
    }
    else if (readb == 0) {
        LOGW("无法从远程桥接收数据，远程桥可能已经断开了连接\n");
        free(buf);
        return;
    }
    else {
        LOGD("从远程桥收取了 %d 字节数据：%s\n", readb, (char*)buf + sizeof(packet_t));
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
    if (received_is_received(received, c->id) != 0) {
        /// 已经收过包了
        LOGD("已经收取过 id=%d 的包了\n", c->id);
        free(c);
        return;
    }
    else {
        received_add(received, c->id);
        LOGD("成功收取 id=%d 的包\n", c->id);
    }
    
    received_try_dropdead(received, 30);
    
    
    int sendb;
    sendb = sendto(g_listen_fd, buf, c->buflen, MSG_DONTWAIT, &g_client_addr, g_client_addrlen);
    if (sendb < 0) {
        LOGW("无法向客户端发送数据：%s\n", strerror(errno));
    }
    else if (sendb == 0) {
        LOGW("客户端可能已经断开了连接\n");
    }
    else {
        LOGD("向客户端(:%u)发送了 %d 字节数据：%s\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), sendb, buf);
    }
    
    free(c);
    return;
}


/**
 * 初始化一个接收器 ev，用来处理收到的数据
 */
int init_recv_ev(int fd) {
    if (g_ev_reactor == NULL) {
        g_ev_reactor = ev_loop_new(EVFLAG_AUTO);
    }
    
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
        LOGI("成功连接到 %s:%d，fd 是 %d\n", host, port, fd);
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


int connect_to_servers(struct list_head *list, char* server_list_path) {
    int i;
    char host[1024];
    char line[1024];
    int port;
    FILE* fp;
    
    fp = fopen(server_list_path, "r");
    if (fp == NULL) {
        LOGE("无法读取配置文件`%s‘：%s\n", server_list_path, strerror(errno));
        exit(-1);
    }
    
    while (!feof(fp)) {
        memset(host, 0x00, sizeof(host));
        memset(line, 0x00, sizeof(line));
        port = 0;
        
        fgets(line, sizeof(line) - 1, fp);
        
        if (line[0] == 0x00 || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        
        sscanf(line, "%s %d\n", host, &port);
        
        LOGI("从配置文件中读取到目标服务器：%s:%d\n", host, port);
        connect_to_server(list, host, port);
    }
    
    fclose(fp);
    
    return 0;
}


/**
 * 将本地数据转发到桥的线程
 * 
 * @param void*     目标服务器配置文件路径
 */
void* client_thread(void* ptr) {
    int readb, sendb, buflen;
    char* buf;
    char server_list_path[1024] = {0};
    
    strncpy(server_list_path, (char*)ptr, sizeof(server_list_path) - 1);
    free(ptr);
    
    buflen = 65536;
    buf = malloc(buflen);
    
    
    /// 连接到服务器
    struct list_head connections = LIST_HEAD_INIT(connections);
    connect_to_servers(&connections, server_list_path);
    
    LOGD("初始化 EV 处理线程\n");
    pthread_t tid;
    pthread_create(&tid, NULL, ev_thread, NULL);
    pthread_detach(tid);
    
    
    while (1) {
        memset(buf, 0x00, sizeof(buflen));
        g_client_addrlen = sizeof(g_client_addr);
        readb = recvfrom(g_listen_fd, buf, buflen, 0, &g_client_addr, &g_client_addrlen);
        
        //LOGD("收到客户端(:%u)发来的数据(fd=%d): %s\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), g_listen_fd, buf);
        LOGD("收到客户端(:%u)发来的数据(fd=%d)\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), g_listen_fd);
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
            /// 收到了数据，将数据转发给桥
            struct list_head *pos;
            connections_t *c;
            
            if (g_packet_id == NULL) {
                g_packet_id = malloc(sizeof(*g_packet_id));
                *g_packet_id = 0;
            }
            (*g_packet_id)++;
            
            list_for_each(pos, &connections) {
                c = list_entry(pos, connections_t, list);
                
                sendb = packet_send(c->fd, buf, readb, *g_packet_id);
                if (sendb < 0) {
                    LOGW("无法向 %s:%d 发送 %d 字节数据: %s\n", c->host, c->port, buflen, strerror(errno));
                }
                else if (sendb == 0){ 
                    LOGW("%s:%d 可能断开了连接\n", c->host, c->port);
                }
                else {
                    //LOGD("向 %s:%d 发送了 %d 字节消息“%s”\n", c->host, c->port, sendb, buf);
                    LOGD("向 %s:%d 发送了 %d 字节消息\n", c->host, c->port, sendb);
                }
            }
        }
    }
    
    free(buf);
    return NULL;
}

