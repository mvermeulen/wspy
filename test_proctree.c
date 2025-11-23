#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <getopt.h>

// Define TEST_PROCTREE to allow including proctree.c without main()
#define TEST_PROCTREE 1

// We include proctree.c directly to test its internal functions
#include "proctree.c"

void test_proctree_parse_stat() {
    printf("Testing proctree parse_stat...\n");
    
    struct process_info pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    
    // Sample /proc/pid/stat content (partial)
    char stat_str[1024];
    // We need at least 52 fields based on NUM_STAT_FIELDS
    sprintf(stat_str, 
        "1234 (test) S 1 1 1 0 -1 0 0 0 0 0 " // 0-12
        "100 200 " // 13 (utime), 14 (stime)
        "0 0 0 0 0 " // 15-19
        "0 " // 20
        "5000 " // 21 (starttime)
        "1048576 " // 22 (vsize)
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 " // 23-37
        "3 " // 38 (processor)
        "0 0 0 0 0 0 0 0 0 0 0 0 " // 39-50
        "0" // 51 (exit_code)
    );
    
    parse_stat(stat_str, &pinfo);
    
    assert(pinfo.utime == 100);
    assert(pinfo.stime == 200);
    assert(pinfo.starttime == 5000);
    assert(pinfo.vsize == 1048576);
    assert(pinfo.processor == 3);
    
    printf("PASS: proctree parse_stat\n");
}

void test_proctree_lookup_pid() {
    printf("Testing proctree lookup_pid...\n");
    
    // Ensure table is clean (globals from included proctree.c)
    memset(proc_table, 0, sizeof(proc_table));
    num_processes = 0;
    
    struct proc_table_entry *entry = lookup_pid(1234, 1); // Insert
    assert(entry != NULL);
    assert(entry->pid == 1234);
    assert(num_processes == 1);
    
    struct proc_table_entry *found = lookup_pid(1234, 0); // Lookup
    assert(found == entry);
    
    struct proc_table_entry *not_found = lookup_pid(9999, 0); // Lookup non-existent
    assert(not_found == NULL);
    
    printf("PASS: proctree lookup_pid\n");
}

int main(int argc, char **argv) {
    printf("Running Proctree Test Suite...\n");
    test_proctree_parse_stat();
    test_proctree_lookup_pid();
    return 0;
}
