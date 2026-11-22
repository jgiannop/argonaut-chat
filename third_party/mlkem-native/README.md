# mlkem-native (ML-KEM, FIPS 203)

ML-KEM-1024 for PQXDH (`src/core/MlKem.{h,cpp}`), from the PQ Code Package's
mlkem-native, the ML-KEM implementation inside liboqs (its default since
0.13) and AWS-LC, with parts formally verified.

- Release: **v2.0.0**, https://github.com/pq-code-package/mlkem-native/releases/tag/v2.0.0
- Source: `https://github.com/pq-code-package/mlkem-native/archive/refs/tags/v2.0.0.tar.gz`,
  SHA-256 `76bf71771f09a25f30463218974ae10752d72bca34bbc470c7f6f8655f51d622`
- What is here: that archive's `mlkem/` folder and `LICENSE`, **unchanged**
  (compare with the archive before updating). Our only addition is
  `chat_mlkem_config.h` (and this file).
- Licence: Apache-2.0 OR ISC OR MIT (used here under MIT), see `LICENSE`.

Built as one C file (`mlkem/mlkem_native.c`), portable C only (no assembly
backends), with `chat_mlkem_config.h`: ML-KEM-1024, symbols prefixed
`chat_mlkem_`, randomness from libsodium. Checked against NIST's ACVP test
vectors in `tests/tst_crypto.cpp`.

To update: download the new release, check it, replace `mlkem/` and
`LICENSE`, read the release notes for API changes, run the tests.
