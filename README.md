# Argonaut Chat

A private messenger for the desktop, like Signal, written in C++17 with Qt 6
Widgets: the same technology and the same look as
[Argonaut](https://github.com/jgiannop/argonaut), the file manager, and
[Argonaut Emails](https://github.com/jgiannop/argonaut-emails).

Version 0.3.1, for Ubuntu and Windows. How Signal works, and what that means for this
program: [docs/how-signal-works.md](docs/how-signal-works.md).

## What it does now

- **Signal's layout**: the conversations at the left (newest first, the last
  message, the unread count), the chosen one at the right, messages as
  bubbles (yours at the right), a field to write in (Enter sends, Shift+Enter
  is a new line).
- **Note to Self**, always there.
- **Chatting with people, encrypted end to end** with the Signal Protocol's
  design (PQXDH — X3DH with post-quantum ML-KEM-1024 — and the Double
  Ratchet, on libsodium and mlkem-native): sign up with a
  username (no phone number), start a chat by someone's username, ✓ sent,
  ✓✓ delivered, white ✓✓ read. Messages wait on the server while the other
  person is offline, and in an outbox while you are.
- **Message requests**: a stranger's first message waits as a request;
  until you accept, they get no receipts at all. Delete it, or block them
  (their messages are then dropped, and they are not told).
- **Files and pictures** (📎, up to 25 MB): encrypted on your computer
  with a new key each, as Signal does; the server keeps only the encrypted
  bytes, for 30 days. Pictures show in the conversation; a click saves a
  file. Kept encrypted on disk too.
- **Replies, reactions, editing and deleting**, as in Signal (right-click a
  message): reply with a quote, react with an emoji, edit what you sent
  within 24 hours, delete it for yourself or for everyone.
- **Voice messages** (🎤): recorded here, sent encrypted like a file
  (Opus, about 120 KB a minute), up to 5 minutes; a click plays one.
- **Typing indicators** (View › Typing Indicators to turn them off) and
  **find in a conversation** (Ctrl+F).
- **Pictures and files by pasting or dropping** them on a chat (Ctrl+V a
  screenshot); they wait above the field until you send. **Copy part of a
  message** (drag over its text, double-click a word, triple-click all of
  it) or whole ones (Ctrl/Shift+click several, Ctrl+C), and a click on a
  reply's quote goes to the message it answers.
- **Mute, archive and pin chats** (right-click a chat, or the Conversation
  menu): mute for an hour up to always, archive out of the list, pin up to
  four at the top.
- **Where you left off**: a "New messages" line at the first unread, and a
  ↓ button back to the newest.
- **Read receipts** can be turned off (View › Read Receipts); as in Signal,
  you then don't see others' either.
- **@mentions in groups**: type @ and pick a member; a mention of you
  notifies even in a muted group.
- **App lock** (File › App Lock…): your chats hidden behind a passphrase at
  start, with Ctrl+L, and after some idle minutes; notifications then show
  no names or text. It guards against someone at your computer while you
  are away. Forgotten: `argonaut-chat --forget-app-lock`.
- **Groups** of up to 50 (File › New Group…): kept by their members, each
  message encrypted for each member; the server cannot tell a group exists.
  Its creator adds and removes people; anyone can leave.
- **Disappearing messages** (Conversation › Disappearing Messages): from
  30 seconds to 4 weeks after they are seen, gone on every side.
- **Linked devices**: your account on up to five computers (File › Linked
  Devices…), linked by a code; what you send and read follows you. A new
  computer can get the last year's messages and files from the one that
  links it, encrypted end to end (the server only passes the pieces on).
- **Your name and picture** (File › Profile…), sent only to the people you
  write to or accept, encrypted; the server never sees them. Others always
  see your username too.
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

Calls, Sender Keys for big groups, Signal's post-quantum ratchet (SPQR).

## Installing

From [Releases](https://github.com/jgiannop/argonaut-chat/releases):

- **Ubuntu 24.04 or 26.04:** the `.deb` for your release. It brings in what
  it needs (Qt 6, libsodium, SQLCipher, QtKeychain) and puts Argonaut Chat
  in the applications menu:

  ```bash
  sudo apt install ./argonaut-chat_<version>_ubuntu24.04_amd64.deb
  ```

- **Windows 10 or 11 (64-bit):** `argonaut-chat-<version>-windows-setup.exe`
  installs it with a Start menu entry (for you alone, or for everyone with
  administrator rights); `argonaut-chat-<version>-windows-x64.zip` is the
  same program as a folder to run from anywhere. The history is kept in
  `%APPDATA%\Argonaut\argonaut-chat`, encrypted, its key in Windows
  Credential Manager. The program isn't signed yet, so SmartScreen warns
  the first time: "More info", then "Run anyway".

`SHA256SUMS` beside them has the checksums. To make the Linux packages on
your own computer: `packaging/build-debs.sh` (in Docker, into `dist/`).

## Next

Done in 0.3.0 (2026-11-22): **PQXDH**, the post-quantum start of a
conversation that Signal uses (X3DH plus ML-KEM-1024 from mlkem-native,
`third_party/mlkem-native`), so that recorded traffic stays safe from a
future quantum computer; and the **app lock**.

**Updating to 0.3.0**: the server first (done on the VPS), then every app.
New conversations need both sides on 0.3 or later (an older app is told to
update); conversations already going on keep working.

Then:

1. **Use 0.3.x for real** with one or two people for a few days, and fix
   what that turns up.
2. Later: voice and video calls, Signal's post-quantum ratchet (SPQR).

## Making a release

Set the version in `CMakeLists.txt` (`project(argonaut-chat VERSION 0.3.1 …)`),
commit, then tag and push:

```bash
git tag v0.3.1
git push origin main v0.3.1
```

The Release workflow builds the two `.deb` packages and the Windows
installer and zip, checks that each installs and starts, and publishes them
as the release `v0.3.1`. Every push to `main` builds and tests on Linux
(also under ASan/UBSan) and on Windows.

## Building

Dependencies (Ubuntu 24.04): `cmake ninja-build pkg-config qt6-base-dev
qt6-websockets-dev qt6-multimedia-dev libsodium-dev libsqlcipher-dev
qtkeychain-qt6-dev libopus-dev libogg-dev libopusfile-dev libopusenc-dev`.

```bash
cmake --preset debug
cmake --build build/debug
./build/debug/argonaut-chat
ctest --preset debug
```

On Windows: Visual Studio 2022, Qt 6.8 with Qt WebSockets and Qt Multimedia, libsodium and
SQLCipher from vcpkg, QtKeychain built against that Qt, then `cmake --preset
windows` with vcpkg's toolchain file; `.github/workflows/windows.yml` has
every step.

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

GPL-3.0-or-later. `third_party/mlkem-native` (ML-KEM) is mlkem-native's,
under Apache-2.0 OR ISC OR MIT (its `LICENSE`).
