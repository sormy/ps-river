# Agent instructions

- `TODO.md` and `WORKLOG.md` are local (not committed).
- When asked to do something, add it to `TODO.md` first. Remove it from `TODO.md` once done.
- Keep `WORKLOG.md` as a short, dated worklog, appended at the end. Never rewrite past entries;
  if one turns out wrong, add an `Addendum (<date>)` under it instead.
- Implementations are generic: platform-agnostic, game-independent, and fit to upstream
  (shadPS4; AstroQuest and the ARM64 port are patches on it). No game- or machine-specific hacks in patches;
  per-host choices belong in `host/`.
- Committed files contain no PII and no local specifics (host names, addresses, users, local
  paths or file names, hardware); naming the game is fine. Those live only in
  `AGENTS.local.md`, which is not committed.
