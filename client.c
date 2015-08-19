#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
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

static struct list_head g_connections = LIST_HEAD_INIT(g_connections);

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
    
    
    g_listen_fd = net_bind("0.0.0.0", 3210, SOCK_DGRAM);
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
    //memset(buf, 0x00, buflen);    /// 为了提升效率，不再初始化接收缓存
    
    //LOGD("收到从 %d 发来的数据\n", w->fd);
    
    static received_t *received = NULL;
    if (received == NULL) {
        received = malloc(sizeof(*received));
        received_init(received);
    }
    
    
    readb = recv(w->fd, buf, buflen, 0);
    if (readb < 0) {
        LOGW("接收数据时出错，远程桥（%d）可能断开了连接：%s\n", w->fd, strerror(errno));
        free(buf);
        
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            struct list_head *pos;
            connections_t *conn;
            
            LOGI("尝试重新启动到远程桥(%d)的连接\n", w->fd);
            
            list_for_each(pos, &g_connections) {
                conn = list_entry(pos, connections_t, list);
                if (conn->fd == w->fd) {
                    LOGI("将要重新启动到远程桥 %s:%d 的连接\n", conn->host, conn->port);
                    reconnect_to_server(conn);
                    break;
                }
            }
        }
        
        return;
    }
    else if (readb == 0) {
        LOGW("无法从远程桥（%d）接收数据，远程桥可能已经断开了连接\n", w->fd);
        free(buf);
        return;
    }
    else {
        //LOGD("从远程桥（%d）收取了 %d 字节数据\n", w->fd, readb);
        struct list_head *pos;
        connections_t *conn;
            
        list_for_each(pos, &g_connections) {
            conn = list_entry(pos, connections_t, list);
            if (conn->fd == w->fd) {
                conn->rc_time = time(NULL);
                break;
            }
        }
    }
    
    packet_t* c;
    
    mpdecrypt(buf);
    
    c = (packet_t*)buf;
    buf = buf + sizeof(packet_t);
    
    
    if (c->type == PKT_TYPE_CTL) {
        LOGD("收到控制包数据，丢弃，数据包编号为 %d\n", c->id);
        free(c);
        return;
    }
    else if (c->type != PKT_TYPE_DATA) {
        LOGE("数据包类型错误, type=%d, id=%d\n", c->type, c->id);
        free(c);
        return;
    }
    
    
    
    /// 简单地丢弃已经收过的包
    if (received_is_received(received, c->id) != 0) {
        /// 已经收过包了
        LOGD("从远程桥（%d）收到长度为 %d 的曾经收取过的编号为 %d 的包，丢弃之\n", w->fd, c->buflen, c->id);
        free(c);
        return;
    }
    else {
        received_add(received, c->id);
        LOGD("从远程桥（%d）收到 %d 字节的数据包，包编号 %d，原始包长度 %d, 载荷长度 %d\n", w->fd, c->buflen, c->id, readb, c->buflen);
    }
    
    received_try_dropdead(received, 30);
    
    
    
    /// 将收到的数据包转发到客户端
    int sendb;
    sendb = sendto(g_listen_fd, buf, c->buflen, MSG_DONTWAIT, &g_client_addr, g_client_addrlen);
    if (sendb < 0) {
        LOGW("无法向客户端转发编号为 %d 的数据包：%s\n", c->id, strerror(errno));
    }
    else if (sendb == 0) {
        LOGW("客户端可能已经断开了连接，无法转发编号为 %d 的数据包\n", c->id);
    }
    else {
        LOGD("向客户端(端口 %u)转发了 %d 字节长度的编号为 %d 的数据包\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), sendb, c->id);
    }
    
    free(c);
    return;
}


/**
 * 初始化一个接收器 ev，用来处理收到的数据
 */
