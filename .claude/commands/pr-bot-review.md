---
description: Collect PR bot and human reviews, list them, and create a prioritized fix plan
argument-hint: "[PR number or URL]"
allowed-tools: Bash(git branch:*), Bash(gh pr list:*), Bash(gh pr view:*), Bash(gh api:*)
---

# PR Bot Review Collector

[PR BOT REVIEW MODE ACTIVATED]

Invoke the `pr-bot-review` skill to:
1. Auto-detect the current PR from the active branch (or use the provided PR number/URL)
2. Collect all reviews and comments via the GitHub API (bot reviews, inline diff comments, general thread comments)
3. Categorize findings by source: **BOT** (CI, coverage, static analysis) vs. **HUMAN** (team reviewers)
4. Display a structured list of all findings with file:line context
5. Produce a prioritized fix plan (CRITICAL → HIGH → MEDIUM → LOW)

Use this skill now with the argument: $ARGUMENTS
