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
        "1234 (test) S 999 1 1 0 -1 0 0 0 0 0 " // 0-12 (3: ppid)
        "100 200 " // 13 (utime), 14 (stime)
        "0 0 0 0 4 " // 15-19 (19: num_threads)
        "0 " // 20
        "5000 " // 21 (starttime)
        "1048576 " // 22 (vsize)
        "256 0 0 0 0 0 0 0 0 0 0 0 0 0 0 " // 23-37 (23: rss)
        "3 " // 38 (processor)
        "0 0 0 0 0 0 0 0 0 0 0 0 " // 39-50
        "0" // 51 (exit_code)
    );

    parse_stat(stat_str, &pinfo);

    assert(pinfo.ppid == 999);
    assert(pinfo.utime == 100);
    assert(pinfo.stime == 200);
    assert(pinfo.num_threads == 4);
    assert(pinfo.starttime == 5000);
    assert(pinfo.vsize == 1048576);
    assert(pinfo.rss == 256);
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

void test_proctree_build_comm_table() {
    printf("Testing proctree build_comm_table...\n");

    struct process_info root, child;
    memset(&root, 0, sizeof(root));
    memset(&child, 0, sizeof(child));
    root.pid = 1;
    root.comm = "root_proc";
    root.utime = 100;
    root.stime = 10;
    root.children = &child;
    child.pid = 2;
    child.comm = "child_proc";
    child.utime = 50;
    child.stime = 5;
    child.parent = &root;

    root_process = &root;
    comm_totals = NULL;
    num_comm_info = 0;

    int count;
    struct comm_info *table = build_comm_table(&count);
    assert(count == 2);

    int found_root = 0, found_child = 0;
    int i;
    for (i = 0; i < count; i++) {
        if (!strcmp(table[i].comm, "root_proc")) {
            assert(table[i].total_utime == 100);
            assert(table[i].total_stime == 10);
            assert(table[i].count == 1);
            found_root = 1;
        } else if (!strcmp(table[i].comm, "child_proc")) {
            assert(table[i].total_utime == 50);
            found_child = 1;
        }
    }
    assert(found_root && found_child);
    free(table);

    printf("PASS: proctree build_comm_table\n");
}

// Builds a tiny synthetic tree by hand, emits it via print_json(), and
// parses the result back with json_reader.c to confirm the JSON shape
// round-trips -- mirroring test_plot.c's/test_affinity.c's "drive internal
// functions against synthetic fixtures" convention.
void test_proctree_json_export() {
    printf("Testing proctree JSON export round-trip...\n");

    struct process_info root, child;
    memset(&root, 0, sizeof(root));
    memset(&child, 0, sizeof(child));
    clocks_per_second = 100;
    root.pid = 100;
    root.comm = "parent";
    root.children = &child;
    child.pid = 101;
    child.comm = "kid";
    child.parent = &root;
    child.futex_wait_count = 3;
    child.futex_wait_seconds = 1.5;

    root_process = &root;
    total_processes = 2;
    max_num_processes = 2;
    comm_totals = NULL;
    num_comm_info = 0;

    char *buf = NULL;
    size_t bufsize = 0;
    FILE *mem = open_memstream(&buf, &bufsize);
    print_json(mem, "test.txt");
    fclose(mem);

    char errbuf[256];
    struct json_value *doc = json_parse(buf, errbuf, sizeof(errbuf));
    assert(doc != NULL);

    assert(!strcmp(json_get_string(doc, "schema_version", ""), PROCTREE_JSON_SCHEMA_VERSION));
    assert(!strcmp(json_get_string(doc, "source_file", ""), "test.txt"));
    assert((int)json_get_number(doc, "process_count", -1) == 2);

    const struct json_value *tree = json_object_get(doc, "tree");
    assert(tree != NULL);
    assert(!strcmp(json_get_string(tree, "comm", ""), "parent"));
    assert((int)json_get_number(tree, "pid", 0) == 100);

    const struct json_value *children = json_object_get(tree, "children");
    assert(json_array_len(children) == 1);
    const struct json_value *kid = json_array_get(children, 0);
    assert(!strcmp(json_get_string(kid, "comm", ""), "kid"));
    assert((int)json_get_number(kid, "futex_wait_count", 0) == 3);
    assert(json_get_number(kid, "futex_wait_seconds", 0) == 1.5);

    json_free(doc);
    free(buf);

    printf("PASS: proctree JSON export round-trip\n");
}

