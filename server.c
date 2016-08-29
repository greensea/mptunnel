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
#include <ev.h>
#include "net.h"

#include "linklist.h"
#include "rbtree.h"

#include "server.h"
#include "mptunnel.h"
#include "buffer.h"
#include "client.h"

#define UDP_KEEP_ALIVE 300

/**
* A UDP connection, the last received packet time and the last transmitted packet time The maximum time difference
*/
#define UDP_INTERACTIVE_TIMEOUT 60


static struct ev_loop * g_ev_reactor = NULL;

static struct list_head g_buffers = LIST_HEAD_INIT(g_buffers);

static struct list_head g_bridge_list = LIST_HEAD_INIT(g_bridge_list);
static pthread_mutex_t g_bridge_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_listen_fd = -1;
static int g_target_fd = -1;

static int g_listen_port = 0;
static char *g_target_host = NULL;
static int g_target_port = 0;

extern int g_config_encrypt;



/**
* Receive remote Bridge is sent to the data processing function
*/
void recv_bridge_callback(struct ev_loop* reactor, ev_io* w, int events) {
    char* buf;
    int buflen = 65536;
    int readb;
    struct sockaddr_in baddr;
    
    static received_t *received = NULL;
    if (received == NULL) {
        received = malloc(sizeof(*received));
        received_init(received);
    }
    
    buf = malloc(buflen);
    memset(buf, 0x00, buflen);
    
    bridge_t* b = (bridge_t*)malloc(sizeof(bridge_t));
    memset(b, 0x00, sizeof(*b));
    b->st_time = time(NULL);
    b->addrlen = sizeof(b->addr);
    baddr = *(struct sockaddr_in*)&b->addr;
    
    //LOGD("received from the bridge end fd=%d sent data\n", w->fd);
    
    readb = recvfrom(w->fd, buf, buflen, 0, &b->addr, &b->addrlen);
    if (readb < 0) {
        LOGW(_("Bridge(fd=%d) may close the connection: %s\n"), w->fd, strerror(errno));
        free(buf); free(b);
        return;
    }
    else if (readb == 0) {
        LOGW(_("Can't received packet from bridge（fd=%d），bridge may close the connection\n"), w->fd);
        free(buf); free(b);
        return;
    }
    else {
        //LOGD("从桥端(:%u)收取了 %d 字节数据：%s\n", htons(baddr.sin_port), readb, (char*)buf + sizeof(packet_t));
        
        int exists = 0;
        bridge_t *lb;
        struct list_head *l;
        pthread_mutex_lock(&g_bridge_list_mutex);
        
        list_for_each(l, &g_bridge_list) {
            lb = list_entry(l, bridge_t, list);
            if (memcmp(&lb->addr, &b->addr, sizeof(struct sockaddr)) == 0) {
                exists = 1;
                
                free(b);
                b = lb;
                
                break;
            }
        }
        
        b->rc_time = time(NULL);
        
        if (exists != 1) {
            /// This is a new client, add it to the client list
            LOGI(_("Got a new client, add it to Client List\n"));
            list_add(&b->list, &g_bridge_list);
        }

        pthread_mutex_unlock(&g_bridge_list_mutex);
    }
    
    
    /// Unpack, and then sent to the target server
    packet_t* p;
    
    mpdecrypt(buf);
    p = (packet_t*)buf;
    
    if (p->type == PKT_TYPE_CTL) {
        //LOGD("from the bridge end(:%u)received %d bytes of data number of %d data packets, but this is a control packet, discarding it\n", htons(baddr. sin_port), readb, p->id);
        LOGD(_("Received control packet from bridge (:%u) of %d bytes, packet ID is %d, drop it\n"), htons(baddr.sin_port), readb, p->id);
        free(buf);
        return;
    }
    else if (p->type != PKT_TYPE_DATA) {
        //LOGD("from the bridge end(:%u)received %d byte number %d of data packet, but this is an unknown type of packet, discard it\n", htons(baddr. sin_port), readb, p->id);
        LOGD(_("Received packet from bridge (:%u) of %d bytes, packet ID is %d, but packet type is unknown, drop it.\n"), htons(baddr.sin_port), readb, p->id);
        free(buf);
        return;
    }
    else {
        //LOGD("from the bridge end(:%u)received %d byte number is %d packet\n", htons(baddr. sin_port), readb, p->id);
    }
    
    buflen = p->buflen;
    buf = (char*)buf + sizeof(*p);
    
    if (received_is_received(received, p->id) == 1) {
        //LOGD("from the bridge end(:%u)received %d byte number %d had charge of the data packet, discarding it\n", htons(baddr. sin_port), readb, p->id);
        LOGD(_("Received packet from bridge (:%u) of %d bytes which was received, packet ID is %d, drop it\n"), htons(baddr.sin_port), readb, p->id);
        free(p);
        
        //received_destroy(received);
        //free(received);
        //received = NULL;
        
        return;
    }
    else {
        //LOGD("from the bridge end(:%u)received %d byte number %d of data packet, forwarding the packet\n", htons(baddr. sin_port), readb, p->id);
        LOGD(_("Received packet from bridge (:%u) of %d bytes, ID is %d, forward it\n"), htons(baddr.sin_port), readb, p->id);
        received_add(received, p->id);
    }
    
    
    if (received != NULL) {
        received_try_dropdead(received, 30);
    }
    
    
    /// Sent to the target server
    int sendb;
    sendb = send(g_target_fd, buf, buflen, MSG_DONTWAIT);
    if (sendb < 0) {
        ///LOGW("unable to the target server to send the number is %d packet:%s\n", p->id, strerror(errno));
        LOGW(_("Can't send packet #%d to target server: %s\n"), p->id, strerror(errno));
    }
    else if (sendb == 0) {
        //LOGW("target server may have been disconnected, unable to forward %d number of data packet\n", p->id);
        LOGW(_("Connection to target server seems closed, can't forward packet #%d\n"), p->id);
    }
    else {
        //LOGD("successfully to the target server to send %d bytes data:%s\n", buflen, buf);
    }
    
    free(p);
    return;
}


