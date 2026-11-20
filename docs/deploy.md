# Running the server on a VPS

`deploy/setup-server.sh` sets up `argonaut-chat-server` on an Ubuntu 24.04
machine (made for a Hetzner VPS; any Ubuntu 24.04 will do):

- installs what it needs (Qt 6, libsodium, SQLCipher, and Caddy from Ubuntu);
- builds the server alone from the uploaded source (`-DCHAT_SERVER_ONLY=ON`);
- runs it under systemd as its own user `argonaut-chat`, listening on
  127.0.0.1:8790 only, sandboxed (`systemd-analyze security` 1.5), its
  database in `/var/lib/argonaut-chat` (0700);
- puts Caddy in front: `wss://<domain>`, with a Let's Encrypt certificate
  it gets and renews by itself, and the real addresses passed on for the
  rate limits (`--trust-proxy`). No access log is written.

## Before

1. A DNS **A record** (and AAAA, for IPv6) for the domain, e.g.
   `chat.example.org`, pointing at the server.
2. Ports **80 and 443** open: in the Hetzner Cloud Firewall, if you use one
   (the script opens them in `ufw` if `ufw` is on). Port 80 is needed for
   the certificate.

## Upload and run

From this computer (replace the address and the domain):

```bash
rsync -az --delete --filter=':- .gitignore' --exclude=.git/ \
    ~/dev/argonaut-chat/ root@SERVER_IP:/opt/argonaut-chat/
ssh root@SERVER_IP 'bash /opt/argonaut-chat/deploy/setup-server.sh chat.example.org'
```

It ends with a WebSocket handshake through Caddy: "wss://… works" means TLS,
the certificate, the proxy and the server all do. Then, in the app, sign up
with the server `wss://chat.example.org`.

## Updating

The same two commands: it rebuilds, reinstalls and restarts; the database
stays.

## On the server

```bash
journalctl -u argonaut-chat-server -f    # the server's log
systemctl restart argonaut-chat-server
journalctl -u caddy -f                   # certificates, the proxy
```

The site is `/etc/caddy/argonaut-chat.caddy`, imported at the end of
`/etc/caddy/Caddyfile`; other sites there are left alone.
