#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mptunnel.h"
#include "net.h"

/**
 * 在指定地址上开始一个监听 
 * 
 * @param const char*   监听地址，暂时只支持 IPv4,使用点分数字格式表示
 * @param int           监听端口
 * @param type          连接类型，可选 SOCK_STREAM 和 SOCK_DGRAM
 * @return int          成功时返回监听 fd,失败时返回其他值，并设置 errno
 */
int net_bind(const char* bind_ip, int port, int type) {
    int ret, n, c[4];
    int fd;
    struct sockaddr_in server_addr;

    if( ( fd = (int) socket( AF_INET, type, IPPROTO_IP ) ) < 0 ) {
        return fd;
    }

    n = 1;
    setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &n, sizeof( n ) );

    server_addr.sin_addr.s_addr = htonl( INADDR_ANY );
    server_addr.sin_family      = AF_INET;
    server_addr.sin_port        = htons( port );

    if( bind_ip != NULL )
    {
        memset( c, 0, sizeof( c ) );
        sscanf( bind_ip, "%d.%d.%d.%d", &c[0], &c[1], &c[2], &c[3] );

        for( n = 0; n < 4; n++ )
            if( c[n] < 0 || c[n] > 255 )
                break;

        if( n == 4 )
            server_addr.sin_addr.s_addr = htonl(
                ( (uint32_t) c[0] << 24 ) |
                ( (uint32_t) c[1] << 16 ) |
                ( (uint32_t) c[2] <<  8 ) |
                ( (uint32_t) c[3]       ) );
    }

    ret = bind( fd, (struct sockaddr *) &server_addr, sizeof( server_addr ) );
    if (ret < 0) {
        LOGW("bind 失败(fd = %d)：%s\n", fd, strerror(errno));
        close(fd);
        return ret;
    }
    
    if (type == SOCK_STREAM) {
        ret = listen(fd, 1);
        if (ret < 0) {
            LOGW("listen 失败(fd = %d)：%s\n", fd, strerror(errno));
            close(fd);
            return ret;
        }
    }

    return fd;
}


int net_accept(int bind_fd, void* client_ip) {
    int client_fd;
    
    struct sockaddr_in client_addr;
    socklen_t n;

    client_fd = accept(bind_fd, (struct sockaddr *) &client_addr, &n);

    if( client_fd < 0 )
    {
        return client_fd;
    }

    if( client_ip != NULL )
    {
        memcpy( client_ip, &client_addr.sin_addr.s_addr,
                    sizeof( client_addr.sin_addr.s_addr ) );
    }

    return( 0 );
}


/**
 * 连接到一个服务器
 * 
 * @param const char*   服务器地址
 * @param int           服务器端口
 * @param int           连接类型，可以是 SOCK_STREAM 或 SOCK_DGRAM
 * @return int          连接成功返回 fd，失败返回负数，并设置 errno
 */
int net_connect(const char* host, int port, int type) { 
    int ret, fd;
    struct sockaddr_in server_addr;
    struct hostent *server_host;



    server_host = gethostbyname( host );
    if (server_host == NULL) {
        return EINVAL;
    }
    
    if( ( fd = (int) socket( AF_INET, type, IPPROTO_IP ) ) < 0 ) {
        return fd;
    }

    memcpy( (void *) &server_addr.sin_addr,
            (void *) server_host->h_addr,
                     server_host->h_length );

    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons( port );

    ret = connect( fd, (struct sockaddr *) &server_addr, sizeof( server_addr ));
    if (ret < 0) {
        close( fd );
        return ret;
    }

    return fd;
}
