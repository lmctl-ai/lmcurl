# LDAPv3 Client & Standard `lmcode` REST/CLI Interfaces

## Summary

`lmcurl` v0.1.4 provides:
1. **Direct CLI & `_lmcode_` LDAPv3 Client (`cpp/ldap_wire.hpp`, `cpp/main.cpp`)**:
   - Direct `curl`-style URL invocations (`lmcurl [-X METHOD] [-u bind_dn:pw] [-d BODY] [-i] ldap://...`) supporting RFC 4516 `ldap://[user:pass@]host[:port][/dn[?attrs[?scope[?filter]]]]]`, `/whoami` (RFC 4532 ExtendedRequest `0x77`), `/compare` (RFC 4511 CompareRequest `0x6e`), `/search`, `/entry`, `/bind`, and `/ping`.
   - Structured CLI subcommands (`lmcurl ldap <ping|bind|whoami|search|get|add|modify|compare|del>`).
   - `_lmcode_` script engine `curl.session("ldap://...")` with full support for `/whoami`, `/compare`, RFC 4516 URLs, attribute projection, and explicit `modify` operations (`{add, replace, delete}`).
2. **Standard `lmcode_std_v3` REST & Stateless CLI Interfaces (`cpp/stdlib/rest.hpp`, `cpp/dslvm.hpp`)**:
   - Supports all 226 `lmcode_std_v3` signatures and standard interfaces (`RestGet`..`RestOptions`, `RestHandler`, `CliCommand`, `CliOption`, `rest_config`, `rest_route`, `rest_serve`, `rest_stop`, `rest_cli`).
   - Verified in `scripts/test-ldap.py` (invoked from `test.sh` and the `ldap-integration` job in `.github/workflows/ci.yml`) against a live `lmldap` server.
