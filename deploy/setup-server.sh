#!/usr/bin/env bash
# Sets up argonaut-chat-server on an Ubuntu 24.04 or 26.04 machine (a Hetzner
# VPS), next to any website already there:
#
#   - builds it from this source (the server alone: no widgets, no keyring)
#   - runs it under systemd as its own unprivileged user, on 127.0.0.1 only,
#     its database in /var/lib/argonaut-chat (0700)
#   - backs that database up each night (deploy/argonaut-chat-backup: 14
#     kept, in /var/backups/argonaut-chat, root only)
#   - serves wss://<domain> through the web server that already holds ports
#     80/443, with its own Let's Encrypt certificate:
#       * nothing there yet: installs Caddy (it gets the certificate itself)
#       * Caddy:  adds one site file, imported at the end of the Caddyfile
#       * nginx:  adds one site file for <domain> only, and a certificate
#                 from certbot (webroot: no other nginx file is touched);
#                 `nginx -t` before every reload, undone if it fails
#       * Apache or anything else: stops before changing anything
#     The other sites on the machine are left as they are.
#   - a relay for calls (coturn: TURN on UDP port 3478, the relayed
#     media on UDP 49160-49200), with passwords the server hands out for 12
#     hours (a secret the two share, in /etc/argonaut-chat); UDP only, never
#     into private networks. RELAY=no leaves it out. It
#     stops before changing anything if another program has port 3478, or
#     coturn is set up here for something else.
#
# Run it as root (or with sudo) from the uploaded source (see docs/deploy.md):
#
#   sudo bash deploy/setup-server.sh chat.example.org
#
# Run it again after uploading a newer source: it rebuilds, reinstalls and
# restarts. The database stays.
#
# Environment: PORT (default 8790), the server's port on 127.0.0.1;
# EMAIL, for Let's Encrypt's expiry notices (nginx only; optional);
# RELAY=no: no relay for calls (calls then connect only directly).
set -euo pipefail

DOMAIN="${1:-}"
PORT="${PORT:-8790}"
EMAIL="${EMAIL:-}"
RELAY="${RELAY:-yes}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVICE=argonaut-chat-server
ACCOUNT=argonaut-chat
STATE=/var/lib/argonaut-chat
BUILD=/var/cache/argonaut-chat-build
BIN=/usr/local/bin/argonaut-chat-server
CADDY_SITE=/etc/caddy/argonaut-chat.caddy
NGINX_SITE=/etc/nginx/sites-available/argonaut-chat
NGINX_LINK=/etc/nginx/sites-enabled/argonaut-chat
ACME_ROOT=/var/www/argonaut-chat-acme
CONFIG=/etc/argonaut-chat
RELAY_SECRET=$CONFIG/relay-secret
TURN_CONF=/etc/turnserver.conf
TURN_MARK="# Written by deploy/setup-server.sh (Argonaut Chat)."
RELAY_PORT=3478
RELAY_MIN=49160
RELAY_MAX=49200

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mWarning: %s\033[0m\n' "$*" >&2; }
die() { printf '\033[31mError: %s\033[0m\n' "$*" >&2; exit 1; }

# --- Checks (nothing is changed until they pass) ---

[[ $EUID -eq 0 ]] || die "run it as root: sudo bash $0 ${DOMAIN:-<domain>}"
[[ -n $DOMAIN ]] || die "usage: sudo bash $0 <domain>   (e.g. chat.example.org)"
[[ $DOMAIN =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ && $DOMAIN == *.* ]] || die "not a domain name: $DOMAIN"
[[ $PORT =~ ^[0-9]+$ ]] && ((PORT > 1023 && PORT < 65536)) || die "not a port above 1023: $PORT"
[[ -f $SRC/CMakeLists.txt && -f $SRC/src/server/main.cpp ]] || die "run it from the uploaded source: $SRC has no server source"

# shellcheck source=/dev/null
. /etc/os-release
if [[ ${ID:-} != ubuntu || (${VERSION_ID:-} != 24.04 && ${VERSION_ID:-} != 26.04) ]]; then
    warn "made for Ubuntu 24.04 and 26.04; this is ${PRETTY_NAME:-unknown}. Going on, but packages may differ."
fi

# The port must be free (or ours, from an earlier run).
if ss -ltnH "sport = :$PORT" | grep -q . && ! systemctl is-active --quiet "$SERVICE"; then
    die "port $PORT is taken by something else on this machine; choose another: PORT=8791 sudo bash $0 $DOMAIN"
fi

# Who serves ports 80 and 443 now: that one gets our site too.
holders=$(ss -ltnpH '( sport = :80 or sport = :443 )' | grep -o 'users:(("[^"]*"' | cut -d'"' -f2 | sort -u | tr '\n' ' ')
holders=${holders% }
case "$holders" in
    "") WEB=caddy-new ;;
    caddy) WEB=caddy ;;
    nginx) WEB=nginx ;;
    *apache2* | *httpd*) die "ports 80/443 are Apache's ($holders). This script does Caddy and nginx only; nothing has been changed. Apache support can be added to this script." ;;
    *) die "ports 80/443 are held by: $holders. This script knows Caddy and nginx only; nothing has been changed." ;;
