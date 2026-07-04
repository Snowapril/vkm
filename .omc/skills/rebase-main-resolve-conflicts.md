---
id: rebase-main-resolve-conflicts
name: Fetch and rebase main branch commits and resolve conflicts
description: Rebase the current feature branch on top of origin/main, resolving conflicts by preserving the feature branch's intent, then force-push.
source: learned
triggers:
  - "rebase main"
  - "fetch and rebase"
  - "rebase mainline"
  - "sync with main"
  - "resolve rebase conflict"
quality: project
---

# Fetch and Rebase Main, Resolve Conflicts

## The Insight

When rebasing a feature branch onto main, conflicts almost always mean main has a partial or
earlier version of the same code you've already improved. The resolution heuristic: **your branch
is the truth** — keep your changes, discard the conflicting fragment from main.

## Why This Matters

A naïve conflict resolution (accepting "theirs" or "ours" blindly) either throws away your work or
leaves stale code. The right mental model: main's conflicting hunk is an older/incomplete edit
that your commit supersedes — not a competing improvement to merge with.

## Recognition Pattern

- You're on a feature branch that modifies the same file(s) recently touched on main
- `git rebase origin/main` exits with "CONFLICT (content)" in a `.mm`, `.cpp`, or `.h` file
- The HEAD side of the conflict marker is a shorter or older version of logic your commit expands

## The Approach

1. **Fetch and start the rebase**
   ```bash
   git fetch origin main && git rebase origin/main
   ```

2. **For each conflicted file**, open it and look at the conflict markers:
   - `<<<<<<< HEAD` — what main has (older/partial)
   - `>>>>>>> <sha>` — what your commit has (the Metal 4 / feature version)
   - Keep the `=======` … `>>>>>>>` side; delete the `<<<<<<< HEAD` … `=======` side and all markers.

3. **Stage and continue**
   ```bash
   git add <conflicted-file>
   GIT_EDITOR=true git rebase --continue   # skip editor prompt for commit message
   ```
   - Use `GIT_EDITOR=true` to avoid an interactive editor opening mid-rebase.
   - Do NOT use `--no-edit` — it is not a valid flag for `git rebase --continue`.

4. **Force-push with lease** (safe: fails if the remote has unexpected commits)
   ```bash
   git push --force-with-lease origin <branch>
   ```

## Example

Conflict seen in this project (`metal_command_queue.mm`):
```
<<<<<<< HEAD
        return VkmGpuEventTimelineObject{gpuEventTimelineMetal, lastSubmittedTimelineValue};
=======

        [_mtlCommandQueue commit:mtlCommandBuffers count:count];
        [_mtlCommandQueue signalEvent:mtlSharedEvent value:lastSubmittedTimelineValue];

        return VkmGpuEventTimelineObject(gpuEventTimelineMetal, lastSubmittedTimelineValue);
>>>>>>> 01d3d71 (Migrate Metal renderer to Metal 4)
```
Resolution: keep the `=======` … `>>>>>>>` block (our Metal 4 batch-commit logic).