/**
* ev processing thread
*/
void* ev_thread(void* ptr) {
    int port = 3002;
    
    LOGD(_("libev thread started\n"));
    
    g_listen_fd = net_bind("0.0.0.0", port, SOCK_DGRAM);
    if (g_listen_fd < 0) {
        LOGE(_("Can't listen port %d: %s\n"), port, strerror(errno));
        exit(0);
    }
    
    g_ev_reactor = ev_loop_new(EVFLAG_AUTO);
     
    ev_io* w = (ev_io*)malloc(sizeof(ev_io));
    ev_io_init(w, recv_bridge_callback, g_listen_fd, EV_READ);
    ev_io_start(g_ev_reactor, w);
     
    ev_run(g_ev_reactor, 0);
        
    LOGW(_("libev thread exited\n"));
}


/**
* To bridge who sent the data
*/
int send_to_servers(char* buf, int buflen) {
    struct sockaddr* addr;
    struct sockaddr_in *baddr;
    socklen_t addrlen;
    int sendb;
    char ipstr[128] = {0};
    static int id = 0;
    
    if (buflen > MAX_PACKET_SIZE) {
        int ret = 0;
        int split = buflen / 2;
        
        //LOGI("sent data size is %d bytes, exceeds the maximum packet size(%d bytes), the packet is split into two packets and then try to send\n", buflen, MAX_PACKET_SIZE);
        LOGI(_("Packet is %d bytes, which excees max packet size limit (%d bytes), spilt the packet into two smaller packets before send.\n"), buflen, MAX_PACKET_SIZE);
        
        ret += send_to_servers(buf, split);
        ret += send_to_servers(buf + split, buflen - split);
        return ret;
    }
    
    
    packet_t* p;
    packet_t rawp;
    p = (packet_t*)malloc(sizeof(*p) + buflen);
    p->type = PKT_TYPE_DATA;
    p->id = ++id;
    p->buflen = buflen;
    memcpy(((char*)p) + sizeof(*p), buf, buflen);
    
    rawp = *p;
    mpencrypt((char*)p, buflen + sizeof(*p));
    
    int ts = time(NULL);
    bridge_t *b;
    struct list_head *l, *tmp;
    
    list_for_each_safe(l, tmp, &g_bridge_list) {
        b = list_entry(l, bridge_t, list);
        baddr = (struct sockaddr_in*)&b->addr;
        
        /// 1. Check whether the connection is timeout
        if (ts - b->rc_time > UDP_KEEP_ALIVE) {
            //LOGD("Bridge%s:%u idle %d seconds, that this bridge has been disconnected, not to forward the data packet of %d\n", ipstr, ntohs(baddr->sin_port), ts - b->rc_time, p->id);
            LOGD(_("No packet received from bridge (%s:%u) for %d seconds, assume the connection is closed, stop forward packet to it %d\n"), ipstr, ntohs(baddr->sin_port), ts - b->rc_time, p->id);
            list_del(l);
            free(l);
            continue;
        }
        
        if (abs(b->rc_time - b->st_time) > UDP_INTERACTIVE_TIMEOUT) {
            //LOGD("Bridge%s:%u last contract and the packet receiving time difference of more than %d seconds, actual:%d, that this bridge has been disconnected, not to forward the data packet of %d\n", ipstr, ntohs(baddr->sin_port), UDP_INTERACTIVE_TIMEOUT, b->rc_time - b->st_time, p->id);
            LOGD(_("The time difference between packet received and packet sent of bridge %s:%u is larger than %d seconds (Actually %d seconds), assume the connection is broken, stop forward packet to it %d\n"), ipstr, ntohs(baddr->sin_port), UDP_INTERACTIVE_TIMEOUT, b->rc_time - b->st_time, p->id);
            list_del(l);
            free(l);
            continue;
        }
        
        b->st_time = time(NULL);
        
        
        /// 2. Transmitting the data packet
        sendb = sendto(g_listen_fd, p, buflen + sizeof(*p), 0, &b->addr, b->addrlen);
        if (sendb < 0) {
            //LOGW("无法向桥(%s:%d)发送 %d 字节数据，包编号 %d: %s\n", ipstr, ntohs(baddr->sin_port), buflen, rawp.id, strerror(errno));
            LOGW(_("Can't send packet to bridge(%s:%d) of %d bytes, packet ID is %d: %s\n"), ipstr, ntohs(baddr->sin_port), buflen, rawp.id, strerror(errno));
        }
        else if (sendb == 0) {
            LOGW("Can't send packet to bridge, bridge may close the connection\n");
        }
        else {
            //LOGD("to bridge, port:%u, sent %d bytes of data, packet number %d\n", ntohs(baddr->sin_port), sendb, rawp. id);
            LOGD(_("Forward packet to bridge(port %u) of %d bytes, packet ID is %d\n"), ntohs(baddr->sin_port), sendb, rawp.id);
        }
    }
    
    free(p);
    
    return 0;
}




