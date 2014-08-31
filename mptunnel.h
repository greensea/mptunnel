#ifndef __MPTUNNEL_H__
#define __MPTUNNEL_H__ 1

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>


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
    struct tm *tmp; time_t t; struct timeval tv; char timestr[128] = {0}; char ms[4] = {0};  \
    t = time(NULL); tmp = localtime(&t); gettimeofday(&tv, NULL);  \
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
    enum packet_type type;
    int id;
    int buflen;     /** 数据长度（不包括 packet_t 自身） */
} packet_t;


packet_t* packet_make(enum packet_type type, const char* buf, int buflen, int);
int packet_free(packet_t* p);
int packet_send(int fd, char* buf, int buflen, int);
    
int packet_received(int id);
int packet_is_received(int _id);

#endif
