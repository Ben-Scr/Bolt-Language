# Third-party notices

## SplitMix64

`stdlib/System/Random.bolt` implements the SplitMix64 state transition described by
Sebastiano Vigna (2015), published in the public domain at:
https://prng.di.unimi.it/splitmix64.c

The original notice is retained in `docs/licenses/SplitMix64.txt`. The Bolt implementation,
bounded distributions, per-instance/shared-state integration and tests are included here;
no external RNG library is needed by the source Random class. This is not a cryptographic API.

## Existing notices

See `docs/ThirdParty.md`, `docs/licenses/` and the notices in the source headers for the retained
Unicode data, editor palette and other third-party materials. The macOS and FreeBSD SDKs bundle a dynamically loaded LLVM distribution. They include the original package notices, component versions and provenance under `bin/llvm/share/`. The source archive contains the fetch scripts and exact checksum locks. Other distributions can load a separately installed LLVM library. JetBrains
SDK binaries are likewise not redistributed.

## fast_float

The bounded decimal reader on Linux (including Android), Apple platforms and FreeBSD uses the upstream fast_float 8.3.3 header under the MIT license. The notice, version and source digest appear in `docs/licenses/FastFloat.txt`.
