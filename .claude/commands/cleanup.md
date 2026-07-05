---
description: Run scripts/run_clean to remove local build/dependency artifacts
argument-hint: "[--deps] [--dry-run] [--build-dir <path>]"
---

# Cleanup Local Artifacts

Remove locally generated build and dependency artifacts.

1. Pick the OS-appropriate wrapper: `scripts/run_clean.sh` (macOS/Linux) or
   `scripts/run_clean.ps1` (Windows).
2. Run it with any arguments the user passed (`$ARGUMENTS`), e.g.
   `./scripts/run_clean.sh $ARGUMENTS`.
3. If the user asks to also wipe dependencies (re-downloaded on next
   `bootstrap.py` run), pass `--deps`.
4. Report which directories/files were removed (or would be, for
   `--dry-run`).