esac
case $WEB in
    caddy-new) echo "Ports 80/443 are free: Caddy will be installed for $DOMAIN." ;;
    caddy) echo "Caddy serves ports 80/443: $DOMAIN is added to it, the other sites left alone." ;;
    nginx) echo "nginx serves ports 80/443: $DOMAIN is added as its own site, the other sites left alone." ;;
esac
if [[ $WEB == nginx && -e /etc/nginx/sites-enabled ]] && grep -rqsw -- "server_name.*$DOMAIN" /etc/nginx/sites-enabled/ --exclude=argonaut-chat; then
    die "another nginx site already uses $DOMAIN; nothing has been changed."
fi

# The relay's port: free, or coturn's as set up by an earlier run.
[[ $RELAY == yes || $RELAY == no ]] || die "RELAY is yes or no, not: $RELAY"
if [[ $RELAY == yes ]]; then
    relay_holders=$({ ss -lunpH "sport = :$RELAY_PORT"; ss -ltnpH "sport = :$RELAY_PORT"; } |
        { grep -o 'users:(("[^"]*"' || true; } | cut -d'"' -f2 | sort -u | tr '\n' ' ')
    relay_holders=${relay_holders% }
    case "$relay_holders" in
        "" | turnserver) ;;
        *) die "port $RELAY_PORT (the relay's) is held by: $relay_holders; nothing has been changed. RELAY=no sets up the server without a relay." ;;
    esac
    if [[ -f $TURN_CONF ]] && ! head -1 "$TURN_CONF" | grep -qxF "$TURN_MARK" &&
        grep -qE '^[[:space:]]*[a-z]' "$TURN_CONF"; then
        die "coturn is already set up here ($TURN_CONF is not ours); nothing has been changed. RELAY=no leaves it alone."
    fi
fi

# The domain must point here, or no certificate can be had.
mine=$(hostname -I 2>/dev/null || true)
resolved=$(getent ahosts "$DOMAIN" | awk '{print $1}' | sort -u || true)
if [[ -z $resolved ]]; then
    [[ $WEB == nginx ]] && die "$DOMAIN does not resolve. Add an A record pointing at this machine first."
    warn "$DOMAIN does not resolve yet. Add an A record for it pointing at this machine; Caddy gets the certificate once it does."
else
    here=no
    for ip in $resolved; do
        [[ " $mine " == *" $ip "* ]] && here=yes
    done
    if [[ $here != yes ]]; then
        [[ $WEB == nginx ]] && die "$DOMAIN points at $(echo "$resolved" | tr '\n' ' ')but this machine is $mine; fix the DNS first."
        warn "$DOMAIN points at $(echo "$resolved" | tr '\n' ' ')but this machine is $mine. Caddy gets the certificate only once it points here."
    fi
fi

# --- Packages ---

say "Installing packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
packages=(build-essential cmake ninja-build pkg-config
    qt6-base-dev qt6-websockets-dev libsodium-dev libsqlcipher-dev ca-certificates curl sqlite3)
case $WEB in
    caddy-new | caddy) packages+=(caddy) ;;
    nginx) command -v certbot >/dev/null || packages+=(certbot) ;;
esac
if [[ $RELAY == yes ]]; then packages+=(coturn); fi
apt-get install -y -q --no-install-recommends "${packages[@]}"
# coturn starts as it is installed; not until it has our configuration (below).
if [[ $RELAY == yes ]] && ! head -1 "$TURN_CONF" 2>/dev/null | grep -qxF "$TURN_MARK"; then
    systemctl stop coturn 2>/dev/null || true
