# Build, run, and test

Install the released CLI with `npm install -g @lmctl-ai/lmctl@0.2.347`, then run `bash test.sh`. The script builds the native host, compiles a DSL chunk, runs it against the schema, and checks success and error paths.

For embedding, copy the SDK header or add the Rust crate from the public SDK repository, keep the schema bytes alongside the bytecode, and use the matching SDK major version.
