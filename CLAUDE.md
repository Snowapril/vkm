# CLAUDE.md

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

## 5. Language Policy

**Conversation may be in Korean or English. Code and documentation must always be in English.**

- All source code, comments, variable names, and identifiers: English only.
- All documentation files (README, docs, etc.): English only.
- Conversation with the user: Korean or English, whichever the user prefers.

## 6. Project Layout
```
vkm/
├── buildscripts
├── dependencies # Third-party libraries that installed by bootstrap.py will be located. Should not be modified by user/claude.
├── include
├── resources
├── src
├── tests
├── ...

```

## 7. Prefer STL Over Custom Implementations

**If the C++ Standard Library already provides the functionality, use it instead of writing new code.**

- Before implementing a utility, check `<algorithm>`, `<numeric>`, `<utility>`, `<functional>`, `<memory>`, etc. for an existing equivalent.
- Prefer `std::` containers, algorithms, and utilities over hand-rolled versions of the same behavior.
- Only write a custom implementation when the STL genuinely lacks the needed functionality or has a documented limitation (e.g. performance, API fit) that rules it out.

## 8. TODO.md Conventions

**Entries are single lines. No descriptions, rationale, or "why/fix" explanations.**

- Add new items as one short bullet line each.
- Do not elaborate, justify, or explain the item — only the user adds additional detail to TODO.md.

## 9. Validation Layer Errors Are Must-Fix

**Never ignore, suppress, or work around a Vulkan/Metal validation layer error or warning.**

- Unit tests always run with `enableValidationLayer = true` / `MTL_DEBUG_LAYER=1` (see
  `tests/*`, `scripts/run_tests.py`) specifically so these errors surface.
- Any validation layer error/warning encountered while running tests or samples is a
  required fix, not something to silence, skip, or filter out of logs.

## 10. Commit Policy

**Commit changes once a requested task is finished — don't wait to be asked each time.**

- After completing each user request that changes files, create a commit for it.
- Still follow standard git safety rules (no force-push, no history rewriting, no `--no-verify`) unless explicitly requested.

## 11. Implementation Notes & Deviation Logging

**Keep `implementation-notes.md` (repo root) updated while implementing a plan.**

- If an edge case forces a deviation from the agreed plan, pick the conservative option
  (the one least likely to change behavior or break invariants outside the task's scope),
  log it under the `## Deviations` section of `implementation-notes.md`, and keep going
  instead of stopping to ask — unless the conservative option itself is ambiguous.
- Each deviation entry: what was planned, what was done instead, and why.
- Run `/session-report` to generate a readable HTML summary (with a comprehension quiz)
  of a session's changes, including any logged deviations.