fi

# --- Build ---

say "Building argonaut-chat-server"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHAT_SERVER_ONLY=ON
# As many compilers at once as memory allows (about 700 MB each), so a small
# machine never runs out and the other sites on it are never at risk.
available_mb=$(awk '/MemAvailable/ {print int($2 / 1024)}' /proc/meminfo)
jobs=$((available_mb / 700))
((jobs > $(nproc))) && jobs=$(nproc)
((jobs < 1)) && jobs=1
echo "Building with $jobs job(s) ($available_mb MB free)."
nice -n 10 cmake --build "$BUILD" -j "$jobs"
install -m 0755 "$BUILD/argonaut-chat-server" "$BIN"
LANG=C.UTF-8 "$BIN" --version

# --- The service ---

say "The service: $SERVICE, on 127.0.0.1:$PORT"
if ! id -u "$ACCOUNT" >/dev/null 2>&1; then
    useradd --system --home-dir "$STATE" --no-create-home --shell /usr/sbin/nologin "$ACCOUNT"
fi

# The relay for calls (coturn), sharing a secret with the server.
relay_args=""
if [[ $RELAY == yes ]]; then
    say "The relay for calls: coturn on port $RELAY_PORT (media on UDP $RELAY_MIN-$RELAY_MAX)"
    install -d -m 0750 -o root -g "$ACCOUNT" "$CONFIG"
    if [[ ! -s $RELAY_SECRET ]]; then
        (umask 077 && head -c 32 /dev/urandom | base64 | tr -d '\n=' > "$RELAY_SECRET")
    fi
    chgrp "$ACCOUNT" "$RELAY_SECRET"
    chmod 0640 "$RELAY_SECRET"
    secret=$(cat "$RELAY_SECRET")
    if [[ -f $TURN_CONF && ! -f $TURN_CONF.before-argonaut-chat ]]; then
        cp -p "$TURN_CONF" "$TURN_CONF.before-argonaut-chat"
    fi
    {
        echo "$TURN_MARK"
        cat <<TURN
# TURN for Argonaut Chat's calls: only their encrypted media passes. The
# passwords come from argonaut-chat-server (the TURN REST scheme, this secret).
listening-port=$RELAY_PORT
min-port=$RELAY_MIN
max-port=$RELAY_MAX
realm=$DOMAIN
use-auth-secret
static-auth-secret=$secret
fingerprint
no-tls
no-dtls
no-tcp
no-cli
no-tcp-relay
no-multicast-peers
no-software-attribute
stale-nonce=600
total-quota=100
user-quota=4
syslog
simple-log
# Never into private and reserved networks (this machine's public address
# stays allowed: two callers both relayed here reach each other through it;
# UDP only, no-tcp-relay).
denied-peer-ip=0.0.0.0-0.255.255.255
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=100.64.0.0-100.127.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.0.0.0-192.0.0.255
denied-peer-ip=192.0.2.0-192.0.2.255
denied-peer-ip=192.88.99.0-192.88.99.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=198.18.0.0-198.19.255.255
denied-peer-ip=198.51.100.0-198.51.100.255
denied-peer-ip=203.0.113.0-203.0.113.255
denied-peer-ip=224.0.0.0-255.255.255.255
denied-peer-ip=::1
denied-peer-ip=64:ff9b::-64:ff9b::ffff:ffff
denied-peer-ip=fc00::-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff
denied-peer-ip=fe80::-febf:ffff:ffff:ffff:ffff:ffff:ffff:ffff
TURN
    } > "$TURN_CONF.new"
    chown root:turnserver "$TURN_CONF.new"
    chmod 0640 "$TURN_CONF.new"
    mv "$TURN_CONF.new" "$TURN_CONF"
    if [[ -f /etc/default/coturn ]] && ! grep -qx 'TURNSERVER_ENABLED=1' /etc/default/coturn; then
        echo 'TURNSERVER_ENABLED=1' >> /etc/default/coturn
    fi
    systemctl enable coturn >/dev/null
    systemctl restart coturn
    relay_args=" --relay turn:$DOMAIN:$RELAY_PORT --relay-secret-file $RELAY_SECRET"
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
ExecStart=$BIN --listen 127.0.0.1 --port $PORT --database $STATE/server.sqlite --trust-proxy$relay_args
Environment=HOME=$STATE
Environment=LANG=C.UTF-8
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

