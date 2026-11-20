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
- **The history on this computer only**: one SQLite file,
  `~/.local/share/Argonaut/argonaut-chat/chat.sqlite`, that only you can read.
- **Themes**: Argonaut's themes, including theme files from
  `~/.local/share/argonaut/themes`.

## Not there yet

Accounts and the server, end-to-end encryption (the Signal Protocol), the
encrypted database, contacts and safety numbers, groups, attachments,
linked devices, calls.

## Building

Dependencies (Ubuntu 24.04): `cmake ninja-build qt6-base-dev libqt6sql6-sqlite`.

```bash
cmake --preset debug
cmake --build build/debug
./build/debug/argonaut-chat
ctest --preset debug
```

## License

GPL-3.0-or-later.
