## Simple, failure-aware design

- Start with the smallest direct change that meets the stated need. If direct code works, use it over a new abstraction, flag, dependency, or layer.
- Before changing code, trace the current path, its callers, its data, its tests, and its recovery behavior. Preserve working checks and contracts unless the change needs a clear update.
- Treat failures as the result of interacting conditions. Reproduce the path, list the contributing conditions, and fix the smallest necessary link instead of naming one root cause or blaming one line.
- Add a defense only for a named failure. Put it at the clearest boundary, keep it distinct from existing checks, and state how tests will show that it works.
- Keep partial failure visible and recovery local. Avoid hidden state, broad fallbacks, and new coupling that make unrelated paths harder to understand.
- For every new piece of complexity, name the failure it prevents, its source of truth, and its test. If you cannot name all three, leave it out.
- After a change, inspect nearby paths for new failure modes, coupling, and single points of failure. Judge the old choice with the evidence available before the failure, not only by its outcome.

## Agent skills

### Issue tracker

Issues and specs live as GitHub issues; use the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

Use the default five triage labels: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repo. See `docs/agents/domain.md`.
