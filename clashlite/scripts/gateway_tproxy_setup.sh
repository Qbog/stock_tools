#!/usr/bin/env bash
set -euo pipefail

# gateway_tproxy_setup.sh
#
# 目的：把“经过本机转发/路由”的 TCP/UDP 流量透明送入 clashlite。
#
# 这是 Linux 上实现 Clash "TUN/路由模式" 类似效果的常见做法：
# - 不真的创建 /dev/net/tun 并做 tun2socks（那需要用户态 TCP/IP 栈，工程量非常大）
# - 而是用内核现成的协议栈 + netfilter TPROXY，把流量交给用户态代理进程
#
# 要点：
# 1) policy routing：把带 fwmark 的包路由到 lo，使其能被本地进程接收
# 2) iptables mangle/TPROXY：把 PREROUTING 中的 tcp/udp 命中规则的流量标记并送到本地端口
#
# 用法：
#   sudo bash scripts/gateway_tproxy_setup.sh \
#     --tcp-port 12345 \
#     --udp-port 12346 \
#     --mark 1 \
#     --table 100
#
# 清理：
#   sudo bash scripts/gateway_tproxy_cleanup.sh --mark 1 --table 100

TCP_PORT=12345
UDP_PORT=12346
MARK=1
TABLE=100
CHAIN=CLASHLITE_TPROXY

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tcp-port) TCP_PORT="$2"; shift 2;;
    --udp-port) UDP_PORT="$2"; shift 2;;
    --mark) MARK="$2"; shift 2;;
    --table) TABLE="$2"; shift 2;;
    *) echo "Unknown arg: $1"; exit 2;;
  esac
done

# 1) 打开转发（做网关必须）
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# 2) policy routing
# - 给打标的包指定路由表
# - 路由表里把所有目的路由到 lo (local)，这样包会被交给本地 socket 接收
ip rule add fwmark "$MARK" lookup "$TABLE" 2>/dev/null || true
ip route add local 0.0.0.0/0 dev lo table "$TABLE" 2>/dev/null || true

# 3) iptables: 创建/刷新链
iptables -t mangle -N "$CHAIN" 2>/dev/null || true
iptables -t mangle -F "$CHAIN"

# 4) 放行一些不应代理的目的地（按需扩展）
# - 本机/回环
iptables -t mangle -A "$CHAIN" -d 127.0.0.0/8 -j RETURN
iptables -t mangle -A "$CHAIN" -d 224.0.0.0/4 -j RETURN
iptables -t mangle -A "$CHAIN" -d 255.255.255.255/32 -j RETURN

# 5) TPROXY TCP/UDP
# - --tproxy-mark 会同时设置 fwmark，必须和 policy routing 对应
iptables -t mangle -A "$CHAIN" -p tcp -j TPROXY --on-port "$TCP_PORT" --tproxy-mark "$MARK"/"$MARK"
iptables -t mangle -A "$CHAIN" -p udp -j TPROXY --on-port "$UDP_PORT" --tproxy-mark "$MARK"/"$MARK"

# 6) 挂到 PREROUTING（转发流量必经）
iptables -t mangle -C PREROUTING -j "$CHAIN" 2>/dev/null || iptables -t mangle -A PREROUTING -j "$CHAIN"

echo "[OK] gateway TPROXY installed: tcp->$TCP_PORT udp->$UDP_PORT mark=$MARK table=$TABLE"

echo "Reminder: configure LAN clients to use this machine as gateway, and set DNS to your FakeDNS (e.g. 127.0.0.1:1053) or run a LAN DNS forwarder."
