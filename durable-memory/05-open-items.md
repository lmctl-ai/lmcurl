# Open items

Windows and macOS transport remain explicit stubs in the current release. The declarative Hurl-style text format is not a separate compiler phase; typed DSL objects are the supported API. OpenAPI coverage is intentionally a bounded JSON/response subset. Provider-specific live LLM behavior, token accounting and price tables can change, so built-in mock fixtures and cassette replay remain the deterministic path for CI.

The downloader/parser examples are closed local-fixture recipes. Full XML namespace validation, complete robots precedence and a complete accessibility audit are outside their scope. Dynamic parallel fan-out belongs to lmscript; these stories deliberately use fixed call lists so the download helper can be reused later.
