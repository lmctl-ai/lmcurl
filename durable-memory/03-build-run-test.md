# Build and verification

`BUILD_DIR=build LMCURL_TEST_NETWORK=1 bash test.sh` is the local release gate. It verifies SDK/dependency pins, builds, checks version/schema/signatures/notices and dynamic-glibc linkage, runs five DSL programs (including tool-stream and lmhtml integration), 32 native cases, the concurrent memory regression and the Mock.serve subprocess lifecycle test. The default 4096 MiB limit is also exercised under a 6 GiB virtual-memory ceiling.

The full rendering gate executes all 48 examples from separate empty directories, checks 900x560 PNGs at most 300 KiB, and validates the 16 new zero-cost request ledgers. Without renderers/network opt-in, image execution skips loudly and committed image bytes are checked. No live provider credentials are needed.

CMake uses out-of-source static OpenSSL, a configure-time imported static zlib archive, and the same Ninja generator as `make test`. Linux x64 is tested locally; the five-platform workflow produces two full Linux hosts and three desktop metadata stubs. Local tests are the gate; hosted packaging is not a merge gate.

After publishing, `python3 scripts/test-autorun.py --only-new` removes lmcurl/lmhtml/lmplot/lmdraw from PATH, creates one empty tool cache, executes the 16 public URLs in separate empty directories, records installed versions and image hashes, and writes versioned evidence. Omit --only-new to verify all 48.
