#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>

// Define TEST_WSPY to allow including wspy.c without main()
#define TEST_WSPY 1
#define AMDGPU 0 // Disable GPU for basic unit tests to avoid dependency complexity

// We need to include wspy.c
// But first we need to make sure we can compile it.
// We will modify wspy.c to guard main() with #ifndef TEST_WSPY

#include "wspy.c"

// Helper to reset globals for wspy
void reset_wspy_globals() {
    aflag = 0;
    oflag = 0;
    sflag = 0;
    vflag = 0;
   // Globals are defined in wspy.c, which is included below.
// We don't need to redeclare them.
    if (outfile && outfile != stdout && outfile != stderr) fclose(outfile);
    outfile = NULL;
    if (treefile) fclose(treefile);
    treefile = NULL;
}

void test_wspy_parse_options() {
    printf("Testing wspy parse_options...\n");

    // Test 1: Basic flags
    reset_wspy_globals();
    char *argv1[] = {"wspy", "--csv", "--verbose", "ls", NULL};
    int argc1 = 4;
    optind = 1; 
    
    if (parse_options(argc1, argv1) != 0) {
        fprintf(stderr, "FAIL: parse_options returned error for valid args\n");
        exit(1);
    }
    assert(csvflag == 1);
    assert(vflag == 1);
    
    // Test 2: Counter flags
    reset_wspy_globals();
    char *argv2[] = {"wspy", "--branch", "--icache", "ls", NULL};
    int argc2 = 4;
    optind = 1;
    
    parse_options(argc2, argv2);
    assert((counter_mask & COUNTER_BRANCH) != 0);
    assert((counter_mask & COUNTER_ICACHE) != 0);

    printf("PASS: wspy parse_options\n");
}

int main(int argc, char **argv) {
    printf("Running Wspy Test Suite...\n");
    test_wspy_parse_options();
    return 0;
}
