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
#
# Run it as root (or with sudo) from the uploaded source (see docs/deploy.md):
#
#   sudo bash deploy/setup-server.sh chat.example.org
#
# Run it again after uploading a newer source: it rebuilds, reinstalls and
# restarts. The database stays.
#
# Environment: PORT (default 8790), the server's port on 127.0.0.1;
# EMAIL, for Let's Encrypt's expiry notices (nginx only; optional).
set -euo pipefail

DOMAIN="${1:-}"
PORT="${PORT:-8790}"
EMAIL="${EMAIL:-}"
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
    *apache2* | *httpd*) die "ports 80/443 are Apache's ($holders). This script does Caddy and nginx only; nothing has been changed. Tell Claude, and it can add Apache." ;;
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
apt-get install -y -q --no-install-recommends "${packages[@]}"

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
