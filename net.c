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
* At the specified address on the start a listener 
* 
* @param const char* listen address, temporarily only supports IPv4,using the point score Word format
* @param int listening port
* @param type The type of connection, the optional SOCK_STREAM and SOCK_DGRAM
* @return int successfully returned when the listening fd,fails to return other value, and set errno
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
        LOGW(_("bind() failed(fd = %d, port=%d)：%s\n"), fd, port, strerror(errno));
        close(fd);
        return ret;
    }
    
    if (type == SOCK_STREAM) {
        ret = listen(fd, 1);
        if (ret < 0) {
            LOGW(_("listen() failed(fd = %d)：%s\n"), fd, strerror(errno));
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
* Connect to a server
* 
* @param const char* server address
* @param int server port
* @param int the type of connection, which can be SOCK_STREAM or SOCK_DGRAM
* @return int the connection is successful return fd, failure to return a negative number and set errno
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
