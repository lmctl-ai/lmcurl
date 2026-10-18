# Open items

1. A structured error channel back to lmscript (requirements question 7): exit codes 2/3/4 separate the layers, but the message is
   free text on stderr.
2. If v4 adds lists, header lists and route tables replace repeated `header`/`allow` calls; re-evaluate the builder style then.
3. A schema shorthand for setter-returns-self (every builder method repeats `"returns": "Request"`).
4. Run the live LLM call once with a key to confirm the OpenAI response shape matches the mock (`choices.0.message.content`,
   `usage.total_tokens`); it costs money, so opt-in only.
5. Keep `sdk.lock` current; rerun `test.sh` with the released lmscript after each compiler release.
