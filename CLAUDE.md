# CLAUDE.md

Guidance for Claude Code in this repository.

## Project

Argonaut Chat: a private messenger like Signal, in C++17 and Qt 6 Widgets, a sibling of Argonaut (`~/dev/argonaut`, the file manager) and Argonaut Emails (`~/dev/argonaut-emails`), with the same look and the same rules. Minimum Qt 6.4 (Ubuntu 24.04). License GPL-3.0-or-later. How Signal works, and what the design follows: `docs/how-signal-works.md`.

## Build and test

```bash
cmake --preset debug && cmake --build build/debug
ctest --preset debug                       # all tests
./build/debug/tests/tst_store noteToSelfAndMessages
```

The debug build uses `-Wall -Wextra -Wshadow=local` and **is free of warnings: keep it so.** Every new source goes into `CMakeLists.txt`.

Tests are Qt Test, one line each in `tests/CMakeLists.txt` (`chat_test(name [LIBRARY chat_app])`). **Every test's `main()` is `CHAT_TEST_MAIN`** (`tests/TestSupport.h`): a private HOME and XDG folders, offscreen, so tests never touch the real settings or chat store. `CHAT_TEST_GRAB=<file.png> tst_mainwindow writeANoteToSelf` saves a picture of the window.

## Architecture

