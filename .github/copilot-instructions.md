# Project instructions

Read CLAUDE.md at the repository root first, before making any changes. It
covers the build/test commands, architecture, and conventions for this
codebase (wspy: a C instrumentation wrapper for performance-counter
profiling) and applies to you the same as to any other AI coding agent
working in this repo (Claude Code, Cline).

Keep CLAUDE.md as the single source of truth. If you learn something about
this project worth persisting (a build quirk, a convention, an architectural
note), update CLAUDE.md rather than adding a separate/duplicate note here.
This file previously held a full standalone copy of that guidance; it had
already drifted out of date (e.g. referencing a `gpu_smi.c` that no longer
exists — GPU support split into `amd_smi.c`/`amd_sysfs.c`), which is why it
now just points at CLAUDE.md instead of duplicating it.
