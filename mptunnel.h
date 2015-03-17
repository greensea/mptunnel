#ifndef __MPTUNNEL_H__
#define __MPTUNNEL_H__ 1

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <stdint.h>

#include "linklist.h"
#include "rbtree.h"


/// 转发时最大的包长度
#define MAX_PACKET_SIZE 8000


#define LOG_ERROR 1
#define LOG_WARNING 2
#define LOG_NOTICE 3
#define LOG_INFO 4
#define LOG_LDEBUG 5
#define LOG_DEBUG 6


#define LOGD(FMT, ...) LOG(LOG_DEBUG, FMT, ##__VA_ARGS__)
#define LOGW(FMT, ...) LOG(LOG_WARNING, FMT, ##__VA_ARGS__)
#define LOGE(FMT, ...) LOG(LOG_ERROR, FMT, ##__VA_ARGS__)
#define LOGN(FMT, ...) LOG(LOG_NOTICE, FMT, ##__VA_ARGS__)
#define LOGI(FMT, ...) LOG(LOG_INFO, FMT, ##__VA_ARGS__)

#define LOG(level, FMT, ...) do {    \
    static struct tm *tmp = NULL; static time_t t1, t2 = (time_t)NULL; struct timeval tv; char timestr[128] = {0}; char ms[4] = {0};  \
    if ((t1 = time(NULL)) != t2) {t2 = t1; tmp = localtime(&t2); }     \
    gettimeofday(&tv, NULL);  \
    if (tmp == NULL) {  \
        strncpy(timestr, "unknow", sizeof(timestr) - 1);   \
    } else {    \
        strftime(timestr, sizeof(timestr), "%F %H:%M:%S.", tmp);  \
        sprintf(ms, "%03d", (int)tv.tv_usec / 1000);   \
        strncat(timestr, ms, sizeof(timestr) - strlen(timestr) - 1); \
    } \
    fputc('[', stderr); fputs(timestr, stderr); fputs("]", stderr); fprintf(stderr, "(%s:%d) ", __FILE__, __LINE__); fprintf(stderr, FMT, ##__VA_ARGS__); \
} while(0);


enum packet_type {
    PKT_TYPE_CTL,
    PKT_TYPE_DATA
};

typedef struct packet_t {
    uint32_t iv;    /** 加密使用的 IV */
    enum packet_type type;
    int id;
    int buflen;     /** 数据长度（不包括 packet_t 自身） */
} packet_t;


typedef struct received_list_t {
    struct rb_node rbnode;
    long ctime;
    int id;
} received_list_t;
    

typedef struct received_t {
    int min_con_id;     /// 连续收到的最小包编号
    int max_id;         /// 目前已经收到的最大包编号
    time_t last_dropdead_time;
    struct list_head rlist;
    pthread_mutex_t rlist_mutex;
} received_t;


packet_t* packet_make(enum packet_type type, const char* buf, int buflen, int);
int packet_free(packet_t* p);
int packet_send(int fd, char* buf, int buflen, int);
    
//int packet_received(int id);
//int packet_is_received(int _id);

int received_is_received(received_t* r, int id);
int received_try_dropdead(received_t* r, int ttl);
int received_init(received_t* r);
int received_add(received_t* r, int id);
int received_destroy(received_t* r);

received_list_t* received_rbtree_get(struct rb_root*, int);
int received_rbtree_add(struct rb_root* , received_list_t*);


void encrypt(char* _buf, int _size, uint32_t*);
void decrypt(char* _buf, int _size, uint32_t*);
void mpdecrypt(char* _buf);
void mpencrypt(char* _buf, int _buflen);

uint32_t lfsr_rand(uint32_t*);
#endif
