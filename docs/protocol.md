# Argonaut Chat's protocol

How messages travel, layer by layer. The code is `src/core/Protocol.{h,cpp}`
and `src/core/Session.{h,cpp}`; the cryptography under it is in
`Crypto`, `MlKem`, `X3dh` (PQXDH) and `RatchetSession` (Signal's design:
see [how-signal-works.md](how-signal-works.md)).

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

### Voice messages

A voice message is an attachment like any other (above), recorded mono at
16 kHz, at most 5 minutes. Since 0.3.2 it is **Ogg Opus** (RFC 7845; 16
kbit/s, about 120 KB a minute), marked `"voiceNote": true` with `"type":
"audio/ogg"`. Before that it was a WAV of 16-bit PCM marked `"voice": true`
with `"type": "audio/wav"`; such messages still play. Each mark is valid
only with its own type, and never both: anything else drops the message.
(0.2–0.3.1 drop a `"voice"` mark on another type, so Opus uses a new key,
which they ignore: to them it is an `.ogg` file.)

The recipient plays it only once it reads cleanly, decrypted in memory
only: a WAV must be a plain, consistent one of 16-bit PCM, 1 or 2
channels, 8 to 48 kHz (`Voice::fromWav`); an Ogg must hold one Opus
stream of 1 or 2 channels that decodes without a hole or an error, by
opusfile (`Voice::fromOpus`). Either at most 5 minutes and a little.

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

### Mentions

A group's text (or an edit of one) may @mention members, as Signal's body
ranges do:

```json
{"v":1, "t":"text", …, "body":"Hi @Anna", "group":"<32 hex>",
 "mentions":[{"start":3, "length":5, "user":"anna"}]}
```

`start` and `length` count UTF-16 code units of `body` (as QString and
JavaScript do); the text there is what the sender's app showed, so
older versions (0.2.0 and before) ignore the field and show the text as it is. At most 50, in order, not
overlapping, inside `body`, never splitting a character, each object
exactly these three fields with `user` a username; anything else drops
the whole message. On arrival, mentions of people who are not members of
that group, and all mentions in direct messages, are dropped. A mention of
me notifies even in a muted conversation, and marks it "@" in the list
until read. An edit's `mentions` replace the old ones (none: none left).

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

### Typing

```json
{"v":1, "t":"typing", "id":"<uuid>", "sent":…, "action":"started"|"stopped", "group":"<32 hex>"?}
```

"started" when someone begins writing, again every 10 s while they go on,
"stopped" when they clear the field or stop for 5 s; a message from them
ends it too, and shown "typing…" goes after 15 s without news. Sent with
`"online": true` (above), so it is never queued; one more than 30 s old
by the server's clock is ignored (a server before 0.2 queues it). Not to
or from requests, and not at all when View › Typing Indicators is off
(neither sent nor shown, as Signal's). Versions before 0.2 drop it unread.

### Editing

The author of a text may change it, up to 24 hours after sending it, as
Signal allows; its files stay:

```json
{"v":1, "t":"edit", "id":"<uuid>", "sent":…, "target":{"id":"<its uuid>", "author":"<the sender>"},
 "body":"Meet at 6", "group":"<32 hex>"?, "sentTo":"<username>"?}
```

The same checks as a delete (below): only the author's own message, in
its conversation, within 48 hours by the server's clock; an edit older
than the one already applied (by `sent`) is ignored. It shows as edited.
Replies keep the words they quoted. Versions before 0.2 drop it unread.

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
| `3` PQXDH prekey message | `0x03` · identity key (32) · base key (32) · signed prekey id (4, big endian) · has one-time prekey (1: 0 or 1) · one-time prekey id (4; 0 when none) · KEM prekey (1: `1` the one-time prekey's, only with one; `2` the signed prekey's last-resort one) · ML-KEM-1024 ciphertext (1568) · ratchet message |
| `2` X3DH prekey message | the same up to the one-time prekey id, then the ratchet message: from versions before 0.3; read only for a session that already exists, never to start one |

The **ratchet message** (`RatchetSession`): version `0x01` · ratchet public
key (32) · previous chain's length (4) · number in this chain (4) · the
XChaCha20-Poly1305 ciphertext with its 16-byte tag. Its associated data is
both identity keys (Alice's, then Bob's) and the header.

Alice sends **prekey messages** until Bob first answers: each carries the
PQXDH header, so whichever arrives first starts the session for Bob. Bob
deletes the one-time prekey it used (and its KEM prekey with it), so the
same start cannot be replayed. If the sender's identity key is not the one
Bob knew for that username, the app must say so (a new safety number)
before showing the message as trusted.

### PQXDH

As Signal's PQXDH (signal.org/docs/specifications/pqxdh), with ML-KEM-1024
(FIPS 203, from mlkem-native: `third_party/mlkem-native`):

```
DH1 = DH(IK_A, SPK_B)   DH2 = DH(EK_A, IK_B)   DH3 = DH(EK_A, SPK_B)   DH4 = DH(EK_A, OPK_B)?
(CT, SS) = ML-KEM-1024.Encaps(PQPK_B)
SK = HKDF-SHA-256(salt = 32 zero bytes, F · DH1 · DH2 · DH3 [· DH4] · SS,
                  info = "ArgonautChat_CURVE25519_SHA-256_ML-KEM-1024"),  F = 32 × 0xFF
AD = IK_A · IK_B
```

`PQPK_B` is the one-time prekey's KEM prekey when the bundle has one, else
the last-resort KEM prekey that goes with the signed prekey (same id,
replaced with it). Every KEM prekey is signed by the identity key, over
`0x08` · its 1568 bytes (Signal's EncodeKEM); Alice checks the signature
and FIPS 203's input check, and Bob, with his, decapsulates (a changed
ciphertext gives another secret: nothing decrypts).

**PQXDH is required.** A bundle without a KEM prekey (a device on a version
before 0.3, or a server leaving it out) starts nothing: the app says the
other person needs to update. A kind `2` start is refused the same way, so
neither a server nor an old version can make a conversation start without
the post-quantum part. Sessions begun before keep working.

## Layer 3: the server's JSON

Binary fields are standard base64.

A **prekey bundle** (what the server gives for a device):

```json
{"identityKey":"…",
 "signedPreKey":{"id":1, "key":"…", "signature":"…", "kemKey":"…", "kemSignature":"…"},
 "oneTimePreKey":{"id":10, "key":"…", "kemKey":"…", "kemSignature":"…"}}
```

`oneTimePreKey` is missing when the server has run out; PQXDH then goes
without it, with the signed prekey's last-resort KEM key. `kemKey` (1568
bytes) and `kemSignature` (64) come together or not at all (not at all:
a version before 0.3). The client checks every signature before use.
Uploads (`register`, `link`, `prekeys`) carry the same fields; the server
checks each signature, the KEM prekeys' too, and refuses the whole upload
if one fails.

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
| `send` | `to`, `messages`: `[{device, message}]`, `online`? | | `devices` (with the right list), `not-found`, `full`; `online: true` (0.2): only to their devices connected now, never kept for later (typing indicators) |
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
  answer. `register` also checks the prekeys' signatures. Signing in
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

  Over the last one, bundles come without a one-time prekey (PQXDH works
  without, with the last-resort KEM prekey), so nobody can use up someone's
  stock. The app keeps rate-limited
  messages in its outbox and sends them after `retryAfter`.
- **Addresses behind a proxy**: with `--trust-proxy`, a connection from
  loopback (the proxy) counts as the last address in its `X-Forwarded-For`;
  without it, or from anywhere else, the header is ignored.

Not yet: linked devices (device numbers above 1), deleting an account.
