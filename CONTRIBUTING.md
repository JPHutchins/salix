# Contributing

```sh
nix develop
uv run camas check
uv run camas ci
```

A fix for a crash needs a test that asserts the guarded outcome.

Follow the code already in the file you are editing.

Commits use conventional prefixes — `feat:`, `fix:`, `build:`, `refactor:`,
`style:`, `chore:` — and carry their reasoning in the body: what was measured,
what was tried and rejected, and why.

Work authored by an LLM agent says so in the commit trailer and in anything it
posts:

```
Co-Authored-By: <model-id> <noreply@<provider>>
```
