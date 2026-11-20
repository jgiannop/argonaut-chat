# Argonaut Chat

A private messenger for the desktop, like Signal, written in C++17 with Qt 6
Widgets: the same technology and the same look as
[Argonaut](https://github.com/jgiannop/argonaut), the file manager, and
[Argonaut Emails](https://github.com/jgiannop/argonaut-emails).

Version 0.0.1: the start. How Signal works, and what that means for this
program: [docs/how-signal-works.md](docs/how-signal-works.md).

## What it does now

- **Signal's layout**: the conversations at the left (newest first, the last
  message, the unread count), the chosen one at the right, messages as
  bubbles (yours at the right), a field to write in (Enter sends, Shift+Enter
  is a new line).
- **Note to Self**, always there.
- **Chatting with people, encrypted end to end** with the Signal Protocol's
  design (X3DH and the Double Ratchet, on libsodium): sign up with a
  username (no phone number), start a chat by someone's username, ✓ sent,
  ✓✓ delivered, white ✓✓ read. Messages wait on the server while the other
  person is offline, and in an outbox while you are.
- **Message requests**: a stranger's first message waits as a request;
  until you accept, they get no receipts at all. Delete it, or block them
  (their messages are then dropped, and they are not told).
- **Notifications and the tray**: a notification for each new message
  (View › Notifications: name and message, name only, nothing, or off), the
  unread count on the tray and dock icons; closing the window keeps it
  receiving in the tray, File › Quit ends it.
- **Safety numbers**: 60 digits to compare with someone; a warning when
  theirs changes.
- **The relay server**, `argonaut-chat-server`: accounts, public keys and
  encrypted messages until delivery; it cannot read any of them
  ([docs/protocol.md](docs/protocol.md)).
- **The history on this computer only, encrypted**: one SQLCipher file,
  `~/.local/share/Argonaut/argonaut-chat/chat.sqlite` (your messages and your
  keys), whose key is kept in the system keyring (GNOME Keyring or KWallet).
- **Themes**: Argonaut's themes, including theme files from
  `~/.local/share/argonaut/themes`.

## Not there yet

PQXDH (the
post-quantum start), contacts and profiles, groups, attachments, linked
devices, calls.

## Installing

A package for Ubuntu 24.04 and one for 26.04, built by
`packaging/build-debs.sh` (each in a clean container of its release, then
installed on a fresh one to check it) into `dist/`:

```bash
sudo apt install ./dist/argonaut-chat_0.0.1_ubuntu24.04_amd64.deb   # or ubuntu26.04
```

It brings in what it needs (Qt 6, libsodium, SQLCipher, QtKeychain), and
puts Argonaut Chat in the applications menu.

## Building

Dependencies (Ubuntu 24.04): `cmake ninja-build pkg-config qt6-base-dev
qt6-websockets-dev libsodium-dev libsqlcipher-dev qtkeychain-qt6-dev`.

```bash
cmake --preset debug
cmake --build build/debug
./build/debug/argonaut-chat
ctest --preset debug
```

## Trying it on one computer

The server, then two copies of the app with their own data folders (each
signs up with its own username; the server's address is `ws://127.0.0.1:8790`):

```bash
./build/debug/argonaut-chat-server --database /tmp/chat-server.sqlite
XDG_DATA_HOME=/tmp/alice ./build/debug/argonaut-chat
XDG_DATA_HOME=/tmp/bob ./build/debug/argonaut-chat
```

On a server, run it behind a reverse proxy that does TLS (Caddy or nginx),
with `--trust-proxy` so its rate limits see the real addresses; the app
accepts only `wss://` addresses, except to the same computer.
`deploy/setup-server.sh` does all of it on an Ubuntu 24.04 VPS:
[docs/deploy.md](docs/deploy.md).

## License

GPL-3.0-or-later.
