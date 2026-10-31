# Host flag cutover

Linux and the unsupported-platform stub accept only --lmcode, --lmcode-schema
and --lmcode-signatures for the host protocol. Retired names are usage exit 2;
stub execution under the new name stays explicit unsupported exit 3. Exact raw
schema and signature artifacts are unchanged in meaning; SDK sync and metadata
regeneration are performed through the owner scripts. DSLB and .dslb remain.

Both original production entry points were observed failing the refusal controls.
The normal gate includes the real Linux CLI and an actual compiled stub source
with exact embedded resource bytes (no replacement parser). Current SDK 20325f7
and source-compiler v6 capability probes agree. Existing generators regenerated
32 normal and 16 web stories. Gallery PATH comes from explicit/private tools,
not hard-coded shared checkouts.

The full Linux gate passes: exact metadata/notices, compiled stub, linkage,
five language suites, 32 native scenarios, mock HTTP/HTML integration, lifecycle,
concurrent/default memory budgets, all 48 self-executing rendered examples and
secret scans. Runtime latency pictures were generated and checked, then their
unrelated timing-dependent tracked changes were restored. No live credential,
global installation or release operation was used.
