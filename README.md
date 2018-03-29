# wspy
wspy - a workload spy

This program is an instrumentation wrapper and testbed for project experiments.
At present, the following instrumentation is included:
* ftrace based (kernel tracing):
  --processtree generation using scheduler instrumentation of fork/exec/exit
* timer based sampling of /proc and /sys/structures
  --cpustats generation of user+system+idle time using /proc/stat
  --diskstats generation of disk activity using /sys/block/*/stats
  --memstats generation of memory using /proc/meminfo (to be added)
  --netstats generation of network using /sys/dev/net (to be added)
* timer based sampling of performance counters
  --perfcounters generation of simple performance counters
The code is mostly my own internal base for project experiments. Added to
github to make it easy to make it accessible from multiple places. Listed as
public if it is otherwise of interest.