- `chat_core` (`src/core`, no widgets, all of it tested):
  - `ChatStore`: SQLite (driver `QSQLCIPHER`) in `$XDG_DATA_HOME/Argonaut/argonaut-chat/chat.sqlite` (0600, folder 0700): `conversations` (kind `self`/`direct`/`group`, title, peer, last activity) and `messages` (author empty = mine, body, sent_at in ms UTC, status sending/sent/delivered/read/failed or received, seen). Note to Self is made on first use. Signals `conversationsChanged`, `messagesChanged(conversation)`. Bind `text()` (never a null QString). **Encrypted with SQLCipher** when given a key (`ChatStore(file, key)`; no key = plain, tests only); a plain file is encrypted in place (`sqlcipher_export`).
  - `CipherSql`: our Qt SQL driver `QSQLCIPHER` over SQLCipher's C API (Qt's QSQLITE uses plain libsqlite3; **never load QSQLITE in our processes**: same symbols). Forward-only results, positional `?` binds, the key as `x'hex'` raw key; `isPlainSqlite`, `encryptFile`. The server's store uses it without a key.
  - The cryptography, all on libsodium (linked privately through pkg-config): `Crypto` (random, SHA-512, HMAC-SHA256, HKDF per RFC 5869 — 1.0.18 has none —, X25519 `DhKeyPair`/`dh()` refusing low-order keys, `IdentityKeyPair` = Ed25519 converted to X25519 for DH, XChaCha20-Poly1305 `seal`/`open`); `X3dh` (signed and one-time prekeys, `PreKeyBundle`, `initiate`/`respond`, AD = IK_A | IK_B); `RatchetSession` (the Double Ratchet per Signal's spec; wire format version | ratchet key | PN | N | ciphertext; skipped keys up to `MaxSkip`; a message that fails changes nothing; `serialize` holds secrets: local encrypted DB only); `SafetyNumber` (Signal's numeric fingerprint, 60 digits). Tests: `tst_crypto` (RFC 5869 vectors, X3DH, bad bundles, safety numbers), `tst_ratchet`. Not yet: PQXDH, locked memory for keys.
  - The protocol (docs/protocol.md): `Protocol` (usernames, `Content` JSON padded to 160-byte blocks, binary `CipherMessage` — plain or prekey with the X3DH header —, JSON `PreKeyBundle` and `Envelope`; every decoder strict) and `Session` (X3DH + ratchet + CipherMessage: `initiate`, `accept` with prekey lookups, prekey messages until the first answer). Test: `tst_protocol`.
  - `ChatStore` also: `uid` (the wire id; added to old databases by `ALTER TABLE`), `directConversation`, `outbox()` (mine in "sending"), `advanceStatus` (receipts: only forward), `unseenUids`, `database()` (for KeyStore); conversations' `state` ('accepted'/'request', added by `ALTER TABLE`), `setAccepted`, `deleteConversation` (never Note to Self), `blocked` table (`block`/`unblock`/`isBlocked`).
  - `KeyStore` (the same file, so encrypted with it): the account (username, device, server, identity secret, registered), signed and one-time prekeys with secrets, sessions per (username, device), identities (trust on first use; `changed`, `verified`).
  - `Messenger` (the engine: `signUp`, `start`/`stop`, reconnect with backoff, `startConversation`, `send` with an outbox, `markRead` with read receipts, delivered receipts, **message requests** (a stranger's first message opens a request: no receipts at all until `acceptRequest`; writing to them accepts; `deleteConversation`; `block` drops their messages silently and deletes the request), dedupe by uid, prekey top-up below 20, signed prekey rotation (every 7 days on sign-in and every 6 h online; old ones kept 30 days after replacement; saved before upload with `uploaded` = 0, re-sent until the server has it; `setClock` for tests), `safetyNumber`, `safetyNumberChanged`). Test: `tst_messenger` (two devices through the real server, restarts, reconnect, top-up).
  - `ChatClient` (QWebSocket: `registerAccount`/`login` by a signed challenge, `fetchKeys` + `bundles()`, `send`, `acknowledge`, `uploadPreKeys`; `envelopeReceived`; `wss://`, or `ws://` to loopback only; TLS errors never ignored).
- `chat_server` (`src/server`) and `argonaut-chat-server` (`--listen` 127.0.0.1, `--port` 8790, `--database`, `--trust-proxy` for X-Forwarded-For from loopback): `RateLimiter` (token buckets, injectable clock) and `ChatServer::Limits` (docs/protocol.md table; one-time prekeys per device then bundles without; tests set generous limits in `init()` and a fake clock `m_now`); `ServerStore` (SQLite 0600: accounts, devices' public keys, one-time prekeys ≤ 200, the queue ≤ 10000 per device) and `ChatServer` (`QWebSocketServer`, the requests in docs/protocol.md, every request checked, a connection that sends rubbish closed). Test `tst_server` runs the real server on 127.0.0.1 with clients exchanging encrypted messages (`chat_test(server LIBRARY chat_server)`).
- Deploying: `deploy/setup-server.sh <domain>` (root, Ubuntu 24.04: apt packages, builds with `-DCHAT_SERVER_ONLY=ON` — only Core/Network/Sql/WebSockets, no app/keyring/tests —, systemd unit `argonaut-chat-server` as user `argonaut-chat` on 127.0.0.1:8790 sandboxed, Caddy site `/etc/caddy/argonaut-chat.caddy` imported in the Caddyfile, ends with a WebSocket handshake check). docs/deploy.md has the rsync command. The owner runs it; tested in a clean ubuntu:24.04 container (packages, build, handshake), not on the real VPS.
- `chat_app` (`src/app`): `DatabaseKey` (32 random bytes in the system keyring via QtKeychain, service "Argonaut Chat", entry "database-key", no insecure fallback; a new key only when no encrypted history exists; `main` refuses to start without it; not unit-tested: tests must not touch the real keyring), `Dialogs` (`SignUpDialog` at first start, cancel = Note to Self only; `SafetyNumberDialog`, three lines of four groups, Mark as Verified), `MainWindow(store, messenger)` (File › New Chat… Ctrl+N, Sign Up…, Blocked People… (`BlockedDialog`); a request shows a "Request" pill in the list and a bar with Block/Delete/Accept in place of the composer (`acceptRequest`, `deleteRequest(ask)`, `blockRequest(ask)`); Conversation › Safety Number…, the connection under "Chats", notices, a header warning when a safety number changed; `startChatWith(username)` for tests; `tst_mainwindow chatThroughTheServer` signs up through the dialog; `chat_test` takes several libraries; Signal's layout: `ConversationDelegate` — a round letter, name, last message, time, unread pill — and `BubbleDelegate` — mine at the right in the accent colour, ✓ sent, ✓✓ delivered, white ✓✓ read, … sending, ⚠ not sent; Enter sends, Shift+Enter a new line), `StyleSheet`, and `Theme.{h,cpp}` **copied from Argonaut Emails** (the same themes; `applicationIcon()` is this program's: a blue speech bubble with Argonaut's boat). A theme change arrives as `PaletteChange` in `changeEvent`.

## Decisions so far (2026-11-20) and the plan

- **Like Signal, not peer to peer**: a server that only relays (accounts, public keys, a queue of encrypted messages until delivery, encrypted attachments); the history only on the devices (docs/how-signal-works.md).
- **Encryption: libsodium** (`libsodium-dev`, installed; 1.0.18 on Ubuntu 24.04), with the Signal Protocol's design built on it: identity keys (Ed25519/X25519), signed and one-time prekeys, X3DH (PQXDH later), the Double Ratchet (as in Signal's published specifications), safety numbers. Tested on its own before any network code.
- **Server: C++/Qt** (confirmed by the owner 2026-11-20): a separate program `argonaut-chat-server` in this repository, `QWebSocketServer` + JSON messages (plain WebSockets, **not socket.io**: a Qt client speaks WebSocket natively), SQLite through QtSql, sharing `chat_core` (protocol and crypto) with the app. Sign-in by a challenge signed with the device's key, no passwords. TLS by a reverse proxy (Caddy or nginx) in front on the VPS. Tests start the real server in-process on 127.0.0.1 and have two clients exchange encrypted messages through it.
- **Sign-up by username** (decided 2026-11-20: no SMS, no phone number; people find each other by username).
- **The local database: SQLCipher** (decided 2026-11-20: the whole file encrypted; `libsqlcipher-dev` 4.5.6), **the key in the system keyring** through QtKeychain (`qtkeychain-qt6-dev` 0.14.2; GNOME Keyring or KWallet), no passphrase at start for now.
- **Order of work** (steps 1-5 done 2026-11-20): 1) crypto in `chat_core` (keys, X3DH, Double Ratchet) with tests; 2) the protocol (message formats); 3) the server; 4) the app talking to it (accounts, contacts, sending); 5) the encrypted local database; then groups, attachments, linked devices.

## Rules

- Nothing a user writes leaves this computer unencrypted: end-to-end encryption comes before any network code that carries messages. Never invent cryptography: the Signal Protocol's design (X3DH/PQXDH, Double Ratchet) from a reviewed library.
- Never ignore TLS errors.
- Everything that comes from another person or the server is data from a stranger.

## Working with the owner

- **Ask before every commit**; push and release only when asked. Commits use the address in this repository's git config (a GitHub noreply address); never put the owner's personal email anywhere.
- **Never add Claude attribution or `Co-Authored-By` lines** to commits or pull requests.
- No remote yet.
- Prefer plain-text questions and direct action; keep the debug build free of warnings and every test passing.
