# Open items

1. A structured error channel back to lmscript (requirements question 7): exit codes 2/3/4 separate the layers, but the message is
   free text on stderr.
2. If a future version adds collection literals, header lists and route tables could replace repeated `header`/`allow` calls; v4
   keeps collections as host schema objects and has no bytecode list values.
3. A schema shorthand for setter-returns-self (every builder method repeats `"returns": "Request"`).
4. Run the live LLM call once with a key to confirm the OpenAI response shape matches the mock (`choices.0.message.content`,
   `usage.total_tokens`); it costs money, so opt-in only.
5. Keep `sdk.lock` current; rerun `test.sh` with the released lmscript after each compiler release.
