# mptunnel
MultiPath Tunnel (Simpified user space MPUDP)

[中文说明点此](README.zh_CN.md)

## ABOUT

MultiPath Tunnel is a multipath UDP implementation in user space. Like MultiPath TCP, you can establish
several connections from local to remote server.

MPTCP(MultiPath TCP) is a good idea to make network connection robust, but it only works on TCP. I was
searching for MPUDP implementation but got nothing, so I write this tool.


## CONCEPTION

```
                        .---- bridge server 1 ----.
                       /                            \
 Server A --- mpclient ------- bridge server 2 ------- mpserver --- Server B
                       \                            /
                        `---- bridge server 3 ----`
```

There are two server named Server A and Server B. The network connection between Server A and Server B is
unstable (with high packet loss ratio). Thus, we like to establish an multipath tunnel between Server A
and Server B, hoping the connection between Server A and Server B become more stable (decrease
packet loss ratio).

_mpclient_ is the client part of mptunnel, it could be run on ServerA. You must tell mpclient the
information of bridge servers. Once mpclient is started, it open a local UDP port for listen, forward
any packet to/from bridge servers.

_mpserver_ is the server part of mptunnel, it could be run on ServerB. You must tell mpserver the
information of Server B. Once mpserver is started, it will forward any packet to/from Server B.

Bridge servers is simple, it only forward packets from mpclient to mpserver, or packets from mpserver to
mpclient. You can use _nc_ or _socat_ to deploy a bridge server.


## EXAMPLE

I want to connection to my OpenVPN server, but the connection is unstable, packet loss ratio is high. The
TCP throughput over the OpenVPN tunnel is very small due to high packet loss ratio. To increase TCP
throughput (decrease packet loss ratio), I can run a MPUDP to OpenVPN server and establish OpenVPN connection
on it.

OpenVPN is listen on UDP port 1194, I run mpserver on OpenVPN server like this:

```
mpserver 2000 127.0.0.1 1194
```

On local, run mpclient:

```
mpclient mpclient.conf
```

Below is the content of mpclient.conf 

```
1.1.1.1 4000
bridge1.myhost.com 4000
bridge2.myhost.com 4000
```

1.1.1.1 is the IP of OpenVPN server. It's okay to use it as a bridge server.

On each bridge server, use _socat_ to forward packet:

```
socat udp-listen:4000 udp4:1.1.1.1:2000
```

Bridge server will listen on UDP port 3001, forward any recieved packet to 1.1.1.1:2000, and vice versa.


Now I make OpenVPN client to connect localhost:3000 which mpclient listening on, then OpenVPN will
establish an OpenVPN connection over MultiPath UDP tunnel.


## BUGS

* mptunnel add some control information into packets, including synchronous information. mpserver and mpclient must be start at the same time. If mpclient or mpserver terminated, you have to restart both mpserver and mpclient to reestablish the tunnel.
* Currently you can only specify signle target host. Any one knows is there any C library of SOCKS5 proxy? I think making mpclient as a SOCKS proxy server will make it more easy to use.


## DEPENDENCIES

To compile mptunnel, these libraries are required:

* libev
