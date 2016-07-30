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
* Have received a package of red-black tree root
*/
static struct rb_root g_received_rbtree = RB_ROOT;

/**
* Assemble a data packet, assembled it out of the pack need to call packet_free release
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
        LOGW(_("Could not send data to %d with %lu bytes: %s\n"), fd, sizeof(*p) + buflen, strerror(errno));
    }
    else if (sendb == 0){ 
        LOGW("fd=%d may close the connection\n", fd);
    }
    else {
        //LOGD ("%d sending %d byte message '%s '\n", fd, sendb, (char*)(p + 1));
        LOGD(_("Sent %d bytes to %d, message id is %d\n"), sendb, fd, id);
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
* Determine whether a packet was received, if received before return 1,otherwise return 0
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
* Received the package Manager, and remove from the list an id
*/
int received_list_del(received_t* r, int id) {
    received_list_t* c;
    
    /// From the“Receive packet list”delete this id 
    pthread_mutex_lock(&r->rlist_mutex);
    
    c = received_rbtree_get(&g_received_rbtree, id);
    if (c != NULL) {
        rb_erase(&c->rbnode, &g_received_rbtree);
        free(c);
    }
    else {
        LOGW(_("Pakcet #%d is not exists in Received Packet List\n"), id);
    }
    
    pthread_mutex_unlock(&r->rlist_mutex);
    
    return 0;
}


/**
* Received the package Manager, to the list add an id
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
* Received the package Manager: determining a whether the data packet has been received.
* 
* @param int if a data packet has been received. returns 1,if not received returns 0
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
        
        /// In the list to find the id
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
* Received the package Manager, the record received is a data packet
* 
* @param received_t* received the package Manager
* @param int the just received data packet the id of the
*/
int received_add(received_t* r, int id) {
    /// The data packet received just more than“the continuous receipt of a minimum data packet number”large-1 case
    if (id == r->min_con_id + 1) {
        r->min_con_id = id;
        received_list_del(r, id);
    }
    else if (id <= r->min_con_id) {
        /// This packet has been received before, ignore
    }
    else {
        /// This package to skip a few packets, add it to the list
        received_list_add(r, id);
    }
    
    if (id > r->max_id) {
        r->max_id = id;
    }
    
    return 0;
}

/**
* Received the package Manager, the discard-timeout for the data packet, and updates the“continuous receipt of the minimum data package”
* 
* @param received_t* throw the package Manager
* @param int discarded number of seconds is not received within the package.
*/
int received_try_dropdead(received_t* r, int ttl) {
    long ts = time(NULL);
    if (r->last_dropdead_time + ttl <= ts) {
        LOGD(_("Cleanup timed out packets, TTL=%d, last cleanup time is %ld, current time is %ld, elapsed time is %ld seconds\n"), ttl, r->last_dropdead_time, ts, ts - r->last_dropdead_time);
        r->last_dropdead_time = ts;
    }
    else {
        return 0;
    }
    
    
    pthread_mutex_lock(&r->rlist_mutex);
    
    do {
        received_list_t *minn = NULL;
        struct rb_node* rbnode = rbnode;
        /// Find the list of the smallest id, to determine its ctime whether the distance now has more than ttl seconds, if it is, then discard the id before all the data packets
        /// If its ctime is now no more than ttl seconds, then exit the Clean-up process
        
        rbnode = rb_first(&g_received_rbtree);
        minn = container_of(rbnode, received_list_t, rbnode);
        
        if (minn != NULL) {
            if (minn->ctime + ttl <= time(NULL)) {
                /// Discard the id before all the data packets
                /// Since this packet is a list of the minimum data package, so no need to clean up the list, just to minn from the list can be deleted
                
                //LOGD("data package“%d”received time has passed for %ld seconds, think before all data packets have been received, the current minimum has received a continuous packet id is %d\n", minn->id, time(NULL) - minn->ctime, r->min_con_id);
                LOGD(_("Packet #%d was received and time elapsed %ld seconds, assume packets which ID is smaller than it are all received. The smallest ID of received packet is %d\n"), minn->id, time(NULL) - minn->ctime, r->min_con_id);
                
                rb_erase(&minn->rbnode, &g_received_rbtree);
                r->min_con_id = minn->id;
                free(minn);
            }
            else {
                /// Exit the loop
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
    
    LOGD(_("Finish cleanup timed out packets, TTL = %d, smallest continuous received packet ID is %d\n"), ttl, r->min_con_id);
    
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
            LOGW(_("Packet #%d is already exists in Packet Received List\n"), cur->id);
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
* Encrypt and decrypt the content
* 
* @param char* content to be encrypted
* @param int to the encrypted Content Length
* @param a uint32_t that the initialization vector
*/
void encrypt_lfsr(char* _buf, int _size, uint32_t *iv) {
    int i;
    unsigned char *buf = (unsigned char*)_buf;
    unsigned char ivc;
    
    for (i = 0; i < _size; i++) {
        ivc = lfsr_rand(iv) % 255;
        buf[i] ^= ivc;
    }
}


void decrypt_lfsr(char* _buf, int _size, uint32_t* iv) {
    encrypt_lfsr(_buf, _size, iv);
}

/**
* For a complete mptunnel packet encryption and decryption
*/
void mpdecrypt(char* _buf) {
    packet_t *p = (packet_t*)_buf;
    uint32_t iv;
    
    if (g_config_encrypt == 0) {
        return;
    }
    
    /// FIXME: since the data by the UDP Protocol to transmit, so the p->buflen not credible, it should be at the same time the incoming buflen and do check, to prevent memory errors
    
    /// First decrypt the packet_t
    iv = p->iv;
    
    decrypt_lfsr(_buf + sizeof(p->iv), sizeof(packet_t) - sizeof(p->iv), &iv);
    
    /// Then decrypt the content
    decrypt_lfsr(_buf + sizeof(packet_t), p->buflen, &iv);
}

void mpencrypt(char* _buf, int _buflen) {
    packet_t *p = (packet_t*)_buf;
    uint32_t iv;
    
    if (g_config_encrypt == 0) {
        return;
    }
    

    iv = rand();
    p->iv = iv;
    
    encrypt_lfsr(_buf + sizeof(p->iv), _buflen - sizeof(p->iv), &iv);
}



/**
* A simple linear feedback shift register random number generator
* 
* @param a uint32_t that the random number seed status, the status value will be changed
* @return a uint32_t that a 32-bit unsigned integer random number
*/
uint32_t lfsr_rand(uint32_t *st) {
    unsigned char b32, b30, b26, b25, b;
    uint32_t r = 0x00;
    int i;
    
    ///
    /** In our application does not require high-strength random number
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