// Hand-built JSON fixtures (the shape --json would produce) covering the
// three merge_nodes() outcomes a run-to-run diff cares about: a matched
// node under threshold ("same"), a matched node over threshold
// ("changed"), a node only on one side ("removed"/"added").
void test_proctree_diff_merge() {
    printf("Testing proctree diff merge...\n");

    const char *json_a =
        "{\"tree\":{\"comm\":\"shell\",\"children\":["
        "{\"comm\":\"worker\",\"utime_seconds\":1.0,\"children\":[]},"
        "{\"comm\":\"worker\",\"utime_seconds\":2.0,\"children\":[]},"
        "{\"comm\":\"cleanup\",\"utime_seconds\":0.1,\"children\":[]}"
        "]}}";
    const char *json_b =
        "{\"tree\":{\"comm\":\"shell\",\"children\":["
        "{\"comm\":\"worker\",\"utime_seconds\":1.0,\"children\":[]},"
        "{\"comm\":\"worker\",\"utime_seconds\":9.0,\"children\":[]},"
        "{\"comm\":\"logger\",\"utime_seconds\":0.2,\"children\":[]}"
        "]}}";

    char errbuf[256];
    struct json_value *doc_a = json_parse(json_a, errbuf, sizeof(errbuf));
    assert(doc_a != NULL);
    struct json_value *doc_b = json_parse(json_b, errbuf, sizeof(errbuf));
    assert(doc_b != NULL);

    const struct json_value *tree_a = json_object_get(doc_a, "tree");
    const struct json_value *tree_b = json_object_get(doc_b, "tree");

    struct diff_node_list flat;
    memset(&flat, 0, sizeof(flat));
    struct diff_node *root = merge_nodes(tree_a, tree_b, "", 0, 0.5, &flat);

    assert(!strcmp(root->status, "matched"));
    assert(root->num_children == 4); // worker#0, worker#1, cleanup, logger

    int found_worker_same = 0, found_worker_changed = 0;
    int found_cleanup_removed = 0, found_logger_added = 0;
    size_t i;
    for (i = 0; i < root->num_children; i++) {
        struct diff_node *child = root->children[i];
        if (!strcmp(child->comm, "worker") && !strcmp(child->status, "matched") && !child->changed) {
            found_worker_same = 1;
            assert(fabs(child->delta[DIFF_METRIC_UTIME] - 0.0) < 1e-9);
        } else if (!strcmp(child->comm, "worker") && !strcmp(child->status, "matched") && child->changed) {
            found_worker_changed = 1;
            assert(fabs(child->delta[DIFF_METRIC_UTIME] - 7.0) < 1e-9);
        } else if (!strcmp(child->comm, "cleanup") && !strcmp(child->status, "removed")) {
            found_cleanup_removed = 1;
        } else if (!strcmp(child->comm, "logger") && !strcmp(child->status, "added")) {
            found_logger_added = 1;
        }
    }
    assert(found_worker_same);
    assert(found_worker_changed);
    assert(found_cleanup_removed);
    assert(found_logger_added);

    json_free(doc_a);
    json_free(doc_b);
    printf("PASS: proctree diff merge\n");
}

int main(int argc, char **argv) {
    printf("Running Proctree Test Suite...\n");
    test_proctree_parse_stat();
    test_proctree_lookup_pid();
    test_proctree_build_comm_table();
    test_proctree_json_export();
    test_proctree_diff_merge();
    return 0;
}
