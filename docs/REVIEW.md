# v0.1.0 review

Final independent follow-up: no blocking findings; approved for the reviewed code.

An independent source review identified and resolved these findings before tagging:

- All LLM JSON entry points now enforce the shared 8 MiB/128-level parser limits; deep tool schemas fail cleanly.
- Parallel memory charges reserve with CAS and account for allocator rounding; the concurrent regression holds reservations until every thread has attempted a charge.
- Redirects are followed manually with destination cost guards, credential stripping, five-hop cap and one deadline. LLM redirects cannot leave their provider origin.
- Outer exception messages pass through the redactor.
- Assertions and query filters use bounded POSIX ERE without backreferences, avoiding recursive std::regex backtracking.
- Streamed tool-call deltas assemble by bounded index, retaining split names, IDs and arguments.

Local gate: four DSL programs, 28 native scenarios, concurrent memory regression, default 4096 MiB rejection, exact schema/notices/signatures, linkage allowlist and 32 regenerated images. Artifact scans found no credential values. Live tests are separate; see ../evidence/live-llm-v010.json.

macOS/Windows execute metadata stubs only. HTTP streaming is buffered within the response cap, and OpenAPI validation covers the documented JSON response subset.
