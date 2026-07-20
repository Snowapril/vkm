---
name: rebase-main
description: Fetch the latest origin/main and rebase the current branch's HEAD onto it, then push to the branch's active PR if one exists. Use this whenever the user asks to sync/rebase against main — e.g. "fetch and rebase latest main branch", "rebase on main", "sync with main", "update this branch from main", "catch up with main" — even if they phrase it loosely. Applies to whatever branch is currently checked out; never run it against main itself.
---

## What this does

Brings the current feature branch up to date with `origin/main` via rebase
(linear history, no merge commits), resolving conflicts conservatively, then
verifies the build/tests before updating the branch's open PR (if any) with a
safe force-push. Local branch and remote ref names may differ (worktree
branches often push to a differently named remote ref), so the remote ref is
always resolved from the branch's upstream, never assumed.

## Steps

1. **Preflight — working tree must be clean.**
   Run `git status --short`. Known tool-state noise (e.g. `.omc/state/*.json`)
   may be discarded with `git checkout -- <path>`. Any other uncommitted
   changes: stop and surface them to the user. Never use bare `git stash` —
   the stash stack is shared across worktrees and other sessions.

2. **Fetch and show what's incoming.**
   `git fetch origin main`, then `git log --oneline HEAD..origin/main` so the
   incoming commits are visible in the conversation. If empty, report the
   branch is already up to date and stop (no push needed).

3. **Rebase.**
   `git rebase origin/main`. On conflicts, resolve commit-by-commit — never
   `git rebase --skip` a commit silently, and never abort without telling the
   user:
   - First understand both sides: `git show <upstream-commit>` for the main
     commits touching the conflicted file, and the branch commit being applied.
   - When main independently implemented overlapping functionality (same API
     or fix landed on both sides), prefer main's version and re-apply only the
     branch's unique additions on top of it.
   - Keep the branch's unique features intact; adapt them to main's helper
     names/signatures rather than keeping duplicates.
   - After resolving, check for *cleanly auto-merged duplicates* too (both
     sides adding a similar function in different places merges without
     conflict): grep for duplicated declarations/definitions of anything both
     sides touched.

4. **Verify before pushing.**
   Build the primary backend and run the unit tests — on macOS Metal:
   `cmake --build build/metal --target UnitTests triangle --parallel` then
   `MTL_DEBUG_LAYER=1 ./build/metal/bin/UnitTests`. All tests must pass and
   validation-layer output must be clean (CLAUDE.md rule 9). Fix regressions
   before any push; commit fixes as separate commits.

5. **Push to the active PR, if any.**
   - Resolve the remote ref: `git rev-parse --abbrev-ref @{upstream}` (e.g.
     `origin/feat/xyz` — strip the `origin/` prefix for the ref name).
   - Check for an open PR: `gh pr list --head <remote-ref> --state open`.
   - If an open PR exists: `git push --force-with-lease origin HEAD:<remote-ref>`.
     A rebase requires a force push; use `--force-with-lease` only (never plain
     `--force`), and never force-push `main`.
   - If there is no upstream or no open PR: stop after the rebase and tell the
     user the branch was rebased but nothing was pushed.
