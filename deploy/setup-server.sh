#!/usr/bin/env bash
# Sets up argonaut-chat-server on an Ubuntu 24.04 machine (a Hetzner VPS):
#
#   - builds it from this source (the server alone: no widgets, no keyring)
#   - runs it under systemd as its own unprivileged user, on 127.0.0.1 only,
#     its database in /var/lib/argonaut-chat (0700)
#   - puts Caddy in front: TLS with a Let's Encrypt certificate, made and
#     renewed by itself, and wss://<domain> passed on to the server
#
# Run it as root from the uploaded source (see docs/deploy.md):
#
#   bash deploy/setup-server.sh chat.example.org
#
# Run it again after uploading a newer source: it rebuilds, reinstalls and
# restarts. The database stays.
#
# Environment: PORT (default 8790), the server's port on 127.0.0.1.
set -euo pipefail

DOMAIN="${1:-}"
PORT="${PORT:-8790}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE=argonaut-chat-server
ACCOUNT=argonaut-chat
STATE=/var/lib/argonaut-chat
BUILD=/var/cache/argonaut-chat-build
BIN=/usr/local/bin/argonaut-chat-server
SITE=/etc/caddy/argonaut-chat.caddy

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mWarning: %s\033[0m\n' "$*" >&2; }
die() { printf '\033[31mError: %s\033[0m\n' "$*" >&2; exit 1; }

# --- Checks ---

[[ $EUID -eq 0 ]] || die "run it as root: sudo bash $0 ${DOMAIN:-<domain>}"
[[ -n $DOMAIN ]] || die "usage: bash $0 <domain>   (e.g. chat.example.org)"
[[ $DOMAIN =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ && $DOMAIN == *.* ]] || die "not a domain name: $DOMAIN"
[[ $PORT =~ ^[0-9]+$ ]] && ((PORT > 0 && PORT < 65536)) || die "not a port: $PORT"
[[ -f $SRC/CMakeLists.txt && -f $SRC/src/server/main.cpp ]] || die "run it from the uploaded source: $SRC has no server source"

# shellcheck source=/dev/null
. /etc/os-release
if [[ ${ID:-} != ubuntu || ${VERSION_ID:-} != 24.04 ]]; then
    warn "made for Ubuntu 24.04; this is ${PRETTY_NAME:-unknown}. Going on, but packages may differ."
fi

# The domain must point here, or Caddy cannot get a certificate.
mine=$(hostname -I 2>/dev/null || true)
resolved=$(getent ahosts "$DOMAIN" | awk '{print $1}' | sort -u || true)
if [[ -z $resolved ]]; then
    warn "$DOMAIN does not resolve yet. Add an A (and AAAA) record for it pointing at this machine; Caddy will get the certificate once it does."
else
    here=no
    for ip in $resolved; do
        [[ " $mine " == *" $ip "* ]] && here=yes
    done
    [[ $here == yes ]] || warn "$DOMAIN points at $(echo "$resolved" | tr '\n' ' ')but this machine is $mine. Caddy gets the certificate only once it points here."
fi

# --- Packages ---

say "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    qt6-base-dev qt6-websockets-dev libsodium-dev libsqlcipher-dev \
    caddy ca-certificates curl

# --- Build ---

say "Building argonaut-chat-server"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHAT_SERVER_ONLY=ON
cmake --build "$BUILD"
install -m 0755 "$BUILD/argonaut-chat-server" "$BIN"
"$BIN" --version

# --- The service ---

say "The service: $SERVICE, on 127.0.0.1:$PORT"
if ! id -u "$ACCOUNT" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin "$ACCOUNT"
fi
cat > "/etc/systemd/system/$SERVICE.service" <<EOF
# Written by deploy/setup-server.sh (Argonaut Chat).
[Unit]
Description=Argonaut Chat relay server
After=network-online.target
Wants=network-online.target

[Service]
User=$ACCOUNT
Group=$ACCOUNT
ExecStart=$BIN --listen 127.0.0.1 --port $PORT --database $STATE/server.sqlite --trust-proxy
Environment=HOME=$STATE
StateDirectory=argonaut-chat
StateDirectoryMode=0700
UMask=0077
Restart=on-failure
RestartSec=2

# It needs nothing but its folder and the network.
NoNewPrivileges=yes
CapabilityBoundingSet=
AmbientCapabilities=
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
ProtectClock=yes
ProtectHostname=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
SystemCallArchitectures=native
SystemCallFilter=@system-service
SystemCallErrorNumber=EPERM
ProtectProc=invisible
ProcSubset=pid
RemoveIPC=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable "$SERVICE" >/dev/null
systemctl restart "$SERVICE"

# --- Caddy ---

say "Caddy: https://$DOMAIN -> 127.0.0.1:$PORT"
cat > "$SITE" <<EOF
# Written by deploy/setup-server.sh (Argonaut Chat): TLS here, the relay
# behind it on this machine. No access log: who talks when stays unwritten.
$DOMAIN {
	reverse_proxy 127.0.0.1:$PORT
}
EOF
if ! grep -qxF 'import argonaut-chat.caddy' /etc/caddy/Caddyfile; then
    printf '\n# Argonaut Chat (deploy/setup-server.sh)\nimport argonaut-chat.caddy\n' >> /etc/caddy/Caddyfile
fi
caddy validate --config /etc/caddy/Caddyfile --adapter caddyfile >/dev/null
systemctl enable caddy >/dev/null
systemctl reload-or-restart caddy

# The firewall, if ufw is on (a Hetzner Cloud Firewall, if any, is set in its console).
if command -v ufw >/dev/null && ufw status | grep -q '^Status: active'; then
    say "Opening ports 80 and 443 in ufw"
    ufw allow 80/tcp >/dev/null
    ufw allow 443/tcp >/dev/null
fi

# --- Checks ---

say "Checking"
sleep 2
systemctl is-active --quiet "$SERVICE" || die "$SERVICE is not running: journalctl -u $SERVICE"
ss -ltn | grep -q "127.0.0.1:$PORT " || die "nothing listens on 127.0.0.1:$PORT: journalctl -u $SERVICE"
echo "The server runs, on 127.0.0.1:$PORT only."
# A WebSocket handshake through Caddy: "101 Switching Protocols" means TLS,
# the certificate, the proxy and the server all work. (curl then waits on the
# open socket until --max-time; that is expected.)
code=$(curl -s -o /dev/null -w '%{http_code}' --http1.1 --max-time 5 \
    -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
    -H "Sec-WebSocket-Key: $(head -c 16 /dev/urandom | base64)" "https://$DOMAIN/" || true)
if [[ $code == 101 ]]; then
    echo "wss://$DOMAIN works, with a valid certificate."
else
    warn "wss://$DOMAIN does not answer yet (HTTP ${code:-none}). If the DNS was just set, give Caddy a minute (journalctl -u caddy -f), then check again with: bash $0 $DOMAIN"
fi

cat <<EOF

Done. In Argonaut Chat, sign up with the server:

    wss://$DOMAIN

Logs:     journalctl -u $SERVICE -f
Restart:  systemctl restart $SERVICE
Database: $STATE/server.sqlite
EOF
