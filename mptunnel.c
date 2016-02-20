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
 * Enable or disable encryption. default = 1 (encrypt)
 */
int g_config_encrypt = 1;


/**
 * 已经收到的包的红黑树根
 */
static struct rb_root g_received_rbtree = RB_ROOT;

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
    
    mpencrypt((char*)p, sizeof(*p) + buflen);
    
    return p;
}


int packet_free(packet_t* p) {
    free(p);
    
    return 0;
}

    
    
int packet_send(int fd, char* buf, int buflen, int id) {
    int sendb;
    packet_t* p = packet_make(PKT_TYPE_DATA, buf, buflen, id);

    errno = 0;
    sendb = send(fd, p, sizeof(*p) + buflen, MSG_DONTWAIT);
    if (sendb < 0) {
        LOGW("无法向 %d 发送 %lu 字节数据: %s\n", fd, sizeof(*p) + buflen, strerror(errno));
    }
    else if (sendb == 0){ 
        LOGW("fd=%d 可能断开了连接\n", fd);
    }
    else {
        //LOGD("向 %d 发送了 %d 字节消息“%s”\n", fd, sendb, (char*)(p + 1));
        LOGD("向 %d 发送了 %d 字节消息，编号为 %d\n", fd, sendb, id);
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
    received_list_t* c;
    
    /// 从“已收到包列表”中删除这个 id 
    pthread_mutex_lock(&r->rlist_mutex);
    
    c = received_rbtree_get(&g_received_rbtree, id);
    if (c != NULL) {
        rb_erase(&c->rbnode, &g_received_rbtree);
        free(c);
    }
    else {
        LOGW("已经收到的包列表中没有编号为 %d 的包\n", id);
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
    n->id = id;
    
    pthread_mutex_lock(&r->rlist_mutex);
    
    //list_add_tail(&n->list_node, &r->rlist);
    received_rbtree_add(&g_received_rbtree, n);
    
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
    else if (id > r->max_id) {
        return 0;
    }
    else {
        int ret = 0;
        
        /// 在列表中查找这个 id
        pthread_mutex_lock(&r->rlist_mutex);
        
        struct received_list_t *n;
        
        n = received_rbtree_get(&g_received_rbtree, id);
        if (n != NULL) {
            ret = 1;
        }
        else {
            ret = 0;
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
    
    if (id > r->max_id) {
        r->max_id = id;
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
    long ts = time(NULL);
    if (r->last_dropdead_time + ttl <= ts) {
        LOGD("进行一次丢包清理，ttl = %d，上次清理时间是 %ld, 当前时间是 %ld, 时间差是 %ld\n", ttl, r->last_dropdead_time, ts, ts - r->last_dropdead_time);
        r->last_dropdead_time = ts;
    }
    else {
        return 0;
    }
    
    
    pthread_mutex_lock(&r->rlist_mutex);
    
    do {
        received_list_t *minn = NULL;
        struct rb_node* rbnode = rbnode;
        /// 找列表中最小的 id，判断其 ctime 是否距离现在已经超过了 ttl 秒，如果是，则丢弃该 id 之前的所有数据包
        /// 如果其 ctime 距离现在没有超过 ttl 秒，则退出清理过程
        
        rbnode = rb_first(&g_received_rbtree);
        minn = container_of(rbnode, received_list_t, rbnode);
        
        if (minn != NULL) {
            if (minn->ctime + ttl <= time(NULL)) {
                /// 丢弃该 id 之前的所有数据包
                /// 由于这个数据包是列表中最小的数据包，所以不需要清理列表，只需要将 minn 从列表中删除即可
                
                LOGD("数据包“%d”收到的时间已经过去了 %ld 秒，认为之前所有的数据包都已经收到了，当前最小已收到连续包 id 是 %d\n", minn->id, time(NULL) - minn->ctime, r->min_con_id);
                
                rb_erase(&minn->rbnode, &g_received_rbtree);
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





int received_rbtree_add(struct rb_root* root, received_list_t *node) {
    struct rb_node **new = &(root->rb_node);
    struct rb_node *parent = NULL;
    received_list_t* cur = NULL;
    
    while (*new) {
        parent = *new;
        cur = container_of(*new, received_list_t, rbnode);
        
        if (node->id < cur->id) {
            new = &(*new)->rb_left;
        }
        else if (node->id > cur->id) {
            new = &(*new)->rb_right;
        }
        else {
            LOGW("编号为 %d 的包已经在已收到的包列表中了\n", cur->id);
            return 0;
        }
    }
        
    rb_link_node(&node->rbnode, parent, new);
    rb_insert_color(&node->rbnode, root);
    
    return 0;
}

int received_rbtree_del(struct rb_root* root, received_list_t *node) {
    rb_erase(&node->rbnode, root);
    return 0;
}

received_list_t* received_rbtree_get(struct rb_root* root, int id) {
    received_list_t* node = NULL;
    struct rb_node* cur = root->rb_node;
    
    while (cur) {
        node = container_of(cur, received_list_t, rbnode);
        
        if (id < node->id) {
            cur = cur->rb_left;
        }
        else if (id > node->id) {
            cur = cur->rb_right;
        }
        else {
            return node;
        }
    }
    
    return NULL;
}


/**
 * 加密和解密内容
 * 
 * @param char*     要加密的内容
 * @param int       要加密内容的长度
 * @param uint32_t  初始化向量
 */
void encrypt(char* _buf, int _size, uint32_t *iv) {
    int i;
    unsigned char *buf = (unsigned char*)_buf;
    unsigned char ivc;
    
    for (i = 0; i < _size; i++) {
        ivc = lfsr_rand(iv) % 255;
        buf[i] ^= ivc;
    }
}


void decrypt(char* _buf, int _size, uint32_t* iv) {
    encrypt(_buf, _size, iv);
}

/**
 * 对一个完整的 mptunnel 数据包进行加密和解密
 */
void mpdecrypt(char* _buf) {
    packet_t *p = (packet_t*)_buf;
    uint32_t iv;
    
    if (g_config_encrypt == 0) {
        return;
    }
    
    /// FIXME: 由于数据通过 UDP 协议传输，故此处的 p->buflen 不可信，应该同时传入 buflen 并做校验，以防止内存错误
    
    /// 首先解密 packet_t
    iv = p->iv;
    
    decrypt(_buf + sizeof(p->iv), sizeof(packet_t) - sizeof(p->iv), &iv);
    
    /// 接着解密内容
    decrypt(_buf + sizeof(packet_t), p->buflen, &iv);
}

void mpencrypt(char* _buf, int _buflen) {
    packet_t *p = (packet_t*)_buf;
    uint32_t iv;
    
    if (g_config_encrypt == 0) {
        return;
    }
    

    iv = rand();
    p->iv = iv;
    
    encrypt(_buf + sizeof(p->iv), _buflen - sizeof(p->iv), &iv);
}



/**
 * 一个简单的线性反馈移位寄存器随机数发生器
 * 
 * @param uint32_t      随机数种子（状态），该状态的值会被改变
 * @return uint32_t     一个 32 位无符号整型的随机数
 */
uint32_t lfsr_rand(uint32_t *st) {
    unsigned char b32, b30, b26, b25, b;
    uint32_t r = 0x00;
    int i;
    
    ///
    /** 在我们的应用中不需要高强度的随机数
    if (*st == 0) {
        *st = 1;
    }
    */
    
    for (i = 0; i < 32; i++) {
        b32 = *st & 0x00000001;
        b30 = (*st & (0x00000001 << 2)) >> 2;
        b26 = (*st & (0x00000001 << 6)) >> 6;
        b25 = (*st & (0x00000001 << 7)) >> 7;
                
        b = b32 ^ b30 ^ b26 ^ b25;
        
        *st >>= 1;
        if (b == 0) {
            *st &= 0x7fffffff;
        }
        else {
            *st |= 0x80000000;
        }
    }
    
    r = *st;
    
    return r;
}
