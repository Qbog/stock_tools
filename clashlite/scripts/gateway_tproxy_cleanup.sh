#!/usr/bin/env bash
set -euo pipefail

MARK=1
TABLE=100
CHAIN=CLASHLITE_TPROXY

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mark) MARK="$2"; shift 2;;
    --table) TABLE="$2"; shift 2;;
    *) echo "Unknown arg: $1"; exit 2;;
  esac
done

iptables -t mangle -D PREROUTING -j "$CHAIN" 2>/dev/null || true
iptables -t mangle -F "$CHAIN" 2>/dev/null || true
iptables -t mangle -X "$CHAIN" 2>/dev/null || true

ip rule del fwmark "$MARK" lookup "$TABLE" 2>/dev/null || true
ip route del local 0.0.0.0/0 dev lo table "$TABLE" 2>/dev/null || true

echo "[OK] gateway TPROXY rules removed"
