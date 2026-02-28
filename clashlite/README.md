# clashlite (Linux / C)

一个 **类 Clash 核心原型（MVP）**，目标是用尽量少的依赖把三件事跑通：

1. **FakeDNS**（UDP DNS 服务器）：匹配规则的域名返回“假 IP”（默认 `198.18.0.0/15`），并维护映射。
2. **TCP 透明代理**：配合 `iptables` 把 TCP 流量透明送到本地端口；程序通过 `SO_ORIGINAL_DST` 取回原目标，然后按需走 FakeDNS 映射并转发。
3. **UDP 透明代理**：通过 `TPROXY + IP_ORIGDSTADDR` 拿到每个 UDP 包的原目标，再使用 **SOCKS5 UDP ASSOCIATE** 转发。

> 说明：
> - 这是“系统级透明代理/VPN感”，不是 WireGuard/OpenVPN 那种真正的 L3 VPN。
> - 你如果坚持“真正 /dev/net/tun + tun2socks”的 TUN 模式，需要用户态 TCP/IP 栈（工程量大）。本项目先用 Linux 内核协议栈 + TPROXY 实现路由模式。

---

## 编译

```bash
cd clashlite
make
```

生成：`./clashlite`

---

## 配置

复制示例：

```bash
cp config.example.ini config.ini
```

关键配置项：
- `dns_listen`：FakeDNS 监听（建议 `127.0.0.1:1053`）
- `tproxy_listen`：TCP 透明代理监听（例如 `0.0.0.0:12345` 用于网关模式）
- `udp_listen`：UDP 透明代理监听（例如 `0.0.0.0:12346`）
- `upstream_dns`：上游 DNS（用于解析真实 IP）
- `socks5_upstream`：上游 SOCKS5（UDP 必须；TCP 可选）
- `fake_range`：假 IP 池（默认 `198.18.0.0/15`）
- `fake_suffix_rules`：逗号分隔后缀规则；空表示全部 A 查询都 Fake
- `fake_rules_file`：可选规则文件（每行一个域名/后缀，支持 `#` 注释）

---

## 运行

```bash
./clashlite -c config.ini
```

### A) 仅代理本机（简单模式）

用 nat OUTPUT REDIRECT 把本机 TCP 送进代理：

```bash
sudo bash scripts/iptables_setup.sh \
  --tproxy-port 12345 \
  --dns-port 1053 \
  --bypass-uid $(id -u)
```

清理：

```bash
sudo bash scripts/iptables_cleanup.sh
```

> 该脚本只处理 TCP（OUTPUT）。UDP 透明代理更推荐走下面的“网关/路由模式”。

### B) 网关/路由模式（推荐，支持 UDP）

用 TPROXY 把 **转发流量**（PREROUTING）送入 clashlite：

```bash
sudo bash scripts/gateway_tproxy_setup.sh \
  --tcp-port 12345 \
  --udp-port 12346 \
  --mark 1 \
  --table 100
```

清理：

```bash
sudo bash scripts/gateway_tproxy_cleanup.sh --mark 1 --table 100
```

> 网关模式要求：
> - 其他设备把本机设为默认网关（或策略路由到本机）
> - `net.ipv4.ip_forward=1`

---

## DNS 指向 FakeDNS

如果你用 `systemd-resolved`：

```bash
sudo resolvectl dns lo 127.0.0.1
sudo resolvectl domain lo ~.
```

或者按发行版修改 `/etc/resolv.conf`/NetworkManager。

---

## 重要限制（MVP 必读）

- **仅 IPv4**
- UDP：目前为每个 (client, original_dst) 创建会话；在“多个 client 同时访问同一个远端 IP:PORT”的场景下，透明回包可能出现冲突（后续可通过更复杂的端口复用/源地址控制改进）。
- SOCKS5：只实现 **no-auth**。
- FakeDNS：只处理 A/IN；其他类型直接转发上游。

---

## 目录结构

- `src/dns.c`：FakeDNS（支持转发）
- `src/tproxy.c`：TCP 透明代理（SO_ORIGINAL_DST + 直连/SOCKS5）
- `src/udp_tproxy.c`：UDP 透明代理（TPROXY + IP_ORIGDSTADDR + SOCKS5 UDP ASSOCIATE）
- `scripts/`：iptables/policy routing 辅助脚本

---

## 合规声明

仅用于你拥有授权的网络环境/合规用途。请遵守所在地法律法规与网络使用政策。
