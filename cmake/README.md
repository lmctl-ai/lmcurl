# lmcurl dependency policy

The dependency sync script fetches the upstream archives pinned in `deps.lock`.
CMake compiles curl, zlib, nghttp2 and libxml2 static targets; upstream
OpenSSL Configure/make supplies static TLS archives. JSON and JSON Schema
implementations are header-only. No source archive is tracked in git.

Linux uses static libcurl/OpenSSL/zlib/nghttp2 targets and dynamic system glibc.
No LDAP, SSH, RTMP, IDN, GSS, libpsl or other optional transport is enabled.
