# clashlite (Linux / C)

一个 **最小可用** 的「类 Clash」透明代理核心原型（MVP），目标是把两件事跑通：

1) **FakeDNS**：本地 DNS 服务器对匹配域名返回“假 IP”（默认使用 `198.18.0.0/15` 地址池）并维护映射。
2) **透明代理**：配合 `iptables REDIRECT` 把本机 TCP 连接转发到本地端口；程序通过 `SO_ORIGINAL_DST` 取得原目标（可能是假 IP），再根据 FakeDNS 映射把连接转发到真实目标。

> 说明：
> - 目前实现 **IPv4 + TCP** 为主；UDP 仅用于 DNS。
> - 上游代理目前支持 **SOCKS5 (no-auth)** 或直连（可选）。
> - 这是“网络代理内核”，不是 WireGuard 那种 L3 VPN。它可以作为“系统级代理/VPN感”使用。

## 编译

```bash
cd clashlite
make
```

生成：`./clashlite`

## 配置

复制示例配置：

```bash
cp config.example.ini config.ini
```

关键配置项：
- `dns_listen`：FakeDNS 监听地址（建议 127.0.0.1:1053）
- `tproxy_listen`：透明代理监听地址（127.0.0.1:12345）
- `upstream_dns`：上游 DNS（例如 223.5.5.5:53）
- `socks5_upstream`：可选，上游 SOCKS5（例如 127.0.0.1:7890）
- `fake_range`：假 IP 池（默认 198.18.0.0/15）
- `fake_suffix_rules`：哪些域名走 FakeDNS（逗号分隔后缀），留空表示全部 A 查询都 Fake

## 运行（需要配合 iptables）

### 1) 启动 clashlite

```bash
./clashlite -c config.ini
```

### 2) 设置系统 DNS 指向 FakeDNS

如果你用 `systemd-resolved`，建议用 `resolvectl`：

```bash
sudo resolvectl dns lo 127.0.0.1
sudo resolvectl domain lo ~.
```

或者把 `/etc/resolv.conf` 指向 `127.0.0.1`（方式因发行版不同而异）。

### 3) iptables 透明转发（本机 OUTPUT REDIRECT）

脚本（可按需改）：

```bash
sudo bash scripts/iptables_setup.sh \
  --tproxy-port 12345 \
  --dns-port 1053 \
  --bypass-uid $(id -u)
```

> 重要：一定要 **绕过本程序自身**（bypass uid 或 mark），否则会造成回环。

清理：

```bash
sudo bash scripts/iptables_cleanup.sh
```

## 验证

```bash
# 让系统 DNS 走 FakeDNS 后
nslookup example.com 127.0.0.1 -port=1053

# 再 curl，看是否被透明代理接管并转发
curl -I https://example.com
```

## 你需要确认的“不确定点”（后续迭代前请回复）

为了避免我“猜需求”，请你回答：
1) 透明代理你更倾向 `iptables REDIRECT`（本机简单）还是 `TPROXY + policy routing`（更通用，可做网关）？
2) 需要 UDP 透明代理吗（QUIC/UDP应用）？
3) FakeDNS 规则：是“全部域名都 fake”，还是只 fake 某些列表（类似 Clash 的 ruleset）？
4) 上游你希望支持哪些协议：只 SOCKS5？还是需要 Shadowsocks/Vmess/Trojan ？

## 免责声明
仅用于你拥有授权的网络环境/合规用途。请遵守所在地法律法规与网络使用政策。