ev_io* init_recv_ev(int fd) {
    if (g_ev_reactor == NULL) {
        g_ev_reactor = ev_loop_new(EVFLAG_AUTO);
    }
    
    ev_io *watcher = (ev_io*)malloc(sizeof(ev_io));
    memset(watcher, 0x00, sizeof(*watcher));
    
    ev_io_init(watcher, recv_remote_callback, fd, EV_READ);
    ev_io_start(g_ev_reactor, watcher);
    
    return watcher;
}


/**
 * 删除一个已经初始化好的接收器 ev
 */
int destroy_recv_ev(ev_io *watcher) {
    assert(g_ev_reactor != NULL);
    
    ev_io_stop(g_ev_reactor, watcher);
    free(watcher);
    
    return 0;
}


connections_t* connect_to_server(char* host, int port) {
    int fd;
    ev_io *watcher = NULL;
    
    LOGD("正在连接到远程主机 %s:%d\n", host, port);
    
    fd = net_connect(host, port, SOCK_DGRAM);
    if (fd < 0) {
        LOGW("无法连接到远程主机 %s:%d: %s\n", host, port, strerror(errno));
        return NULL;
    }
    else {
        LOGI("成功连接到远程主机 %s:%d，fd 是 %d\n", host, port, fd);
    }
    
    connections_t* c = (connections_t*)malloc(sizeof(connections_t));
    memset(c, 0x00, sizeof(*c));
    c->host = strdup(host);
    c->port = port;
    c->fd = fd;
    c->st_time = 0;
    c->rc_time = 0;
    
    
    watcher = init_recv_ev(c->fd);
    if (watcher == NULL) {
        LOGE("无法初始化 watcher\n");
    }
    c->watcher = watcher;
    
    return c;
}


int connect_to_servers(struct list_head *list, char* server_list_path) {
    char host[1024];
    char line[1024];
    int port;
    FILE* fp;
    connections_t *conn;
    
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
        conn = connect_to_server(host, port);
        
        list_add_tail(&conn->list, list);
    }
    
    fclose(fp);
    
    return 0;
}


/**
 * 重新连接到链表中指定的服务器
 */
int reconnect_to_server(connections_t *c) {
    connections_t* newc = NULL;
    
    assert(c != NULL);
    
    destroy_recv_ev(c->watcher);
    close(c->fd);
    
    newc = connect_to_server(c->host, c->port);
    
    
    LOGD("重新连接到了 %s:%d，fd 由 %d 替换为 %d\n", c->host, c->port, c->fd, newc->fd);
    
    /// host 和 port 是相同的，所以不用复制
    c->fd = newc->fd;
    c->watcher = newc->watcher;
    c->st_time = 0;
    c->rc_time = 0;
    
    free(newc->host);
    free(newc);
    
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
    connect_to_servers(&g_connections, server_list_path);
    
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
            
            list_for_each(pos, &g_connections) {
                c = list_entry(pos, connections_t, list);
                
                /// 1. 检查连接是否超时
                /// 1.1 如果最后一次收到服务器包的时间已经初始化，则检查连接是否超时
                if (c->rc_time != 0 && c->st_time - c->rc_time > CLIENT_BRIDGE_TIMEOUT) {
                    LOGD("到 %s:%d 的连接在最后一次发包后已经超过 %d 秒没有收到桥端的数据了，认为连接断开，即将重连\n", c->host, c->port, c->st_time - c->rc_time);
                    reconnect_to_server(c);
                }
                c->st_time = time(NULL);
                
                /// 1.2 初始化最后一次收到服务器包的时间，以便进行超时判断
                if (c->rc_time == 0) {
                    c->rc_time = time(NULL);
                }
                
                
                /// 2. 发送数据
                sendb = packet_send(c->fd, buf, readb, *g_packet_id);
                if (sendb < 0) {
                    LOGW("无法向 %s:%d 发送 %d 字节数据: %s\n", c->host, c->port, buflen, strerror(errno));
                    
                    if (errno == EINVAL || errno == ECONNREFUSED) {
                        LOGD("重新启动到 %s:%d 的连接\n", c->host, c->port);
                        reconnect_to_server(c);
                    }
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

