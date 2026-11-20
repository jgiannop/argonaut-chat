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

## License

GPL-3.0-or-later.
