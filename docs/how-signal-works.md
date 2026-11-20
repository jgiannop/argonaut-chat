# How Signal works

What Argonaut Chat is modelled on. Signal's code is open (the apps, the
server and the cryptography library are all on GitHub, under AGPL-3.0), so
all of this can be checked there.

## Is it peer to peer?

**No.** Every message goes through Signal's servers. Phones are often
offline, behind NAT, or asleep, so two of them can rarely reach each other
directly; a server that is always there holds a message until the other side
comes to fetch it.

But the server is a **blind post office**:

- Messages are **end-to-end encrypted** on the sender's device, for each of
  the recipient's devices. The server only ever sees ciphertext.
- It keeps a message **only until it is delivered**, then deletes it (and it
  drops undelivered ones after a while).
- With **sealed sender** it doesn't even know who sent a message: the
  sender's identity is inside the encryption; the server only checks a
  delivery token.

The exception is **calls**: voice and video use WebRTC and go peer to peer
when the two networks allow it, else through a relay (TURN) server. Signal
also offers "Always relay calls" so the other side never learns your IP
address.

## Where are messages stored? Is there a database?

**On each device, in a local encrypted database. Not on the server.**

| Where | How |
|---|---|
| Android | SQLite encrypted with **SQLCipher**; the key is protected by the Android Keystore. |
| iOS | SQLite with SQLCipher (through GRDB); the key in the iOS Keychain. |
| Desktop (Electron) | SQLite with SQLCipher in `~/.config/Signal/sql/db.sqlite`; its key in `config.json`, itself encrypted with the operating system's keyring (Electron's safeStorage). Attachments are files next to it, encrypted too. |

So the history lives with you. A new phone starts empty unless you transfer
or restore a backup. A linked desktop gets new messages from the moment it
is linked (newer versions can carry recent history over when linking).

## What the server keeps

As little as it can:

- your **account**: the phone number (and optionally a username), when you
  registered, when you were last connected (to the day);
- your **public keys** (identity key, signed prekey, a stock of one-time
  prekeys, post-quantum prekeys), so others can start a conversation while
  you are offline;
- your **profile** (name, photo, about) **encrypted**: only people you talk
  to have the key;
- **groups** (Groups v2) **encrypted**: the server keeps the member list in a
  form it can't read, and checks permissions with zero-knowledge proofs
  (zkgroup) without learning who is in the group;
- the **message queue**, encrypted, until delivery;
- **attachments**: encrypted with a random key by the sender, uploaded to a
  CDN; the key and the file's hash travel inside the message;
- with a **PIN**: some account data (profile key, settings, contacts list
  for restore) in Secure Value Recovery, protected by the PIN in secure
  enclaves (SVR), guessing limited.

**Contact discovery** (which of your phone contacts use Signal) runs inside
a secure enclave (Intel SGX), so the server can't see your address book.

## The encryption: the Signal Protocol

The library is **libsignal** (Rust, with Java, Swift and TypeScript
bindings; no official C or C++ binding).

1. **Keys**: every device has a long-term **identity key** (Curve25519), a
   **signed prekey** (changed now and then) and many **one-time prekeys**,
   published on the server.
2. **Starting a conversation: X3DH, now PQXDH** (2023). The sender fetches the
   recipient's keys and combines several Diffie-Hellman results (plus a
   post-quantum ML-KEM/Kyber key) into a shared secret, without the
   recipient being online.
3. **Every message: the Double Ratchet.** Each message is encrypted with a
   new key (AES-256 + HMAC-SHA256), derived by a symmetric ratchet, and
   every reply mixes in a new Diffie-Hellman exchange. A stolen key reveals
   neither the past (**forward secrecy**) nor, after a reply, the future
   (**post-compromise security**). Since 2025 a post-quantum ratchet (SPQR)
   runs beside it.
4. **Safety numbers**: a fingerprint of both identity keys, to compare in
   person (or by QR code) and be sure no one is in the middle. A changed key
   is shown in the conversation.
5. **Several devices**: each linked device (desktop, tablet) has its own
   keys; the sender encrypts every message once per device of the
   recipient, and once per own other device (so they show what you sent).

## What this means for Argonaut Chat

To be "like Signal" it needs:

- a **local encrypted database** on each device (the history is ours);
- the **Signal Protocol**, or the same design: X3DH/PQXDH + Double Ratchet;
- a **server** that only relays: accounts, public keys, a queue of
  encrypted messages, encrypted attachments. It can be small, and in C++/Qt
  too (a QWebSocket/HTTPS service).

A server can't be avoided for messages to someone who is offline. A
peer-to-peer design (no server) only works while both are online and can
reach each other (on the same network, or through NAT traversal), and then
it gives each side the other's IP address.
