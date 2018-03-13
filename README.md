# wspy
wspy - a workload spy

This program is a simple wrapper that enables instrumentation of a program. At first two "spies" are created:
-- trace; uses linux kernel ftrace facility to record scheduler events for fork/exec/exit and uses these to
   construct a process tree.
-- timer; creates a periodic timer that wakes up to read system state. State sampled is contents of /proc/stat
   for the cpu* lines.
   
The code is mostly my internal project experiments and test base to extend in creating instrumentation. Added to
github to make it accessible from multiple places. Made public to enable others to see it if of interest.