/**
* For the forwarding server message to client thread
*/
void* server_thread(void* ptr) {
    int readb, sendb, buflen;
    char* buf;
    
    LOGD(_("Thread which forward packet from server to bridge is started\n"));
    
    
    buflen = 65536;
    buf = malloc(buflen);
    
    g_target_fd = net_connect(g_target_host, g_target_port, SOCK_DGRAM);
    if (g_target_fd < 0) {
        LOGE(_("Could not connect to target server：%s\n"), strerror(errno));
        return NULL;
    }
    
    while (1) {
        memset(buf, 0x00, sizeof(buflen));
        readb = recv(g_target_fd, buf, buflen, 0);
        if (readb < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            else {
                LOGI(_("Target server close the connection: %s\n"), strerror(errno));
                g_target_fd = net_connect(g_target_host, g_target_port, SOCK_DGRAM);
                continue;
            }
        }
        else if (readb == 0) {
            LOGW(_("Can't received packet from server, server close the connection\n"));
            g_target_fd = net_connect(g_target_host, g_target_port, SOCK_DGRAM);
            continue;
        }
        else {
            /// Received a data, the data is forwarded to the bridge
            send_to_servers(buf, readb);
        }
    }
    
    free(buf);
    
    LOGD(_("Thread which forward packet from server to bridge exited\n"));
    
    return NULL;
}




int main(int argc, char** argv) {
    int clientfd, listenfd;
    
    setlocale(LC_ALL, "");
    bindtextdomain("mptunnel", "locale");
    textdomain("mptunnel");
    
    if (argc <= 3) {
        fprintf(stderr, _("Usage: <%s> <listen_port> <target_ip> <target_port>\n"), argv[0]);
        fprintf(stderr, _("To disable encryption, set environment variable MPTUNNEL_ENCRYPT=0\n"));
        exit(-1);
    }
    else {
        /// Load the configuration information
        g_listen_port = atoi(argv[1]);
        g_target_host = strdup(argv[2]);
        g_target_port = atoi(argv[3]);
        
        if (g_listen_port <= 0 || g_listen_port >= 65536) {
            LOGE("Invalid listen port `%s'\n", argv[1]);
            exit(-2);
        }
        if (g_target_port <= 0 || g_target_port >= 65536) {
            LOGE("Invalid target port `%s'\n", argv[3]);
            exit(-3);
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
        LOGD(_("Configuration: Local listening port: %d\n"), g_listen_port);
        LOGD(_("Configuration: server：%s:%d\n"), g_target_host, g_target_port);
    }
    
    
    
    LOGD(_("Initializing libev thread\n"));
    pthread_t tid;
    pthread_create(&tid, NULL, ev_thread, NULL);
    pthread_detach(tid);
    


    /// Create a forwards the data to the destination server thread
    int* ptr = malloc(sizeof(int));
    *ptr = clientfd;
    pthread_create(&tid, NULL, server_thread, NULL);
    pthread_detach(tid);
    
    while (1) {
        sleep(100);
    }

    
    return 0;
}




/**
* Initialize a receiver ev, is used to process the received data
*/
ev_io* init_recv_ev(int fd) {
    ev_io *watcher = (ev_io*)malloc(sizeof(ev_io));
    memset(watcher, 0x00, sizeof(*watcher));
    
    ev_io_init(watcher, recv_bridge_callback, fd, EV_READ);
    ev_io_start(g_ev_reactor, watcher);
    
    return watcher;
}






