# lmcurl dependency policy

The dependency sync script fetches the upstream archives pinned in `deps.lock`.
Only the curl CMake target is compiled here; the other archives supply the
permissive TLS, HTTP/2, JSON-schema and XML implementation inputs used by the
host. No source archive is tracked in git.

Linux uses static libcurl/OpenSSL/zlib/nghttp2 targets and dynamic system glibc.
No LDAP, SSH, RTMP, IDN, GSS, libpsl or other optional transport is enabled.
