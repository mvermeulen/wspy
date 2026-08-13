# zen-portable - quick + ibs-basic composed (AMD, any Zen generation) -- a
# fast, broadly-compatible default: quick's aggregate ipc/system summary
# plus a real AMD IBS interval time series, deliberately avoiding --power
# (RAPL/energy-pkg needs AMD Family 19h+, so Zen1/2 would just warn) and
# IBS l3missonly filtering (a Zen5-only hardware feature, see the Zen5/IBS
# deep-dive in INVESTIGATION.md) so it runs warning-free across the whole
# Zen family.
#
# One line: the comma-separated builtin-profile spec wspy-run's own
# load_profiles() already accepts (same composition wspy-run --list uses
# to expand "deep-cpu,tree-heavy"-style multi-profile arguments).
quick,ibs-basic
