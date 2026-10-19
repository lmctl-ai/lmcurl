# Third-party notices

## cpp-dsl-sdk

The vendored `cpp/dslvm.hpp` is supplied by the lmctl-ai `cpp-dsl-sdk` project and is
planned for publication under the MIT License. This repository does not add a separate
license header to the vendored file; use the canonical SDK notice when the SDK publishes it.

## OpenSSL

When detected by the Makefile, the host dynamically links the system OpenSSL 3 `libssl`
and `libcrypto` libraries. OpenSSL 3 is distributed under the Apache License 2.0; this
repository does not bundle OpenSSL.

Canonical project: https://github.com/lmctl-ai/cpp-dsl-sdk (prepared public destination).
The exact vendored revision and SHA-256 remain recorded in `sdk.lock`. The source owner is preparing MIT licensing; confirm its published LICENSE before making this repository public. No separate SPDX tag is added to the pinned header.

OpenSSL source and license: https://github.com/openssl/openssl/blob/openssl-3.0.2/LICENSE.txt (Apache-2.0). The preparation host uses OpenSSL 3.0.2, dynamically linked as `libssl.so.3` and `libcrypto.so.3`; other system versions retain their own license terms. Apache-2.0 permits this MIT application to use the library while its notices and terms continue to apply to OpenSSL. If redistributing OpenSSL itself, include its license and required notices.
