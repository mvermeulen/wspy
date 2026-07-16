/*
 * run_index.h - shared "run index" file: one compact JSON record per run,
 * appended (not rewritten) so tooling can query "all runs" without
 * scraping output directories. Independent of --manifest -- a run can use
 * either, neither, or both.
 */
#ifndef _WSPY_RUN_INDEX_H
#define _WSPY_RUN_INDEX_H 1

#include "manifest.h"

/* SemVer of the run index *record* shape (one JSON object per line).
 * Deliberately separate from MANIFEST_SCHEMA_VERSION: the index record is a
 * leaner, line-oriented projection of a run (not the manifest itself), and
 * its shape can evolve on its own schedule. Bump MAJOR when a field is
 * removed/renamed, MINOR when a field is added, PATCH for fixes that don't
 * change the shape. */
#define RUN_INDEX_SCHEMA_VERSION "1.6.0"

/* Appends one compact JSON record (JSON Lines format: one self-contained
 * object per line, newline-terminated, no enclosing array) describing this
 * run to path, creating the file if it doesn't exist. Safe for multiple
 * concurrent wspy processes to append to the same shared index file --
 * writes are serialized with flock(LOCK_EX) so records from different
 * processes never interleave. Returns 0 on success, -1 if the file could
 * not be opened or locked (a warning is logged in that case). */
int append_run_index(const char *path,const struct manifest_info *info);

#endif
