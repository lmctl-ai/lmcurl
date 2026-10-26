# Build and verification

`bash test.sh` verifies pinned SDK/dependencies, builds the native host, checks exact version/schema/notices/signatures, audits Linux linkage, compiles and runs four DSL test programs (including the tool-stream path), runs 28 native regression scenarios, regenerates 32 examples and images where sibling renderers are available, checks secret-looking artifacts, and exercises the default memory budget under a 6 GiB virtual-memory ceiling.

The local gate is authoritative for this initial release. Linux x86_64 is tested directly. macOS and Windows currently return explicit unsupported stubs. No live credentials are required by the gate.

A clean Linux build (including out-of-source OpenSSL and configure-time static zlib) passed the same full gate. `make test` uses the same Ninja generator as test.sh. `scripts/test-autorun.py` verifies all public raw URLs with one fresh tool cache and separate empty working directories; it requires a published release and records sanitized evidence.
