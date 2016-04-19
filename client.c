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

static struct {
    ev_async w;
    int fd;
} ev_async_reset_conn;

extern int g_config_encrypt;



/**
 * 用于保护 ev_io 的锁
 * 在调用 reconnect_to_server 时，我们需要销毁旧的 ev_io，但此时对应的 ev_io 可能正在其回调处理函数(recv_remote_callback)中，
 * 如果这时候 free 掉 ev_io，就会出现内存访问错误，因此，我们使用此锁来保护，不要在 recv_remote_callback 正在执行的时候删除 ev_io
 */
static pthread_mutex_t g_ev_io_w_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * ev 处理线程
 */
void* ev_thread(void* ptr) {
    LOGD(_("Starting libev thread\n"));
    
    ev_run(g_ev_reactor, 0);
    
    LOGW(_("libev thread exited\n"));
    exit(0);
}



int main(int argc, char** argv) {
    setlocale(LC_ALL, "");
    bindtextdomain("mptunnel", "locale");
    textdomain("mptunnel");
    
    if (argc <= 1) {
        fprintf(stderr, _("Usage: <%s> <config_file>\n"), argv[0]);
        fprintf(stderr, _("To disable encryption, set environment variable MPTUNNEL_ENCRYPT=0\n"));
        exit(-1);
    }
    
            
    if (getenv("MPTUNNEL_ENCRYPT") == NULL) {
        g_config_encrypt = 1;
    }
    else if(atoi(getenv("MPTUNNEL_ENCRYPT")) == 0) {
        g_config_encrypt = 0;
    }
    else {
        g_config_encrypt = 1;
    }
    
    
    LOGD(_("Configuration: Encryption %s\n"), (g_config_encrypt) ? _("enabled") : _("disabled"));

    
    
    g_listen_fd = net_bind("0.0.0.0", 3210, SOCK_DGRAM);
    if (g_listen_fd < 0) {
        LOGE(_("Can't listen on：%s\n"), strerror(errno));
        exit(0);
    }
    LOGD(_("Port listening started\n"));
    
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
    connections_t *conn = NULL;
    
    
    buf = malloc(buflen);
    //memset(buf, 0x00, buflen);    /// 为了提升效率，不再初始化接收缓存
    
    /// 查找对应的连接对象
    struct list_head *pos;
    list_for_each(pos, &g_connections) {
        conn = list_entry(pos, connections_t, list);
        if (conn->fd == w->fd) {
            break;
        }
    }
    if (conn == NULL) {
        LOGE(_("Connection of fd=%d is not exists in Connection List\n"), w->fd);
    }
    
    
    static received_t *received = NULL;
    if (received == NULL) {
        received = malloc(sizeof(*received));
        received_init(received);
    }
    
    pthread_mutex_lock(&g_ev_io_w_mutex);
    
    
    readb = recv(w->fd, buf, buflen, MSG_DONTWAIT);
    if (readb < 0) {
        //LOGW("接收数据时出错，远程桥 %s:%d （fd=%d）可能断开了连接(errno=%d)：%s\n", conn->host, conn->port, w->fd, errno, strerror(errno));
        LOGW(_("Error while receive data, remote bridge %s:%d (fd=%d) may close the connection(errno=%d): %s\n"), conn->host, conn->port, w->fd, errno, strerror(errno));
        free(buf);
        
        /// 实验性修改：EWOULDBLOCK 也断开，因为长期没有收到数据包时，我们需要主动断开链接，这时另一个线程会给这个回调喂一个事件，但此时系统认为连接还是正常的，所以会返回 EWOULDBLOCK
        //if ((errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || conn->broken == 1) {
        if (errno != EINTR) {
            struct list_head *pos;
            
            /// 仅当链接断开的时候才会发生 EWOULDBLOCK 事件
            if ((errno == EWOULDBLOCK || errno == EAGAIN) && conn->broken == 1) {
                LOGI(_("The connection is marked broken, try restart connection to %s:%d(fd=%d),errno=%d: %s\n"), conn->host, conn->port, w->fd, errno, strerror(errno));
                //LOGI("因为连接被标记为断开的，尝试重新启动到远程桥 %s:%d (fd=%d)的连接, errno=%d: %s\n", conn->host, conn->port, w->fd, errno, strerror(errno));
            }
            else {
                LOGI(_("Network broken, try to restart connection to %s:%d(fd=%d), errno=%d: %s\n"), conn->host, conn->port, w->fd, errno, strerror(errno));
                //LOGI("因为网络中断，尝试重新启动到远程桥 %s:%d (fd=%d)的连接, errno=%d: %s\n", conn->host, conn->port, w->fd, errno, strerror(errno));
            }
            
            list_for_each(pos, &g_connections) {
                conn = list_entry(pos, connections_t, list);
                if (conn->fd == w->fd) {
                    LOGI("About to re-establish connection to %s:%d\n", conn->host, conn->port);
                    //LOGI("将要重新启动到远程桥 %s:%d 的连接\n", conn->host, conn->port);
                    reconnect_to_server(conn);
                    break;
                }
            }
        }
        
        goto cleanup_return;
    }
    else if (readb == 0) {
        //LOGW("无法从远程桥（%d）接收数据(readb=0)，远程桥可能已经断开了连接: %s\n", w->fd, strerror(errno));
        LOGW(_("Can't received data from remote bridge(%d), remote bridge may close the connection (readb=0): %s\n"), w->fd, strerror(errno));
        free(buf);
        goto cleanup_return;
    }
    else {
        //LOGD("从远程桥（%d）收取了 %d 字节数据\n", w->fd, readb);
        struct list_head *pos;
        
            
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
        //LOGD("收到来自 %s:%d 的控制类型数据包(fd=%d)，丢弃，数据包编号为 %d\n", conn->host, conn->port, w->fd, c->id);
        LOGD(_("Receive control packet from %s:%d (fd=%d), packet ID is %d, drop it.\n"), conn->host, conn->port, w->fd, c->id);
        free(c);
        goto cleanup_return;
    }
    else if (c->type == PKT_TYPE_NONE) {
        //LOGD("收到来自 %s:%d 的无类型数据包，这应该是网络出现了问题(fd=%d)，丢弃，数据包编号为 %d\n", conn->host, conn->port, w->fd, c->id);
        LOGD(_("Received unknown type packet from %s:%d (fd=%d), may cause by bad network, packet ID is %d, drop it."), conn->host, conn->port, w->fd, c->id);
        free(c);
        goto cleanup_return;
    }
    else if (c->type != PKT_TYPE_DATA) {
        LOGE(_("Invalid packet type, type=%d, id=%d\n"), c->type, c->id);
        free(c);
        goto cleanup_return;
    }
    
    
    
    /// 简单地丢弃已经收过的包
    if (received_is_received(received, c->id) != 0) {
        /// 已经收过包了
        //LOGD("从远程桥 %s:%d （fd=%d）收到长度为 %d 的曾经收取过的编号为 %d 的包，丢弃之\n", conn->host, conn->port, w->fd, c->buflen, c->id);
        LOGD(_("Received packet from remote bridge %s:%d (fd=%d) of %d bytes, packet ID is %d, drop it.\n"), conn->host, conn->port, w->fd, c->buflen, c->id);
        free(c);
        goto cleanup_return;
    }
    else {
        received_add(received, c->id);
        //LOGD("从远程桥 %s:%d （fd=%d）收到 %d 字节的数据包，包编号 %d，原始包长度 %d, 载荷长度 %d\n", conn->host, conn->port, w->fd, c->buflen, c->id, readb, c->buflen);
        LOGD(_("Received packet from remote bridge %s:%d (fd=%d) of %d bytes, packet ID is %d, raw lenth %d bytes, payload length %d bytes\n"), conn->host, conn->port, w->fd, c->buflen, c->id, readb, c->buflen);
    }
    
    received_try_dropdead(received, 30);
    
    
    
    /// 将收到的数据包转发到客户端
    int sendb;
    sendb = sendto(g_listen_fd, buf, c->buflen, MSG_DONTWAIT, &g_client_addr, g_client_addrlen);
    if (sendb < 0) {
        //LOGW("无法向客户端转发编号为 %d 的数据包：%s\n", c->id, strerror(errno));
        LOGW(_("Can not forward packet #%d to client: %s\n"), c->id, strerror(errno));
    }
    else if (sendb == 0) {
        //LOGW("客户端可能已经断开了连接，无法转发编号为 %d 的数据包\n", c->id);
        LOGW(_("Client might close the connection, packet #%d can not be forwarded\n"), c->id);
    }
    else {
        //LOGD("向客户端(端口 %u)转发了 %d 字节长度的编号为 %d 的数据包\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), sendb, c->id);
        LOGD(_("Forwarded packet to client(port %u) of %d bytes, ID is %d\n"), ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), sendb, c->id);
    }
    
    free(c);
    
