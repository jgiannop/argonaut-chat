# Running the server on a VPS

`deploy/setup-server.sh` sets up `argonaut-chat-server` on an Ubuntu 24.04
or 26.04 machine (made for a Hetzner VPS), next to any website already
there:

- installs what it needs (Qt 6, libsodium, SQLCipher; Caddy or certbot as
  needed, all from Ubuntu);
- builds the server alone from the uploaded source (`-DCHAT_SERVER_ONLY=ON`);
- runs it under systemd as its own user `argonaut-chat`, listening on
  127.0.0.1:8790 only, sandboxed (`systemd-analyze security` 1.5), its
  database in `/var/lib/argonaut-chat` (0700);
- serves `wss://<domain>` through the web server that already holds ports
  80 and 443, with its own Let's Encrypt certificate, the real addresses
  passed on for the rate limits (`--trust-proxy`), and no access log:

  | ports 80/443 held by | what it does |
  |---|---|
  | nothing | installs Caddy, which gets and renews the certificate itself |
  | Caddy | adds `/etc/caddy/argonaut-chat.caddy`, imported at the end of the Caddyfile |
  | nginx | adds the site `/etc/nginx/sites-available/argonaut-chat` for this domain only, and a certificate from certbot (webroot, renewed by certbot's timer); no other nginx file is touched, `nginx -t` runs before every reload, and a site nginx refuses is taken out again |
  | Apache, anything else | stops before changing anything |

  Other sites on the machine are left as they are. (Tested on Ubuntu 26.04
  with systemd and nginx already serving another site on 80 and 443: that
  site kept answering, its files unchanged.)

## Before

1. A name for the server that points at it:
   - your own, e.g. `chat.example.org`, with a DNS **A record** to the
     server's address; or
   - for now, a free name from [sslip.io](https://sslip.io) that needs no
     setup: `188-34-165-32.sslip.io` already points at `188.34.165.32`.
2. Ports **80 and 443** open: in the Hetzner Cloud Firewall, if you use one
   (the script opens them in `ufw` if `ufw` is on). Port 80 is needed for
   the certificate.

## Upload and run

From this computer, as your user on the server (with sudo):

```bash
rsync -az --delete --filter=':- .gitignore' --exclude=.git/ \
    ~/dev/argonaut-chat/ jgiannop@188.34.165.32:argonaut-chat/
ssh -t jgiannop@188.34.165.32 'sudo bash ~/argonaut-chat/deploy/setup-server.sh 188-34-165-32.sslip.io'
```

(`-t` lets sudo ask for your password.) For nginx's certificate,
`EMAIL=you@example.org` before `bash` gets you Let's Encrypt's expiry
notices; without it there are none (certbot renews by itself anyway).

It ends with a WebSocket handshake through the web server: "wss://… works"
means TLS, the certificate, the proxy and the server all do. The app's
address is built in (`src/core/AppConfig.h`, now
`wss://188-34-165-32.sslip.io`): another one needs that changed and a new
release; the app then moves existing accounts to it when it starts.

## Updating, or moving to your own domain

The same two commands (with the new domain, if it changed): it rebuilds,
reinstalls and restarts; the database stays.

## Backups

Each night at about 03:30 (`argonaut-chat-backup.timer`), the database is
copied while the server runs, checked, compressed, and kept in
`/var/backups/argonaut-chat` (root only); the newest 14 stay. The setup
makes one at once, too. What is in it: accounts, the devices' public keys,
and the messages still waiting for delivery (encrypted; the server cannot
read them).

These copies are on the same machine: they help against a damaged database
or a mistake, not against losing the machine. For that, turn on Hetzner's
backups (in the Cloud Console), or copy the folder elsewhere now and then:

```bash
rsync -a --rsync-path='sudo rsync' jgiannop@SERVER:/var/backups/argonaut-chat/ ~/argonaut-chat-backups/
```

A backup now: `sudo systemctl start argonaut-chat-backup`. The list:
`sudo ls -l /var/backups/argonaut-chat`.

### Restoring

```bash
sudo systemctl stop argonaut-chat-server
db=/var/lib/argonaut-chat/server.sqlite
sudo rm -f $db-wal $db-shm
sudo sh -c "gunzip -c /var/backups/argonaut-chat/server-YYYY-MM-DD-HHMMSS.sqlite.gz > $db"
sudo chown argonaut-chat:argonaut-chat $db && sudo chmod 600 $db
sudo systemctl start argonaut-chat-server
```

Messages that arrived after that backup and were still waiting are lost;
accounts made after it are gone (their owners sign up again).

## On the server

```bash
journalctl -u argonaut-chat-server -f    # the server's log
sudo systemctl restart argonaut-chat-server
journalctl -u caddy -f                   # or: journalctl -u nginx
```
