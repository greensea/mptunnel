# mptunnel
MultiPath Tunnel （多路 UDP 隧道的用户空间实现）

[README in English](README.md)

## 关于

mptunnel 是一个多路 UDP 在用户空间的实现（类似 MPTCP）。mptunnel 支持创建一个虚拟隧道，并将通过这个隧道中的包通过多条路径转发到目标服务器。

MPTCP 旨在提高网络稳定性，但它只支持 TCP 协议。似乎目前还没有 MPUDP 的实现，于是我就写了这个小工具。

## 原理

```
                        .---- 桥 1 ----.
                       /                \
 服务器 A  --- mpclient -------桥 2 ------ mpserver --- 服务器 B
                       \                /
                        `---- 桥 3 ----`
```

有两台服务器，分别是服务器 A 和服务器 B。它们之间的网络连接不是很稳定，丢包率很高。为了提高网络稳定性，我们希望降低丢包率。为达到这一目标，我们将 A 和 B 之间的通信复制多份，然后通过不同的路径发送出去。这样，除非几条路径上的数据包都丢了，A 和 B 之间的数据包才会真的丢失。只要有一条路径上的数据包能够到达目的地，那么 A 和 B 就不会感觉到丢包。

_mpclient_ 运行在服务器端（可以直接在服务器 B 上运行），他会接收从多条路径上发来的数据包，并将数据包转发给服务器 B。同时，它也会将服务器 B 的数据包复制多份，然后转发到各个桥。运行 mpserver 时，需要指定监听端口和目标服务器地址及端口。

_mpclient_ 运行在客户端，启动后，它会在本地监听一个端口，将收到的数据包复制多份，然后通过桥转发给目标服务器。启动 mpclient 时，需要指定远程桥地址和端口。

桥就是一个简单的数据包转发服务器，你可以用 _socat_ 或者 _nc_ 等工具来实现。桥应该原封不动地在 mpclient 和 mpserver 之间转发数据包。


## 例子

我有一台 OpenVPN 服务器，但是本机到 OpenVPN 服务器的网络连接丢包率很高，这很影响 OpenVPN 隧道之上的 TCP 连接的吞吐量。为了提高 TCP 吞吐量，就需要降低丢包率。于是，我们可以先建立一条 MPUDP 隧道，然后在这个隧道上再建立 OpenVPN 连接。

OpenVPN 在服务器的 1194 端口上监听。在 OpenVPN 上运行 mpserver：

```
mpserver 2000 127.0.0.1 1194
```

mpserver 会监听 2000 端口，并将数据包转发到本机的 1194 端口。


在本机上，运行 mpcilent：

```
mpclient mpclient.conf
```

mpclient.conf 的内容是

```
1.1.1.1 4000
bridge1.myhost.com 4000
bridge2.myhost.com 4000
```

1.1.1.1 是 OpenVPN 服务器的 IP 地址。把目标服务器当成一个桥是完全没有问题的。

在桥服务器上，用 socat 来转发数据包：

```
socat udp-listen:4000 udp4:1.1.1.1:2000
```

这样，桥服务器就会在监听端口 4000 和 1.1.1.1 的 2000 端口之间转发数据包。

现在，MPUDP 隧道已经建立起来了。接下来要把 OpenVPN 客户端配置文件中的服务器地址改为 127.0.0.1:3000。3000 是 mpclient 在本地监听的端口。启动 OpenVPN 后，OpenVPN 就会在 MPUDP 隧道上建立 OpenVPN 连接了。


## 已知问题

* mptunnel 会在转发的数据包中添加一些控制和包同步信息。如果 mpclient 或者 mpserver 其中的一个被停止了，那么必须重新启动 mpserver 和 mpclient 才能重建 MPUDP 隧道。

* 目前 mptunnel 只支持指定一个目标服务器。事实上我觉得将 mpclient 做成 SOCKS 代理的话会更好用，但是我找了很久也没发现好用的 C 语言的 SOCKS 库，自己实现一个又太麻烦了。有谁知道有什么好用的 SOCKS 库吗？

* mptunnel 自带数据加密功能，但加密速度较慢，在一台 AMD Athlon II P320 CPU 机器上实测，在 3 通道的情况下，实际数据流量仅为 3Mbps。如果你不需要 mptunnel 对数据进行加密，可以设定环境变量 MPTUNNEL_ENCRYPT=0 以禁用 mptunnel 自带的加密功能。在禁用加密功能后，吞吐量从 3Mbps 提高到了 300Mbps。

## 依赖关系

编译 mptunnel 时，需要先安装下面的库

* libev
