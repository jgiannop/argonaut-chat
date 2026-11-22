# libdatachannel (calls)

The connection of a call (`src/call/CallConnection.cpp`): ICE (direct, or
through our relay with TURN), DTLS-SRTP, and the RTP track the Opus packets
travel on, from Paul-Louis Ageneau's libdatachannel, a C++ WebRTC
implementation without Google's.

- Release: **v0.24.7** (commit `800e849a1ede5667caaf8f6caa1eb21e4906dd87`),
  https://github.com/paullouisageneau/libdatachannel/releases/tag/v0.24.7
- With the submodules that release pins, in `deps/`:
  - libjuice `b89c792e3612faf2f12cf35bcc56857313a06be3` (ICE; MPL-2.0)
  - libsrtp `24b3bf8f19b6f5ab4cd2bcceb4f4064efca86fd5` (SRTP; BSD-3-Clause)
  - usrsctp `fec583d54493f879d2ae44a743423bf8a04371ab` (SCTP; BSD-3-Clause)
  - plog `94899e0b926ac1b0f4750bfbd495167b4a6ae9ef` (logging; MIT)
- What is here, **unchanged**: the release's `CMakeLists.txt`, `cmake/`,
  `include/`, `src/`, `LICENSE`, `README.md`, and those four submodules
  without their `.git` and `fuzzer/` folders. Not copied: `examples/`,
  `test/`, `pages/`, and `deps/json` (used only by the examples). Our only
  addition is this file.
- Licence: MPL-2.0 (libdatachannel, libjuice), BSD-3-Clause (libsrtp,
  usrsctp), MIT (plog): see each `LICENSE`. All may be combined with the
  GPL-3.0-or-later program.

Built by our `CMakeLists.txt` (`chat_add_libdatachannel`) as static
libraries, without its WebSocket part, examples and tests, without our
warning flags, and not installed (the package gets only our files). It uses
the system's OpenSSL (`libssl-dev`; vcpkg's `openssl` on Windows) for DTLS.
libjuice speaks TURN over UDP only, so our relay is offered over UDP.

To update: clone the new release with `--recurse-submodules`, read its
release notes, replace the files above (and these commits), build, run
`tst_calls` (and `throughTheRelay` against a coturn).
