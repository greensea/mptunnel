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
* For the protection of ev_io lock
* In call reconnect_to_server, we need to destroy the old ev_io, but in this case the corresponding ev_io may be its callback processing function(recv_remote_callback),
* If this time free off ev_io, there will be memory access errors, and therefore, we use this lock to protect, not in the recv_remote_callback being executed when the Delete ev_io
*/
static pthread_mutex_t g_ev_io_w_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
* ev processing thread
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
* Receive remote Bridge is sent to the data processing function
*/
void recv_remote_callback(struct ev_loop* reactor, ev_io* w, int events) {
    char* buf;
    int buflen = 65536;
    int readb;
    connections_t *conn = NULL;
    
    
    buf = malloc(buflen);
    //memset(buf, 0x00, buflen); /// in order to enhance the efficiency, no longer initialize the receive buffer
    
    /// Find the corresponding connection object
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
        
        /// Experimental modifications: EWOULDBLOCK is also turned off, because long-term did not receive a packet, we need to take the initiative to disconnect the link, then another thread will give this a callback feed of an event, but in this case the system considers the connection is still normal, it will return EWOULDBLOCK
        //if ((errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || conn->broken == 1) {
        if (errno != EINTR) {
            struct list_head *pos;
            
            /// Only when the link is disconnected when it will happen EWOULDBLOCK event
            if ((errno == EWOULDBLOCK || errno == EAGAIN) && conn->broken == 1) {
                LOGI(_("The connection is marked broken, try restart connection to %s:%d(fd=%d),errno=%d: %s\n"), conn->host, conn->port, w->fd, errno, strerror(errno));
                //LOGI("because the connection is marked as disconnected, try to restart to the remote Bridge %s:%d (fd=%d), errno=%d: %s\n", conn->host, conn->port, w->fd, errno, strerror(errno));
            }
            else {
                LOGI(_("Network broken, try to restart connection to %s:%d(fd=%d), errno=%d: %s\n"), conn->host, conn->port, w->fd, errno, strerror(errno));
                //LOGI("because the network interruption, try to restart to the remote Bridge %s:%d (fd=%d), errno=%d: %s\n", conn->host, conn->port, w->fd, errno, strerror(errno));
            }
            
            list_for_each(pos, &g_connections) {
                conn = list_entry(pos, connections_t, list);
                if (conn->fd == w->fd) {
                    LOGI("About to re-establish connection to %s:%d\n", conn->host, conn->port);
                    //LOGI("will restart to the remote Bridge %s:%d connected\n", conn->host, conn->port);
                    reconnect_to_server(conn);
                    break;
                }
            }
        }
        
        goto cleanup_return;
    }
    else if (readb == 0) {
        //LOGW("unable to remote from Bridge%d receive data(readb=0), the remote Bridge may have been disconnected: %s\n", w->fd, strerror(errno));
        LOGW(_("Can't received data from remote bridge(%d), remote bridge may close the connection (readb=0): %s\n"), w->fd, strerror(errno));
        free(buf);
        goto cleanup_return;
    }
    else {
        //LOGD("from a remote Bridge%d received %d bytes of data\n", w->fd, readb);
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
        //LOGD("received from %s:%d the control type data packet(fd=%d), discarding that data packet number is %d\n", conn->host, conn->port, w->fd, c->id);
        LOGD(_("Receive control packet from %s:%d (fd=%d), packet ID is %d, drop it.\n"), conn->host, conn->port, w->fd, c->id);
        free(c);
        goto cleanup_return;
    }
    else if (c->type == PKT_TYPE_NONE) {
        //LOGD("received from %s:%d non-type data packet, which should be a network problem(fd=%d), discarding that data packet number is %d\n", conn->host, conn->port, w->fd, c->id);
        LOGD(_("Received unknown type packet from %s:%d (fd=%d), may cause by bad network, packet ID is %d, drop it."), conn->host, conn->port, w->fd, c->id);
        free(c);
        goto cleanup_return;
    }
    else if (c->type != PKT_TYPE_DATA) {
        LOGE(_("Invalid packet type, type=%d, id=%d\n"), c->type, c->id);
        free(c);
        goto cleanup_return;
    }
    
    
    
    /// Simply discard already received packet
    if (received_is_received(received, c->id) != 0) {
        /// Has been received through the packet.
        //LOGD("from a remote Bridge %s:%d fd=%d, received length is %d ever charged over the number is %d packet, discarding it\n", conn->host, conn->port, w->fd, c->buflen, c->id);
        LOGD(_("Received packet from remote bridge %s:%d (fd=%d) of %d bytes, packet ID is %d, drop it.\n"), conn->host, conn->port, w->fd, c->buflen, c->id);
        free(c);
        goto cleanup_return;
    }
    else {
        received_add(received, c->id);
        //LOGD("from a remote Bridge %s:%d fd=%d, received %d bytes of packet, packet number %d, the original packet length %d payload length %d\n", conn->host, conn->port, w->fd, c->buflen, c->id, readb, c->buflen);
        LOGD(_("Received packet from remote bridge %s:%d (fd=%d) of %d bytes, packet ID is %d, raw lenth %d bytes, payload length %d bytes\n"), conn->host, conn->port, w->fd, c->buflen, c->id, readb, c->buflen);
    }
    
    received_try_dropdead(received, 30);
    
    
    
    /// The received data packet is forwarded to the client
    int sendb;
    sendb = sendto(g_listen_fd, buf, c->buflen, MSG_DONTWAIT, &g_client_addr, g_client_addrlen);
    if (sendb < 0) {
        //LOGW("unable to client forwarding number is %d packet:%s\n", c->id, strerror(errno));
        LOGW(_("Can not forward packet #%d to client: %s\n"), c->id, strerror(errno));
    }
    else if (sendb == 0) {
        //LOGW("the client may have been disconnected, not the forwarding number is %d packet\n", c->id);
        LOGW(_("Client might close the connection, packet #%d can not be forwarded\n"), c->id);
    }
    else {
        //LOGD("to client(port %u), forwarding %d bytes to the length of the number is %d packet\n", ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), sendb, c->id);
        LOGD(_("Forwarded packet to client(port %u) of %d bytes, ID is %d\n"), ntohs(((struct sockaddr_in*)&g_client_addr)->sin_port), sendb, c->id);
    }
    
    free(c);
    