# --- Nightly backup ---

say "A nightly backup of the database"
install -m 0755 "$SRC/deploy/argonaut-chat-backup" /usr/local/sbin/argonaut-chat-backup
cat > /etc/systemd/system/argonaut-chat-backup.service <<EOF
# Written by deploy/setup-server.sh (Argonaut Chat).
[Unit]
Description=Back up the Argonaut Chat server's database

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/argonaut-chat-backup
Nice=10
IOSchedulingClass=idle
PrivateTmp=yes
ProtectHome=yes
NoNewPrivileges=yes
EOF
cat > /etc/systemd/system/argonaut-chat-backup.timer <<EOF
# Written by deploy/setup-server.sh (Argonaut Chat).
[Unit]
Description=Back up the Argonaut Chat server's database each night

[Timer]
OnCalendar=*-*-* 03:30
RandomizedDelaySec=30min
Persistent=true

[Install]
WantedBy=timers.target
EOF
systemctl daemon-reload
systemctl enable --now argonaut-chat-backup.timer >/dev/null

# --- The web server in front ---

setup_caddy() {
    say "Caddy: https://$DOMAIN -> 127.0.0.1:$PORT"
    cat > "$CADDY_SITE" <<EOF
# Written by deploy/setup-server.sh (Argonaut Chat): TLS here, the relay
# behind it on this machine. No access log: who talks when stays unwritten.
$DOMAIN {
	reverse_proxy 127.0.0.1:$PORT
}
EOF
    local caddyfile=/etc/caddy/Caddyfile backup
    backup=$(mktemp)
    cp "$caddyfile" "$backup"
    if ! grep -qxF 'import argonaut-chat.caddy' "$caddyfile"; then
        printf '\n# Argonaut Chat (deploy/setup-server.sh)\nimport argonaut-chat.caddy\n' >> "$caddyfile"
    fi
    if ! caddy validate --config "$caddyfile" --adapter caddyfile >/dev/null 2>&1; then
        cp "$backup" "$caddyfile"
        rm -f "$CADDY_SITE" "$backup"
        die "Caddy's configuration did not validate with our site; it is back as it was. (caddy validate --config $caddyfile)"
    fi
    rm -f "$backup"
    systemctl enable caddy >/dev/null
    systemctl reload-or-restart caddy
}

# Our nginx site; with `tls` once the certificate is there. Tested before the
# reload, and taken out again if nginx does not accept it.
write_nginx_site() {
    local tls=$1
    {
        cat <<EOF
# Written by deploy/setup-server.sh (Argonaut Chat): $DOMAIN only.
server {
    listen 80;
    listen [::]:80;
    server_name $DOMAIN;
    access_log off;
    location /.well-known/acme-challenge/ {
        root $ACME_ROOT;
    }
EOF
        if [[ $tls == yes ]]; then
            cat <<'EOF'
    location / {
        return 301 https://$host$request_uri;
    }
}
EOF
            cat <<EOF

server {
    listen 443 ssl;
    listen [::]:443 ssl;
    server_name $DOMAIN;
    access_log off;
    ssl_certificate /etc/letsencrypt/live/$DOMAIN/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/$DOMAIN/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:$PORT;
        proxy_http_version 1.1;
EOF
            cat <<'EOF'
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        # Only the address nginx saw (the server trusts the last one).
        proxy_set_header X-Forwarded-For $remote_addr;
        # A chat connection stays open and may be quiet for long.
        proxy_read_timeout 1d;
        proxy_send_timeout 1d;
    }
}
EOF
        else
            echo "}"
        fi
    } > "$NGINX_SITE.new"
    local previous=""
    if [[ -f $NGINX_SITE ]]; then
        previous=$(mktemp)
        cp "$NGINX_SITE" "$previous"
    fi
    mv "$NGINX_SITE.new" "$NGINX_SITE"
    ln -sf "$NGINX_SITE" "$NGINX_LINK"
    if ! nginx -t >/dev/null 2>&1; then
        local why
        why=$(nginx -t 2>&1 | tail -3)
        if [[ -n $previous ]]; then mv "$previous" "$NGINX_SITE"; else rm -f "$NGINX_SITE" "$NGINX_LINK"; fi
        die "nginx did not accept our site, so it is back as it was (the other sites were never touched):
$why"
    fi
    if [[ -n $previous ]]; then rm -f "$previous"; fi
    systemctl reload nginx
}

