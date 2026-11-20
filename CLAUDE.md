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
  - `ChatStore`: SQLite (`QSQLITE`) in `$XDG_DATA_HOME/Argonaut/argonaut-chat/chat.sqlite` (0600, folder 0700): `conversations` (kind `self`/`direct`/`group`, title, peer, last activity) and `messages` (author empty = mine, body, sent_at in ms UTC, status sending/sent/delivered/read/failed or received, seen). Note to Self is made on first use. Signals `conversationsChanged`, `messagesChanged(conversation)`. Bind `text()` (never a null QString: it is bound as NULL). Not encrypted on disk yet.
  - The cryptography, all on libsodium (linked privately through pkg-config): `Crypto` (random, SHA-512, HMAC-SHA256, HKDF per RFC 5869 — 1.0.18 has none —, X25519 `DhKeyPair`/`dh()` refusing low-order keys, `IdentityKeyPair` = Ed25519 converted to X25519 for DH, XChaCha20-Poly1305 `seal`/`open`); `X3dh` (signed and one-time prekeys, `PreKeyBundle`, `initiate`/`respond`, AD = IK_A | IK_B); `RatchetSession` (the Double Ratchet per Signal's spec; wire format version | ratchet key | PN | N | ciphertext; skipped keys up to `MaxSkip`; a message that fails changes nothing; `serialize` holds secrets: local encrypted DB only); `SafetyNumber` (Signal's numeric fingerprint, 60 digits). Tests: `tst_crypto` (RFC 5869 vectors, X3DH, bad bundles, safety numbers), `tst_ratchet`. Not yet: PQXDH, locked memory for keys.
  - The protocol (docs/protocol.md): `Protocol` (usernames, `Content` JSON padded to 160-byte blocks, binary `CipherMessage` — plain or prekey with the X3DH header —, JSON `PreKeyBundle` and `Envelope`; every decoder strict) and `Session` (X3DH + ratchet + CipherMessage: `initiate`, `accept` with prekey lookups, prekey messages until the first answer). Test: `tst_protocol`.
- `chat_app` (`src/app`): `MainWindow` (Signal's layout: `ConversationDelegate` — a round letter, name, last message, time, unread pill — and `BubbleDelegate` — mine at the right in the accent colour, a ✓ when sent; Enter sends, Shift+Enter a new line), `StyleSheet`, and `Theme.{h,cpp}` **copied from Argonaut Emails** (the same themes; `applicationIcon()` is this program's: a blue speech bubble with Argonaut's boat). A theme change arrives as `PaletteChange` in `changeEvent`.

## Decisions so far (2026-11-20) and the plan

- **Like Signal, not peer to peer**: a server that only relays (accounts, public keys, a queue of encrypted messages until delivery, encrypted attachments); the history only on the devices (docs/how-signal-works.md).
- **Encryption: libsodium** (`libsodium-dev`, installed; 1.0.18 on Ubuntu 24.04), with the Signal Protocol's design built on it: identity keys (Ed25519/X25519), signed and one-time prekeys, X3DH (PQXDH later), the Double Ratchet (as in Signal's published specifications), safety numbers. Tested on its own before any network code.
- **Server: C++/Qt** (proposed by Claude, the owner leaned to it and installed `qt6-websockets-dev`; confirm when starting it): a separate program `argonaut-chat-server` in this repository, `QWebSocketServer` + JSON messages (plain WebSockets, **not socket.io**: a Qt client speaks WebSocket natively), SQLite through QtSql, sharing `chat_core` (protocol and crypto) with the app. Sign-in by a challenge signed with the device's key, no passwords. TLS by a reverse proxy (Caddy or nginx) in front on the VPS. Tests start the real server in-process on 127.0.0.1 and have two clients exchange encrypted messages through it.
- **Sign-up by username** (decided 2026-11-20: no SMS, no phone number; people find each other by username).
- **Still to decide**: the local database's encryption (SQLCipher or our own, the key in the keyring).
- **Order of work** (steps 1 and 2 done 2026-11-20): 1) crypto in `chat_core` (keys, X3DH, Double Ratchet) with tests; 2) the protocol (message formats); 3) the server; 4) the app talking to it (accounts, contacts, sending); 5) the encrypted local database; then groups, attachments, linked devices.

## Rules

- Nothing a user writes leaves this computer unencrypted: end-to-end encryption comes before any network code that carries messages. Never invent cryptography: the Signal Protocol's design (X3DH/PQXDH, Double Ratchet) from a reviewed library.
- Never ignore TLS errors.
- Everything that comes from another person or the server is data from a stranger.

## Working with the owner

- **Ask before every commit**; push and release only when asked. Commits use the address in this repository's git config (a GitHub noreply address); never put the owner's personal email anywhere.
- **Never add Claude attribution or `Co-Authored-By` lines** to commits or pull requests.
- No remote yet.
- Prefer plain-text questions and direct action; keep the debug build free of warnings and every test passing.