cleanup_return:
    pthread_mutex_unlock(&g_ev_io_w_mutex);
    
    return;
}


/**
 * 用于异线程发送 EV 事件，通知 client_thread 有 fd 事件（连接断开）发生的函数
 */
void restart_conn_signal(struct ev_loop *reactor, ev_async *w, int events) {
    ev_feed_fd_event(g_ev_reactor, ev_async_reset_conn.fd, EV_READ);
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
    
    ev_async_init(&ev_async_reset_conn.w, restart_conn_signal);
    ev_async_start(g_ev_reactor, &ev_async_reset_conn.w);
    
    return watcher;
}


/**
 * 删除一个已经初始化好的接收器 ev
 */
int destroy_recv_ev(ev_io *watcher) {
    assert(g_ev_reactor != NULL);
    
    ev_io_stop(g_ev_reactor, watcher);
    //free(watcher);
    
    return 0;
}


connections_t* connect_to_server(char* host, int port) {
    int fd;
    ev_io *watcher = NULL;
    
    LOGD(_("Connect to %s:%d\n"), host, port);
    
    fd = net_connect(host, port, SOCK_DGRAM);
    if (fd < 0) {
        LOGW(_("Can't connect to %s:%d: %s\n"), host, port, strerror(errno));
        return NULL;
    }
    else {
        LOGI(_("Connected to remote host %s:%d, fd is %d\n"), host, port, fd);
    }
    
    connections_t* c = (connections_t*)malloc(sizeof(connections_t));
    memset(c, 0x00, sizeof(*c));
    c->host = strdup(host);
    c->port = port;
    c->fd = fd;
    c->st_time = 0;
    c->rc_time = 0;
    c->broken = 0;
    
    watcher = init_recv_ev(c->fd);
    if (watcher == NULL) {
        LOGE(_("watcher initalize failed\n"));
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
        LOGE(_("Error while reading configura file `%s‘：%s\n"), server_list_path, strerror(errno));
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
        
        LOGI(_("Get remote server address %s:%d from configura file\n"), host, port);
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
    
    
    LOGD(_("Reconnected to %s:%d, fd %d -> %d"), c->host, c->port, c->fd, newc->fd);
    
    /// host 和 port 是相同的，所以不用复制
    c->fd = newc->fd;
    c->watcher = newc->watcher;
    c->st_time = 0;
    c->rc_time = 0;
    c->broken = 0;
    
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
    
    LOGD(_("Initializing libev thread\n"));
    pthread_t tid;
    pthread_create(&tid, NULL, ev_thread, NULL);
    pthread_detach(tid);
    
    
    while (1) {
        memset(buf, 0x00, sizeof(buflen));
        g_client_addrlen = sizeof(g_client_addr);
        readb = recvfrom(g_listen_fd, buf, buflen, 0, &g_client_addr, &g_client_addrlen);
        
        //LOGD("收到客户端(:%u)发来的数据(fd=%d): %s\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), g_listen_fd, buf);
        LOGD(_("Received data from client(:%u), fd=%d\n"), ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), g_listen_fd);
        if (readb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            else {
                LOGI(_("Client close the connection: %s\n"), strerror(errno));
                break;
            }
        }
        else if (readb == 0) {
            LOGW(_("Can't received from client, client close the connection\n"));
            break;
        }
        else {
            /// 收到了数据，将数据转发给桥
            struct list_head *pos;
            connections_t *c, *c_broken = NULL;
            
            
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
                    //LOGD("到 %s:%d 的连接在最后一次发包后已经超过 %d 秒没有收到桥端的数据了，认为连接断开，即将重连\n", c->host, c->port, c->st_time - c->rc_time);
                    LOGD(_("No packet received from %s:%d since last packet sent to it (%d seconds), assume connection broken, about to reconnect\n"), c->host, c->port, c->st_time - c->rc_time);
                    //reconnect_to_server(c);
                    c_broken = c;
                }
                c->st_time = time(NULL);
                
                /// 1.2 初始化最后一次收到服务器包的时间，以便进行超时判断
                if (c->rc_time == 0) {
                    c->rc_time = time(NULL);
                }
                
                
                /// 2. 发送数据
                sendb = packet_send(c->fd, buf, readb, *g_packet_id);
                if (sendb < 0) {
                    //LOGW("无法向 %s:%d(%d) 发送 %d 字节数据: %s\n", c->host, c->port, c->fd, readb, strerror(errno));
                    LOGW(_("Error while sending data to %s:%d(%d) with %d bytes: %s\n"), c->host, c->port, c->fd, readb, strerror(errno));
                    
                    if (errno == EINVAL || errno == ECONNREFUSED) {
                        LOGD(_("About to reconnect to %s:%d (err: %s)\n"), c->host, c->port, strerror(errno));
                        //reconnect_to_server(c);
                        c_broken = c;
                    }
                }
                else if (sendb == 0){ 
                    LOGW(_("%s:%d(%d) may close the connection\n"), c->host, c->port, c->fd);
                }
                else {
                    //LOGD("向 %s:%d 发送了 %d 字节消息“%s”\n", c->host, c->port, sendb, buf);
                    LOGD(_("Packet sent to %s:%d(%d) of %d bytes.\n"), c->host, c->port, c->fd, sendb);
                }
            }
            
            
            /// 检查是否有失效的连接
            if (c_broken != NULL) {
                LOGI(_("Connection to %s:%d is broken, try reconnect\n"), c_broken->host, c_broken->port);
              //LOGD("不会在该线程中进行重连操作, 才怪，不在这里重连的话似乎一直不会重连");
              //LOGD("不会在该线程(%s)中进行重连操作，但会调用 ev_async_send(c->fd=%d) 以便通知另一线程调用 ev_feed_fd_event，以执行此操作\n", __FUNCTION__, c_broken->fd);
              LOGD(_("I will not perform reconnect in current thread(%s), I will call ev_async_send(c->fd=%d) to ask another thread to perform the reconnect\n"), __FUNCTION__, c_broken->fd);
              c_broken->broken = 1;
              ev_async_reset_conn.fd = c_broken->fd;
              ev_async_send(g_ev_reactor, &ev_async_reset_conn.w);
              //reconnect_to_server(c_broken);
            }
        }
    }
    
    free(buf);
    return NULL;
}

