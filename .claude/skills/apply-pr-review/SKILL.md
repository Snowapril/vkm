---
name: apply-pr-review
description: Turn review feedback on the current branch's open PR into one commit per finding, then post a single PR comment recording what was asked, what changed, and which commit did it. Use this whenever the user is reviewing a PR and hands you changes to make — "리뷰할게", "내가 리뷰할게 …", "이거 고쳐줘", "here's my review", "address these comments", "apply this feedback and push", or bare feedback given while a PR is open — even when they don't say the words "commit", "push", or "comment". Also use it to act on findings that /pr-bot-review collected, since that skill deliberately stops at the fix plan. Adds commits only; never rewrites history, and never runs against main.
---

## What this does

Takes review feedback given in conversation and lands it on the branch's open
PR as a readable trail: each finding becomes its own commit, and one summary
comment on the PR maps finding → what changed → commit hash. The comment is the
point. A reviewer's next pass opens with "were my points handled, and where?",
and without a written answer they have to re-read the whole diff to find out —
or worse, cannot tell a deliberate "no" from a comment you missed.

This is the write half of the review loop. `/pr-bot-review` is the read half:
it collects bot and human findings and prints a prioritized plan, but its tools
are read-only and it explicitly declines to post anything back. When the user
asks for the fixes to actually happen, that is this skill.

Local branch and remote ref names may differ (worktree branches often push to a
differently named remote ref), so the remote ref is always resolved from the
branch's upstream, never assumed.

## Steps

1. **Resolve the PR and check the tree before touching anything.**
   - `git status --short` first. Unrelated uncommitted work would be swept into
     the per-finding commits below, which is exactly what makes the trail
     unreadable. Tool state left behind by other skills is expected noise and
     can be left alone — `.claude/.session-report-marker`, `.omc/state/*.json`.
     Stop and surface anything else. Never use bare `git stash` — the stash
     stack is shared across worktrees and other sessions.
   - Resolve the remote ref: `git rev-parse --abbrev-ref @{upstream}`, stripping
     the `origin/` prefix.
   - `gh pr list --head <remote-ref> --state open --json number,title,url`.
   - If there is no upstream or no open PR, stop and say so. Committing review
     fixes onto a branch with nowhere to report them defeats the purpose; the
     user may have meant a different branch.

2. **Enumerate the findings before fixing any of them.**
   Restate the feedback as a numbered list of discrete, checkable items and show
   it to the user. One sentence of Korean or English review often bundles three
   separate asks, and both the commit structure and the final comment key off
   this list — splitting it later, after the edits are made, means untangling
   changes that have already been mixed together.

   Decide here which items you will *not* act on, and why. That decision belongs
   in the comment, so make it deliberately rather than discovering at the end
   that something quietly went unaddressed.

3. **Fix and commit one finding at a time.**
   One finding per commit, so each can be reverted or re-reviewed on its own and
   the comment can link them 1:1. Record each short hash as you commit —
   reconstructing the mapping from `git log` afterwards is guesswork once the
   messages no longer mention which finding they came from.

   Commit messages follow the repo's existing style: imperative, sentence case,
   no prefix, no trailing period, roughly 50-70 characters, describing the
   change rather than the review that prompted it. `git log` is read by people
   who never saw the review thread.

   | Good | Bad |
   |---|---|
   | `Copy pool data instead of retaining structure pointers` | `Address review comment 2` |
   | `Autorelease the bitmap rep; this target builds without ARC` | `fix: review feedback` |

4. **Log a deviation when the fix isn't what was asked for.**
   Per CLAUDE.md §11, if an edge case forced a different choice than the
   reviewer requested, take the conservative option and record it under the
   `## Deviations` section of `implementation-notes.md`. Locate it with
   `grep -n "^## Deviations" implementation-notes.md` rather than by scrolling —
   it sits in the middle of a very long file, and the file currently contains
   two copies of the section (a duplication worth mentioning to the user, not
   worth silently repairing here). Append to the first one. Newest entry first:

   ```markdown
   ### <YYYY-MM-DD> — <short title>
   - Planned: <what the review asked for>
   - Did instead: <the conservative option taken>
   - Why: <the edge case that forced it>
   ```

   The same material becomes the comment's "Judgment calls" section, so writing
   it once here covers both.

5. **Verify before pushing.**
   Build and run `python3 scripts/run_tests.py`. Validation-layer output must be
   clean (CLAUDE.md §9). Two traps specific to this repo:
   - The script exits 0 even when `bootstrap.py` fails or a backend reports
     FAIL, so read the Test Summary table rather than trusting the exit code.
   - Check `TODO.md` before calling a failure a regression — it records known
     intermittent failures, and treating one as yours sends you hunting a bug
     that isn't there.

   Fix real regressions before pushing, as their own commits. Pushing a red
   branch spends the reviewer's next pass on noise you already knew about.

6. **Push, then post one comment.**
   Plain `git push origin HEAD:<remote-ref>` — no force flags. This skill only
   appends commits, and CLAUDE.md §10 rules out history rewriting; if you find
   yourself reaching for `--force-with-lease`, something has gone wrong that the
   user should hear about first.

   Then post the comment with `gh pr comment <number> --body`, using the
   template below.

## The comment

```markdown
## Review round <n>

| Finding | What changed | Commit |
|---|---|---|
| <the ask, in English><br><sub><i><the reviewer's original wording, when it wasn't English></i></sub> | <what was actually done> | `<short hash>` |

### Judgment calls
- <where the fix diverged from what was asked, and why>

### Not changed
- **<finding>** — <the reason>
```

The table is what a reviewer reads first: their own point on the left, a hash on
the right to jump straight to the change.

Reviews here are often given in Korean, and the comment is written in English —
it matches the existing PR bodies and CLAUDE.md §5, and the PR outlives the
conversation. So translate the finding rather than pasting the original into an
otherwise-English table: a reader who doesn't read Korean cannot check whether
their colleague's point was handled, which is the one job this table has.

Keep the original wording underneath the translation, small and italic. The
translation is what makes the table usable; the original is what makes it
verifiable. Wording carries weight in review — "원하지 않음" is *doesn't want
this*, not *should not* — and the author needs to recognize their own sentence
to confirm you read it the way they meant it. Drop the second line when the
review was already in English; there is nothing to preserve.

The other two sections exist because silence is ambiguous. A finding that simply
doesn't appear reads as an oversight, and a reviewer has no way to tell that
from a considered decision unless you write the decision down. Drop a section
entirely when it has nothing in it rather than leaving it saying "None".

## Notes

- Number the rounds (`Review round 2`, and so on) when a PR goes through several
  passes, so the history stays readable as comments accumulate.
- When the findings came from inline diff comments rather than conversation,
  link each one from the table so the threads stay traceable. Posting one
  summary is still right — judgment calls and skipped findings are cross-cutting
  and don't belong scattered across individual threads.
- Reaching this skill from `/pr-bot-review` is the intended path: that skill's
  "do not re-post findings unless the user explicitly asks" is about
  volunteering unsolicited comments, not about this one, which the user asked
  for.
