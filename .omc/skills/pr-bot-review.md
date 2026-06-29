---
name: pr-bot-review
description: Collect PR bot and human reviews, list them with context, and create a prioritized fix plan for each finding
argument-hint: "[PR number or URL] (optional, defaults to current branch's PR)"
---

# PR Bot Review Collector

Collect all reviews and comments from a pull request, categorize them by source (bot vs. human), and produce a prioritized fix plan.

## Phase 1: Detect PR

If an argument (PR number or URL) was provided, use it directly. Otherwise, auto-detect from the current branch:

```bash
BRANCH=$(git branch --show-current)
gh pr list --head "$BRANCH" --json number,title,url,headRepositoryOwner,headRepository --limit 1
```

If no PR is found, report clearly and stop.

Extract:
- `PR_NUMBER` — numeric PR ID
- `OWNER` — repo owner (e.g. `snowapril`)
- `REPO` — repo name (e.g. `vkm`)

## Phase 2: Collect All Reviews and Comments

Run these three calls and capture the JSON output:

```bash
# Full-file reviews (approve / request changes / comment)
gh api "repos/$OWNER/$REPO/pulls/$PR_NUMBER/reviews" --paginate

# Inline diff comments (attached to specific lines)
gh api "repos/$OWNER/$REPO/pulls/$PR_NUMBER/comments" --paginate

# General issue-style comments on the PR thread
gh api "repos/$OWNER/$REPO/issues/$PR_NUMBER/comments" --paginate
```

## Phase 3: Categorize Reviewers

For each comment/review, classify the author:

**Bot detection rules:**
- `login` ends with `[bot]` — e.g. `github-actions[bot]`, `codecov[bot]`
- `login` ends with `_bot`
- `type` field is `"Bot"`
- Known bot logins: `dependabot`, `renovate`, `sonarcloud`, `mergify`, `stale`, `linear`, `snyk-bot`, `deepsource-autofix`

Group into:
- **BOT REVIEWS** — automated checks, CI bots, coverage tools
- **HUMAN REVIEWS** — code reviews from team members

## Phase 4: Display Collected Reviews

Output a structured report:

```
PR REVIEW COLLECTION
====================
PR #<number>: <title>
Branch: <branch> → <base>

BOT REVIEWS (<count>)
---------------------
[<bot-login>] <review state or comment type>
  Body: <first 200 chars of body>
  File: <path>:<line>  (if inline comment)
  URL:  <html_url>

...

HUMAN REVIEWS (<count>)
-----------------------
[<login>] <review state: APPROVED / REQUEST_CHANGES / COMMENTED>
  Body: <first 200 chars>
  File: <path>:<line>  (if inline comment)
  URL:  <html_url>

...

TOTAL: <bot_count> bot + <human_count> human findings
```

If a review has no actionable body (e.g. empty approve), note it as `(no actionable comment)` and skip it in the fix plan.

## Phase 5: Build Fix Plan

For each actionable finding, determine:

1. **Severity**
   - `CRITICAL` — CI/build failure, security issue
   - `HIGH` — test failure, logic bug, blocking review request
   - `MEDIUM` — code quality, style violation, non-blocking review note
   - `LOW` — suggestion, cosmetic, informational bot report

2. **Location** — file path and line number if available

3. **Action** — specific change required (not vague; cite file:line)

Output format:

```
FIX PLAN
========

CRITICAL (<count>)
------------------
1. [github-actions[bot]] Build failure
   Severity: CRITICAL
   Location: src/vulkan/queue.cpp:87
   Finding: Missing #include <vulkan/vulkan.h>
   Action: Add `#include <vulkan/vulkan.h>` at the top of the file
   Source: <html_url>

HIGH (<count>)
--------------
2. [reviewer] Null pointer dereference
   Severity: HIGH
   Location: src/render/pipeline.cpp:134
   Finding: `device` may be null before dereference
   Action: Add null check: `if (!device) return VK_ERROR_INITIALIZATION_FAILED;`
   Source: <html_url>

MEDIUM (<count>)
----------------
...

LOW (<count>)
-------------
...

SUMMARY
-------
Total findings: <n>
Suggested fix order: address CRITICAL first, then HIGH, then MEDIUM/LOW together.
```

## Severity Heuristics

| Signal | Severity |
|--------|----------|
| CI check failed, build error | CRITICAL |
| Test failure | CRITICAL |
| `REQUEST_CHANGES` review state | HIGH |
| Security warning (sonarcloud, snyk) | HIGH |
| Coverage drop > 5% | MEDIUM |
| Coverage drop 1-5% | LOW |
| Style comment, suggestion | LOW |
| `COMMENTED` review (no state change) | MEDIUM or LOW based on body |
| `APPROVED` with note | LOW |

## Use With Other Skills

**Auto-fix all findings:**
```
/oh-my-claudecode:ralph fix all findings from pr-bot-review
```

**Re-review after fixes:**
```
/oh-my-claudecode:code-review
```

**Targeted fix with persistence:**
```
/oh-my-claudecode:ralph: fix the CRITICAL and HIGH items from the PR review plan
```

## Notes

- Always paginate API calls (`--paginate`) to avoid missing comments on large PRs.
- Do not re-post findings as PR comments unless the user explicitly asks.
- If a bot comment is purely informational (e.g. deployment URL), classify as LOW and note it is informational only.
- For bots that post multiple comments (e.g. one per test failure), group them under a single bot entry with sub-items.
