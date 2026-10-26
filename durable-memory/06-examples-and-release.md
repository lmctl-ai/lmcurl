# Examples and release

The catalog has 48 self-executing PNG examples: 32 HTTP/LLM stories plus 16 downloader/parser stories. New stories cover links, sitemap, RSS/Atom, product tables, price/stock polling, CSRF/login/account, pagination, static mirroring, SEO, image/redirect audit, API/HTML consistency, content diff, accessibility smells, JSON-LD/OpenGraph, forms and polite robots crawling.

`scripts/generate-examples.py` regenerates the base stories and invokes `generate-web-examples.py`; the latter uses web_stories_a.py/web_stories_b.py. Generated .lms files embed their helpers and need no repo imports. `examples/render-all.sh` regenerates all images; latency plots vary between runs. The new stories write per-example request/cost ledgers and detailed reports, with a zero-price loopback table and $0.02/$0.25 caps. No new live/provider calls are made.

v0.1.1 adds the 16 recipes, Mock.serve, exact mock cookie checks and an explicit empty-proxy override. Linux x64/ARM64 are full hosts with dynamic glibc >=2.35; macOS x64/ARM64 and Windows x64 remain metadata stubs. Release artifacts include five executables plus SHA256SUMS. Historical v0.1.0 evidence remains in evidence/*v010.json; use current main or v0.1.1 for source builds.

Original live provider evidence is evidence/live-llm-v010.json: OpenRouter 47/3 input/output tokens ($0), DeepSeek Flash 8/1 ($0.00000360), local Ollama 29/3 ($0 marginal), cloud Ollama 71/16 ($0 marginal). Cloud Ollama exhausted its tiny output allowance with no visible answer; API/usage handling passed. Infrastructure/subscription charges are excluded. No keys are recorded; do not casually repeat successful probes.