setup_nginx() {
    say "nginx: https://$DOMAIN -> 127.0.0.1:$PORT"
    [[ -d /etc/nginx/sites-available && -d /etc/nginx/sites-enabled ]] ||
        die "this nginx has no sites-available/sites-enabled; nothing has been changed in it."
    nginx -t >/dev/null 2>&1 || die "nginx's configuration is already failing its test (nginx -t), before any change of ours; fix that first."
    mkdir -p "$ACME_ROOT"
    if [[ ! -f /etc/letsencrypt/live/$DOMAIN/fullchain.pem ]]; then
        write_nginx_site no
        say "Getting a certificate for $DOMAIN from Let's Encrypt"
        local who=(--register-unsafely-without-email)
        if [[ -n $EMAIL ]]; then who=(--email "$EMAIL"); fi
        certbot certonly --webroot -w "$ACME_ROOT" -d "$DOMAIN" --non-interactive --agree-tos "${who[@]}" \
            --deploy-hook "systemctl reload nginx" ||
            die "no certificate for $DOMAIN (is port 80 open to the internet, and does the name point here?). Our port-80 site stays, harmless; run the script again once fixed."
    fi
    write_nginx_site yes
}

case $WEB in
    caddy-new | caddy) setup_caddy ;;
    nginx) setup_nginx ;;
esac

# The firewall, if ufw is on (a Hetzner Cloud Firewall, if any, is set in its console).
if command -v ufw >/dev/null && ufw status | grep -q '^Status: active'; then
    say "Opening ports 80 and 443 in ufw"
    ufw allow 80/tcp >/dev/null
    ufw allow 443/tcp >/dev/null
    if [[ $RELAY == yes ]]; then
        say "Opening the relay's ports in ufw"
        ufw allow "$RELAY_PORT/udp" >/dev/null
        ufw allow "$RELAY_MIN:$RELAY_MAX/udp" >/dev/null
    fi
fi

# --- Checks ---

say "Checking"
sleep 2
systemctl is-active --quiet "$SERVICE" || die "$SERVICE is not running: journalctl -u $SERVICE"
ss -ltnH "sport = :$PORT" | grep -q "127.0.0.1:$PORT" || die "nothing listens on 127.0.0.1:$PORT: journalctl -u $SERVICE"
echo "The server runs, on 127.0.0.1:$PORT only."
if systemctl start argonaut-chat-backup.service; then
    echo "A backup now: $(ls -1t /var/backups/argonaut-chat/server-*.sqlite.gz 2>/dev/null | head -1)"
else
    warn "the backup did not work: journalctl -u argonaut-chat-backup"
fi
if [[ $RELAY == yes ]]; then
    if systemctl is-active --quiet coturn && ss -lunH "sport = :$RELAY_PORT" | grep -q .; then
        echo "The relay for calls runs, on port $RELAY_PORT."
    else
        warn "the relay for calls is not running: journalctl -u coturn"
    fi
fi
# A WebSocket handshake through the web server: "101 Switching Protocols"
# means TLS, the certificate, the proxy and the server all work. (curl then
# waits on the open socket until --max-time; that is expected.)
code=$(curl -s -o /dev/null -w '%{http_code}' --http1.1 --max-time 5 \
    -H 'Connection: Upgrade' -H 'Upgrade: websocket' -H 'Sec-WebSocket-Version: 13' \
    -H "Sec-WebSocket-Key: $(head -c 16 /dev/urandom | base64)" "https://$DOMAIN/" || true)
if [[ $code == 101 ]]; then
    echo "wss://$DOMAIN works, with a valid certificate."
else
    warn "wss://$DOMAIN does not answer yet (HTTP ${code:-none}). If the DNS was just set, give it a minute, then run this again."
fi

cat <<EOF

Done. In Argonaut Chat, sign up with the server:

    wss://$DOMAIN

Logs:     journalctl -u $SERVICE -f
Restart:  systemctl restart $SERVICE
Database: $STATE/server.sqlite
Backups:  /var/backups/argonaut-chat (each night, the newest 14 kept)
EOF
if [[ $RELAY == yes ]]; then
    cat <<EOF
Relay:    journalctl -u coturn -f
          With a Hetzner Cloud Firewall, allow there too (inbound):
          UDP $RELAY_PORT and UDP $RELAY_MIN-$RELAY_MAX.
EOF
fi
