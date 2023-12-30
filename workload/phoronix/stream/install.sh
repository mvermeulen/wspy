#!/bin/bash
CC=/opt/AMD/aocc-compiler-4.1.0/bin/clang CCOPTS="-fopenmp -O2 -mcmodel=large -ffp-contract=fast -fnt-store" phoronix-test-suite force-install stream
