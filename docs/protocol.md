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

### Attachments

A text may carry up to 10 attachments (its body is then their caption,
maybe empty), each a pointer:

```json
"attachments": [{"id":"<32 hex>", "key":"…", "digest":"…", "size":12345,
                 "name":"photo.jpg", "type":"image/jpeg"}]
```

The file is encrypted on the sender's computer (`src/core/Attachment.h`): a
new random key (32 bytes), the file padded with zeros to Signal's size
bucket (`max(541, 1.05^ceil(log_1.05 size))`, so the size shows little),
then `nonce (24) | XChaCha20-Poly1305(key, nonce, padded)`; `digest` is the
SHA-256 of that blob, which alone is uploaded. The recipient downloads the
blob, checks its size and digest, decrypts, and drops the padding. At most
25 MB a file. Names arrive without folders, control characters or
direction marks; types must look like `type/subtype`. On disk, both sides
keep the blob as it was (the key in the encrypted database).

### Replies

A text that answers another quotes it, as Signal does:

```json
{"v":1, "t":"text", …, "body":"Yes!", "quote":{"id":"<its uuid>", "author":"alice", "text":"Pizza tonight?"}}
```

`id` is the answered message's id, `author` the username of whoever wrote
it (the replier's own too), `text` the start of it, at most 200 characters
(a file without a caption: "📎 " and its name), so the quote shows even
where the original is gone. A quote that is not exactly that object drops
the whole message. Versions before 0.2 ignore the field and show the text
alone.

### Reactions

One emoji per person per message, as Signal's; a new one replaces the
last, and `remove` takes it back:

```json
{"v":1, "t":"reaction", "id":"<uuid>", "sent":…, "target":{"id":"<its uuid>", "author":"alice"},
 "emoji":"👍", "group":"<32 hex>"?, "sentTo":"<username>"?}
{"v":1, "t":"reaction", "id":"<uuid>", "sent":…, "target":{…}, "remove":true}
```

`target` names the message by its id and author's username; a reaction is
kept only on a message of the conversation it comes in (the sender's direct
one, or a group they are in) whose author matches. `emoji` is 1 to 32 bytes
of UTF-8 with no letters, digits, spaces, controls or direction marks.
`sentTo` only to my own other devices (a group's has `group`). A reaction
never starts a conversation. Versions before 0.2 drop it unread.

### Deleting for everyone

The author of a message may delete it for everyone, up to 24 hours after
sending it, as Signal allows:

```json
{"v":1, "t":"delete", "id":"<uuid>", "sent":…, "target":{"id":"<its uuid>", "author":"<the sender>"},
 "group":"<32 hex>"?, "sentTo":"<username>"?}
```

Kept only from the message's own author (`target.author` and the sender
must be the same, and the message's), in the conversation it comes in, and
no more than 48 hours after the message by the server's clock. What it
said, its files and reactions are deleted, and the quotes of it in
replies; a mark stays ("This message was deleted"). Versions before 0.2
drop it unread, and keep the message.

### Profiles

```json
{"v":1, "t":"profile", "id":"<uuid>", "sent":…, "name":"Anna K.", "avatar":{…pointer…}}
```

A display name (at most 64 characters, no control characters or direction
marks; cleaned again on arrival) and maybe a picture, sent as an attachment
pointer (PNG or JPEG, at most 256 KB, `image/` only). Profiles travel only
in encrypted messages: to the people one writes to or accepts, and again to
all of them when it changes; the server never stores one. A profile is not
a message: nothing is shown or acknowledged. A request's sender's name is
kept (shown with the username), their picture fetched only once accepted.
The picture is uploaded once and its pointer reused for 25 days (the server
keeps it 30).

### Groups

Kept by their members; the server knows nothing of them (it sees only
one-to-one messages). A group is a random id (32 hex digits), a name, an
admin and a member list (at most 50, the admin among them):

```json
{"v":1, "t":"group", "id":"<uuid>", "sent":…, "group":"<32 hex>", "name":"Trip",
 "admin":"alice", "members":["alice","bob","carol"], "revision":3}
{"v":1, "t":"leave", "id":"<uuid>", "sent":…, "group":"<32 hex>"}
{"v":1, "t":"text", …, "group":"<32 hex>"}
```

- The admin sends `group` to every member (and to those taken out) at each
  change, the revision one higher. A new group is taken only from its admin
  and only if I am in it; later updates only from the admin, only newer. A
  group made by someone I have not accepted is a request.
- A group text goes to each member, encrypted for each with the session
  already there; it is taken only from a member of a group I am in. If some
  members could not be reached, it goes again to all (the others drop the
  copy by its id).
- `leave` goes to the others; the admin then sends the new list.
- Receipts go to the author: a group message is ✓✓ once a member has it,
  read once a member has read it.

### Disappearing messages

```json
{"v":1, "t":"timer", "id":"<uuid>", "sent":…, "expire":3600, "group":"<32 hex>"?}
{"v":1, "t":"text", …, "expire":3600}
```

Either side of a chat, or any member of a group, sets a conversation's
timer (seconds, at most four weeks; 0 is off); every side shows a note. A
text carries its timer: the sender's copy counts from sending, the
recipient's from when it is seen (read on another device of mine counts
too); then it is deleted on each side with its attachments. A timer from
someone with no conversation starts none.

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

## The server's requests

`argonaut-chat-server` (`src/server`), reached through `ChatClient`
(`src/core`). One WebSocket per device; text frames of JSON only (a binary
frame, or anything that is not a JSON object with a whole-number `rid` and a
string `op`, closes the connection). Requests are at most 4 MiB.

Every request carries `rid`, which its answer repeats, with `"ok": true` and
its fields, or `"ok": false` with `error` (a code) and `message` (for
people). The server sends one thing unasked: `{"op":"deliver","envelope":{…}}`.

| op | fields | answer | |
|---|---|---|---|
| `challenge` | | `nonce` (32 bytes) | for one sign-in or sign-up try |
| `register` | `username`, `identityKey`, `signedPreKey`, `oneTimePreKeys` (≤ 100), `signature` | `device` (1) | `taken`, `bad-username`, `denied` |
| `login` | `username`, `device`, `signature` | `prekeys` (one-time prekeys left) | `denied` |
| `keys` | `username` | `devices`: `[{device, bundle}]` | `not-found`; takes one one-time prekey per device |
| `send` | `to`, `messages`: `[{device, message}]` | | `devices` (with the right list), `not-found`, `full` |
| `ack` | `ids` (≤ 1000) | | only the device's own messages |
| `prekeys` | `signedPreKey`?, `oneTimePreKeys` | `prekeys` | `denied` (a signature that is not the device's) |

| `upload` | `size` (the blob's) | `id`, `chunk` (largest chunk) | `full` (the server's 5 GB are used) |
| `upload-chunk` | `id`, `offset`, `data` (≤ 512 KiB) | `received` | `bad-offset` (with `received`), `not-found` (not yours) |
| `download` | `id`, `offset`, `length` (≤ 512 KiB) | `data`, `size` | `not-found` (unknown, unfinished or expired) |

Any request may also be answered `rate-limited`, with `retryAfter` (ms).

| `provision-open` | (not signed in) | `id` (a slot) | the connection may wait 10 minutes unsigned in |
| `provision-send` | `id`, `message` (≤ 768 KiB) | | `not-found` (no device waiting there; one message per slot) |
| `add-device` | as `register`, with the account's `identityKey`; signature for device 0 | `device` | `denied`, `too-many-devices` (5) |
| `devices` | | `devices`: `[{device, created}]` | |
| `remove-device` | `device` | | `denied` (the first device removes others; any removes itself; the first stays) |

### Linked devices

One account, one identity key, up to 5 computers. The new computer opens a
slot and shows a **code**: base64url of its one-time X25519 public key (32
bytes) and the slot (16 bytes), 64 characters. On a computer of the account
the user types that code; it sends the account, sealed to that key with
libsodium's sealed box (the server cannot read or forge it), into the slot:
`{"v":1, "username", "identity" (the secret key), "name", "avatar"?,
"contacts": [accepted people], "groups": [{id, name, admin, members,
revision}], "blocked": [...]}`. The new computer joins with `add-device`,
starts with the same trust but no history.

Messages to someone go to all their devices (the server insists). What I
send goes also to my other devices, as a text with `sentTo` (or its
`group`); reading sends them a read receipt (marks those messages seen);
group updates, leaving and my profile go to them too. A device of mine
must have my identity key.

Attachments are blobs the server cannot read, kept in `attachments/` next
to its database for 30 days (unfinished uploads: a day), and handed to
anyone signed in who knows the random id: the id is only ever in the
encrypted message. Chunks go in order, each after the last was answered;
only the uploader may append, never past the announced size. Per account:
100 uploads an hour, 1 GB up and 5 GB down a day.

- **Signing in** (`register`, `login`): the `signature` is the identity key's
  Ed25519 signature of `"ArgonautChat sign-in v1" 0x00 · nonce · device (4,
  big endian) · username`. No passwords. A nonce serves one try; after 5
  failed tries the connection is closed, and one that has not signed in
  within 30 seconds too. An unknown account and a wrong key get the same
  answer. `register` also checks the signed prekey's signature. Signing in
  again as the same device closes the older connection.
- **`send`**: one CipherMessage for each of the recipient's devices, no more,
  no fewer (not for the sending device itself); otherwise `devices` lists
  them, so the sender can fetch the missing keys. Each must decode as a
  CipherMessage; the server cannot read it.
- **Delivery**: queued messages are pushed after signing in, new ones at
  once, at most 500 unacknowledged at a time. A message is deleted only
  when its device acknowledges it; until then it comes again on every
  sign-in. At most 10000 are queued per device.
- **Keep-alive**: WebSocket pings both ways. The app pings every 30 s and
  closes a connection silent for 10 s past that, then connects again; the
  server pings every minute and drops a connection not heard from in two.
  Routers drop quiet connections without a word: this notices.
- **One-time prekeys**: at most 200 kept per device (the oldest dropped).
  When none are left, bundles come without one.
- **TLS**: the server speaks plain WebSockets and listens on 127.0.0.1 by
  default; a reverse proxy (Caddy, nginx) in front does TLS. `ChatClient`
  accepts `wss://` anywhere, `ws://` only to this computer, and never ignores
  a TLS error.

- **Rate limits** (token buckets: a burst, then a steady rate):

  | what | per | burst | then |
  |---|---|---|---|
  | open connections | address | 50 at once | |
  | requests | connection | 200 | 20 a second |
  | sign-ups | address | 5 | 5 an hour |
  | challenges (sign-in tries) | address | 30 | 30 a minute |
  | sends | account | 120 | 2 a second |
  | key fetches | account | 100 | 100 an hour |
  | prekey uploads | account | 10 | 10 an hour |
  | one-time prekeys handed out | device | 50 | 50 an hour |

  Over the last one, bundles come without a one-time prekey (X3DH works
  without), so nobody can use up someone's stock. The app keeps rate-limited
  messages in its outbox and sends them after `retryAfter`.
- **Addresses behind a proxy**: with `--trust-proxy`, a connection from
  loopback (the proxy) counts as the last address in its `X-Forwarded-For`;
  without it, or from anywhere else, the header is ignored.

Not yet: linked devices (device numbers above 1), deleting an account.
