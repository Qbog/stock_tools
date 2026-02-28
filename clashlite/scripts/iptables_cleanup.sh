#!/usr/bin/env bash
set -euo pipefail

CHAIN=CLASHLITE

# remove hook
iptables -t nat -D OUTPUT -p tcp -j "$CHAIN" 2>/dev/null || true

# flush and delete
iptables -t nat -F "$CHAIN" 2>/dev/null || true
iptables -t nat -X "$CHAIN" 2>/dev/null || true

echo "[OK] iptables rules removed"
