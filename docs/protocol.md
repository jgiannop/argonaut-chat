# Argonaut Chat's protocol

How messages travel, layer by layer. The code is `src/core/Protocol.{h,cpp}`
and `src/core/Session.{h,cpp}`; the cryptography under it is in
`Crypto`, `X3dh` and `RatchetSession` (Signal's design: see
[how-signal-works.md](how-signal-works.md)).

Everything a device receives, from the server or from another person, is
data from a stranger: every decoder checks sizes, types and ranges and
refuses anything else.

## Accounts

People sign up with a **username** (no phone number). A username is 3 to 32
characters: `a-z`, `0-9`, `_` and `.`, starting with a letter, no `..`, no
`.` at the end. ASCII only, so no look-alike letters from other alphabets.
Case does not matter: usernames are kept and compared in lower case.

A person may have several devices (linked devices, later); each is
`username` + a device number (1 for the first).

## Layer 1: Content (only ever seen encrypted)

What a person wrote, as compact JSON:

```json
{"v":1, "t":"text", "id":"<uuid>", "sent":1763600000000, "body":"Hello"}
{"v":1, "t":"receipt", "id":"<uuid>", "sent":1763600000000, "kind":"read", "for":["<uuid>", "..."]}
```

- `id`: a lower-case UUID without braces, made by the sender; receipts name
  messages by it.
- `sent`: milliseconds since 1970, UTC, by the sender's clock.
- `kind`: `delivered` or `read`; at most 1000 ids in `for`.
- A text's body is at most 64 KiB of UTF-8.

Then **padded** as Signal pads: a byte `0x80`, then zeros up to a multiple of
160 bytes, so the length of a message shows little.

## Layer 2: CipherMessage (binary)

The padded Content encrypted by the sender's `Session` for one device:

| kind | bytes |
|---|---|
| `1` message | `0x01` · ratchet message |
| `2` prekey message | `0x02` · identity key (32) · base key (32) · signed prekey id (4, big endian) · has one-time prekey (1: 0 or 1) · one-time prekey id (4; 0 when none) · ratchet message |

The **ratchet message** (`RatchetSession`): version `0x01` · ratchet public
key (32) · previous chain's length (4) · number in this chain (4) · the
XChaCha20-Poly1305 ciphertext with its 16-byte tag. Its associated data is
both identity keys (Alice's, then Bob's) and the header.

Alice sends **prekey messages** until Bob first answers: each carries the X3DH
header, so whichever arrives first starts the session for Bob. Bob deletes
the one-time prekey it used, so the same start cannot be replayed. If the
sender's identity key is not the one Bob knew for that username, the app must
say so (a new safety number) before showing the message as trusted.

## Layer 3: the server's JSON

Binary fields are standard base64.

A **prekey bundle** (what the server gives for a device):

```json
{"identityKey":"…",
 "signedPreKey":{"id":1, "key":"…", "signature":"…"},
 "oneTimePreKey":{"id":10, "key":"…"}}
```

`oneTimePreKey` is missing when the server has run out; X3DH then goes
without it. The client checks the signature before use.

An **envelope** (a message as the server hands it over):

```json
{"id":"42", "from":"alice", "device":1, "time":1763600000000, "message":"…"}
```

`id` is the server's (1 to 64 printable ASCII characters), acknowledged once
the message is safely stored; `time` is when the server got it; `message` is
an encoded CipherMessage (at most 192 KiB).

## The server's requests (step 3, to be fixed then)

Over one WebSocket, JSON objects with an `op`, and a request number `rid`
that the answer repeats. A first sketch:

- `register` `{username, device, identityKey, signedPreKey, oneTimePreKeys[]}`
- `challenge` → `{nonce}`; `login` `{username, device, signature}` (the nonce
  signed by the identity key: no passwords)
- `keys` `{username}` → a bundle for each of their devices (and one
  one-time prekey of each taken)
- `send` `{to, messages: [{device, message}]}`: one CipherMessage per device
- `deliver` (from the server) an envelope; `ack` `{ids}`
- `prekeys` `{signedPreKey?, oneTimePreKeys[]}` to top up; the server says
  when few are left.