cleanup_return:
    pthread_mutex_unlock(&g_ev_io_w_mutex);
    
    return;
}


/**
* For ISO-thread to send an EV event, the notification client_thread have the fd event that disconnect occurs the function
*/
void restart_conn_signal(struct ev_loop *reactor, ev_async *w, int events) {
    ev_feed_fd_event(g_ev_reactor, ev_async_reset_conn.fd, EV_READ);
}




/**
* Initialize a receiver ev, is used to process the received data
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
* To delete an already initialized the good of the receiver ev
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
* Re-connection to the list specified in the server
*/
int reconnect_to_server(connections_t *c) {
    connections_t* newc = NULL;
    
    assert(c != NULL);
    
    destroy_recv_ev(c->watcher);
    close(c->fd);
    
    newc = connect_to_server(c->host, c->port);
    
    
    LOGD(_("Reconnected to %s:%d, fd %d -> %d"), c->host, c->port, c->fd, newc->fd);
    
    /// host and port are the same, so don't copy
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
* The local data is forwarded to the bridge thread
* 
* @param void* target server configuration file path
*/
void* client_thread(void* ptr) {
    int readb, sendb, buflen;
    char* buf;
    char server_list_path[1024] = {0};
    
    strncpy(server_list_path, (char*)ptr, sizeof(server_list_path) - 1);
    free(ptr);
    
    buflen = 65536;
    buf = malloc(buflen);
    
    
    /// Connect to the server
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
            /// Received a data, the data is forwarded to the bridge
            struct list_head *pos;
            connections_t *c, *c_broken = NULL;
            
            
            if (g_packet_id == NULL) {
                g_packet_id = malloc(sizeof(*g_packet_id));
                *g_packet_id = 0;
            }
            (*g_packet_id)++;
            
            list_for_each(pos, &g_connections) {
                c = list_entry(pos, connections_t, list);
                
                /// 1. Check whether the connection is timeout
                /// 1.1 if the last received server time has been initialized, it is checked whether the connection timeout
                if (c->rc_time != 0 && c->st_time - c->rc_time > CLIENT_BRIDGE_TIMEOUT) {
                    //LOGD("to %s:%d connection in the last contract has more than %d seconds have not received the bridge end of the data, that the connection is disconnected, the upcoming re-connect\n", c->host, c->port, c->st_time - c->rc_time);
                    LOGD(_("No packet received from %s:%d since last packet sent to it (%d seconds), assume connection broken, about to reconnect\n"), c->host, c->port, c->st_time - c->rc_time);
                    //reconnect_to_server(c);
                    c_broken = c;
                }
                c->st_time = time(NULL);
                
                /// 1.2 initialize the last time the server receives packets time, for timeout judgment
                if (c->rc_time == 0) {
                    c->rc_time = time(NULL);
                }
                
                
                /// 2. Send data
                sendb = packet_send(c->fd, buf, readb, *g_packet_id);
                if (sendb < 0) {
                    //LOGW("无法向 %s:%d(%d) 发送 %of 字节数据: %s\n", c->host, c->port, c->fd, read, strerror(errno));
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
                    //LOGD("向 %s:%d 发送了 %of 字节消息“%s”\n", c->host, c->port, sendb, buf);
                    LOGD(_("Packet sent to %s:%d(%d) of %d bytes.\n"), c->host, c->port, c->fd, sendb);
                }
            }
            
            
            /// Check whether fail a connection
            if (c_broken != NULL) {
                LOGI(_("Connection to %s:%d is broken, try reconnect\n"), c_broken->host, c_broken->port);
              //LOGD("not in the thread to re-connect operation, No, is not here to re-connect the words seem to have been not re-connect");
              //LOGD("not in this thread(%s)to re-connect to operate, but would call ev_async_send(c->fd=%d) to notify another thread to call ev_feed_fd_event to perform this operation\n", __FUNCTION__, c_broken->fd);
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

