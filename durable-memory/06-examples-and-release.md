# Examples and release

The catalog contains 32 self-executing examples: REST/login/cookies/auth; redirect/retry/poll; JSONPath/Schema/OpenAPI/XML/HTML; data-driven/tagged/parallel/lifecycle suites; reports/forms/uploads/SSE/compression/limits/snapshots; load/timing; and LLM prompts, streams, sampling, matrix, replay, JSON and redaction. Each writes images/<name>.png. scripts/generate-examples.py owns the script generation; examples/render-all.sh reproduces the 32 PNGs. Latency figures vary between runs.

v0.1.0 is published with five executables plus SHA256SUMS. Linux x64/ARM64 execute the full host; macOS x64/ARM64 and Windows x64 are metadata stubs. All five packaging jobs passed. Runtime source is tagged at 7b6274d; build portability fixes are in c4cf59a (use this or current main for source builds). Runtime/schema/dependency/notice files are identical across those commits; the tag was not rewritten.

All 32 raw GitHub autoruns passed from separate empty directories with one initially empty tool cache and lmcurl/lmplot/lmdraw removed from PATH. The installed Linux host hash matches the published SHA256SUMS. See evidence/autorun-v010.json and evidence/release-v010.json.

Live provider evidence is in evidence/live-llm-v010.json: OpenRouter free route 47/3 input/output tokens ($0), DeepSeek Flash 8/1 ($0.00000360), Ollama local 29/3 ($0 marginal), Ollama cloud 71/16 ($0 marginal). The cloud call exhausted its tiny output allowance with no visible answer; API/usage plumbing passed. Infrastructure/subscription charges are excluded. No keys are recorded. Do not repeat successful probes casually.
