---
description: Generate an HTML report (with a comprehension quiz) of changes made since the last report
argument-hint: "[none]"
---

# Session Change Report

Produce an HTML artifact that lets the user catch up on what changed in the repo since the
last time this command ran, with enough context to actually understand *why*, plus a short
quiz at the bottom that checks they understood it.

## 1. Determine the reporting window

- Marker file: `.claude/.session-report-marker` — a single line containing the commit hash
  of `HEAD` as of the last report.
- If the marker exists and differs from current `HEAD`, the window is
  `git log <marker>..HEAD` (committed changes) plus the current working tree diff.
- If the marker doesn't exist (first run), the window is just the current working tree diff
  (`git diff HEAD`) — do not dump the whole repo history.
- If there is nothing in the window (no new commits and a clean working tree), say so and stop
  — don't generate an empty report.

## 2. Gather material

- `git log <marker>..HEAD --stat` and the full `git show` for each commit in range, for context
  on intent (read commit messages — they carry the "why").
- `git diff HEAD` for uncommitted changes (staged + unstaged).
- The `## Deviations` section of `implementation-notes.md` (repo root) — include any entries
  that look new (not already covered by a prior report); these carry the reasoning behind
  edge-case decisions and are high-value content for the report.
- Skim changed files only as needed to explain *why* a change was made when the commit
  message/diff comment doesn't already say — don't re-derive things already obvious from the
  diff.

## 3. Build the report

Load the `artifact-design` skill before writing HTML. Then write a self-contained HTML file
(no external requests) covering, per logical change (group by commit or by feature, not by
raw file):
- What changed, in plain language.
- Why — the intuition/context: the problem it solves, the constraint that shaped it, or the
  deviation reasoning if it came from `implementation-notes.md`.
- The relevant diff snippet, syntax-highlighted, kept short (the interesting hunk, not the
  whole file).

At the bottom, add a quiz: 4-8 questions generated specifically from the actual changes in
this report (never generic filler questions). Mix formats (multiple choice / short answer).
Show correct/incorrect feedback client-side with plain inline JS (no external libraries) —
this must work fully offline since the artifact CSP blocks external requests. Make it clear
this is meant to be "passed" (i.e. show a score, and which ones were missed) but don't gate
navigation on it.

Publish via the `Artifact` tool. Keep the favicon emoji stable across reruns of this command
(pick one the first time, e.g. 📋, and reuse it) — check `Artifact({action: "list"})` for a
prior "Session Report" artifact URL to update in place rather than minting a new one each run.

## 4. Update the marker

After publishing, write the current `git rev-parse HEAD` into `.claude/.session-report-marker`
(overwrite). This file is local dev state, not something to gitignore-fight over — leave it
untracked if it isn't already tracked.

## 5. Report back

Give the user the artifact URL and a one-sentence summary of what the report covers.
