#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

#include "mptunnel.h"

//static int* g_packet_id = NULL;

//static int* g_received_id = NULL;

/**
 * 组装一个数据包，组装出来的包需要调用 packet_free 进行释放
 */
packet_t* packet_make(enum packet_type type, const char* buf, int buflen, int id) {
    packet_t* p;

    
    p = (packet_t*)malloc(sizeof(*p) + buflen);
    memset(p, 0x00, sizeof(*p));
    p->type = type;
    p->id = id;
    p->buflen = buflen;
    memcpy(p + 1, buf, buflen);
    
    return p;
}


int packet_free(packet_t* p) {
    free(p);
    
    return 0;
}
    
    
    
int packet_send(int fd, char* buf, int buflen, int id) {
    int sendb;
    
    packet_t* p = packet_make(PKT_TYPE_DATA, buf, buflen, id);

    sendb = send(fd, p, sizeof(*p) + p->buflen, MSG_DONTWAIT);
    if (sendb < 0) {
        LOGW("无法向发送 %d 字节数据: %s\n",  buflen, strerror(errno));
    }
    else if (sendb == 0){ 
        LOGW("fd=%d 可能断开了连接\n", fd);
    }
    else {
        //LOGD("向 %d 发送了 %d 字节消息“%s”\n", fd, sendb, (char*)(p + 1));
        LOGD("向 %d 发送了 %d 字节消息\n", fd, sendb);
    }
    
    packet_free(p);
    
    return sendb;
}


/*
int packet_received(int _id) {
    if (g_received_id == NULL) {
        g_received_id = malloc(sizeof(int));
    }
    
    *g_received_id = _id;
    
    return 0;
}
*/

/**
 * 判断一个包是否曾经接收过，如果接收过返回 1,否则返回 0
 */
/*
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
*/

int received_init(received_t* r) {
    pthread_mutexattr_t mutexattr;
    
    memset(r, 0x00, sizeof(*r));
    
    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&r->rlist_mutex, &mutexattr);
    pthread_mutexattr_destroy(&mutexattr);
    
    INIT_LIST_HEAD(&r->rlist);
    
    return 0;
}

int received_destroy(received_t* r) {
    pthread_mutex_lock(&r->rlist_mutex);
    list_del_init(&r->rlist);
    pthread_mutex_unlock(&r->rlist_mutex);
    
    pthread_mutex_destroy(&r->rlist_mutex);
    
    return 0;
}

/**
 * 收包管理器，从列表中删除一个 id
 */
int received_list_del(received_t* r, int id) {
    /// 从“已收到包列表”中删除这个 id 
    pthread_mutex_lock(&r->rlist_mutex);
    
    struct list_head *pos, *n;
    received_list_t* c;
    list_for_each_safe(pos, n, &r->rlist) {
        c = list_entry(n, received_list_t, list_node);
        if (c->id == id) {
            list_del(&c->list_node);
            break;
        }
    }
    
    pthread_mutex_unlock(&r->rlist_mutex);
    
    return 0;
}


/**
 * 收包管理器，往列表中增加一个 id
 */
int received_list_add(received_t* r, int id) {
    pthread_mutex_lock(&r->rlist_mutex);
    
    received_list_t* n;
    
    n = malloc(sizeof(*n));
    memset(n, 0x00, sizeof(*n));
    n->ctime = time(NULL);
    LOGD("(id=%d)n->ctime = %ld\n", id, n->ctime);
    n->id = id;
    
    pthread_mutex_lock(&r->rlist_mutex);
    
    list_add_tail(&n->list_node, &r->rlist);
    
    pthread_mutex_unlock(&r->rlist_mutex);
    
    return 0;
}


/**
 * 收包管理器：判断一个数据包是否已经收过了
 * 
 * @param int       如果一个数据包已经收到过了就返回 1,如果没有收到过就返回 0
 */
int received_is_received(received_t* r, int id) {
    if (id <= r->min_con_id) {
        return 1;
    }
    else {
        int ret = 0;
        
        /// 在列表中查找这个 id
        pthread_mutex_lock(&r->rlist_mutex);
        
        struct received_list_t *n;
        list_for_each_entry(n, &r->rlist, list_node) {
            if (n->id == id) {
                ret = 1;
                break;
            }
        }
        
        pthread_mutex_unlock(&r->rlist_mutex);
        
        return ret;
    }
}


/**
 * 收包管理器，记录收到了一个数据包
 * 
 * @param received_t*       收包管理器
 * @param int               刚刚收到的数据包的 id
 */
int received_add(received_t* r, int id) {
    /// 收到的数据包正好比“连续收到的最小数据包编号”大 1 的情况
    if (id == r->min_con_id + 1) {
        r->min_con_id = id;
        received_list_del(r, id);
    }
    else if (id <= r->min_con_id) {
        /// 这个包已经收过了，无视
    }
    else {
        /// 这个包跳过了几个包，将其添加到列表中
        received_list_add(r, id);
    }
    
    return 0;
}

/**
 * 收包管理器，丢弃超时的数据包，并更新“连续收到的最小数据包”
 * 
 * @param received_t*       丢包管理器
 * @param int               丢弃多少秒内未收到的包。
 */
int received_try_dropdead(received_t* r, int ttl) {
    if (r->last_dropdead_time + ttl <= time(NULL)) {
        LOGD("进行一次丢包清理，ttl = %d\n", ttl);
        r->last_dropdead_time = time(NULL);
    }
    else {
        return 0;
    }
    
    
    pthread_mutex_lock(&r->rlist_mutex);
    
    do {
        received_list_t *pos = NULL, *n = NULL, *minn = NULL;
        int minid = INT_MAX;
        
        /// 找列表中最小的 id，判断其 ctime 是否距离现在已经超过了 ttl 秒，如果是，则丢弃该 id 之前的所有数据包
        /// 如果其 ctime 距离现在没有超过 ttl 秒，则退出清理过程
        
        list_for_each_entry_safe(pos, n, &r->rlist, list_node) {
            if (pos->id < minid) {
                minn = pos;
                minid = minn->id;
            }
        }
        
        
        if (minn != NULL) {
            if (minn->ctime + ttl <= time(NULL)) {
                /// 丢弃该 id 之前的所有数据包
                /// 由于这个数据包是列表中最小的数据包，所以不需要清理列表，只需要将 minn 从列表中删除即可
                
                LOGD("数据包“%d”收到的时间已经过去了 %ld 秒，认为之前所有的数据包都已经收到了，当前最小已收到连续包 id 是 %d\n", minn->id, time(NULL) - minn->ctime, r->min_con_id);
                
                list_del(&minn->list_node);
                r->min_con_id = minn->id;
                free(minn);
            }
            else {
                /// 退出循环
                minn = NULL;
                break;
            }
        }
        else {
            break;
        }
    }
    while (1);
    
    pthread_mutex_unlock(&r->rlist_mutex);
    
    LOGD("完成了一次丢包清理，ttl = %d, 当前最小连续收到的包编号是 %d\n", ttl, r->min_con_id);
    
    return 0;
}
