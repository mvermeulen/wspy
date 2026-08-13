# zen4plus-deep - deep-cpu + ibs-sample + tree-heavy composed (AMD Family
# 19h+, i.e. Zen4/Zen5) -- deep-cpu's full topdown/cache/branch sweep
# (already includes --power, real on Family 19h+) plus IBS sampling mode's
# named memory-hierarchy breakdown (human-readable .txt, no --interval --
# see ibs-sample.conf), plus a --tree pass for process-tree capture
# (auto-timed by estimate_tree_pass_timeouts() same as tree-heavy on its
# own).
#
# wspy-run's load_builtin_profile() applies one small patch after loading
# this spec: an extra --no-rusage on the composed "ibs" pass, since this
# composition's "counters" pass (deep-cpu's --passes sweep) already
# captures the same elapsed/utime/stime/nvcsw/... block ibs-sample's own
# rusage-on default would otherwise duplicate in ibs.txt. Kept as a small
# named special-case in wspy-run itself (one profile, one line) rather than
# a general per-pass-flag-append mechanism in this file format.
deep-cpu,ibs-sample,tree-heavy
