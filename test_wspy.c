#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <sys/wait.h>

// Define TEST_WSPY to allow including wspy.c without main()
#define TEST_WSPY 1
#define AMDGPU 0 // Disable GPU for basic unit tests to avoid dependency complexity

// We need to include wspy.c
// But first we need to make sure we can compile it.
// We will modify wspy.c to guard main() with #ifndef TEST_WSPY

#include "wspy.c"
#include "manifest.h"
#include "run_index.h"
#include "coverage.h"

// Helper to reset globals for wspy
void reset_wspy_globals() {
    aflag = 0;
    oflag = 0;
    sflag = 0;
    vflag = 0;
    versionflag = 0;
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

    // Test 3: Version flag
    reset_wspy_globals();
    char *argv3[] = {"wspy", "--version", NULL};
    int argc3 = 2;
    optind = 1;

    if (parse_options(argc3, argv3) != 2) {
        fprintf(stderr, "FAIL: parse_options should return version sentinel\n");
        exit(1);
    }
    assert(versionflag == 1);

    // Test 4: --manifest <file>
    reset_wspy_globals();
    manifest_path = NULL;
    char *argv4[] = {"wspy", "--manifest", "/tmp/does-not-need-to-exist.json", "ls", NULL};
    int argc4 = 4;
    optind = 1;

    if (parse_options(argc4, argv4) != 0) {
        fprintf(stderr, "FAIL: parse_options returned error for --manifest\n");
        exit(1);
    }
    assert(manifest_path != NULL);
    assert(strcmp(manifest_path, "/tmp/does-not-need-to-exist.json") == 0);

    // Test 5: --run-index <file>
    reset_wspy_globals();
    run_index_path = NULL;
    char *argv5[] = {"wspy", "--run-index", "/tmp/does-not-need-to-exist.jsonl", "ls", NULL};
    int argc5 = 4;
    optind = 1;

    if (parse_options(argc5, argv5) != 0) {
        fprintf(stderr, "FAIL: parse_options returned error for --run-index\n");
        exit(1);
    }
    assert(run_index_path != NULL);
    assert(strcmp(run_index_path, "/tmp/does-not-need-to-exist.jsonl") == 0);

    // Test 6: --capabilities needs no workload command, unlike a normal run
    reset_wspy_globals();
    capabilitiesflag = 0;
    char *argv6[] = {"wspy", "--capabilities", NULL};
    int argc6 = 2;
    optind = 1;

    if (parse_options(argc6, argv6) != 3) {
        fprintf(stderr, "FAIL: parse_options should return the capabilities sentinel\n");
        exit(1);
    }
    assert(capabilitiesflag == 1);

    printf("PASS: wspy parse_options\n");
}

void test_coverage() {
    printf("Testing counter coverage tracking...\n");

    coverage_reset();
    assert(coverage_requested == 0);
    assert(coverage_measured == 0);
    assert(coverage_entries == NULL);

    coverage_note("ipc", "instructions", 1, 0);
    coverage_note("ipc", "cpu-cycles", 0, EACCES);
    coverage_note("cache", "l1d-read", 1, 0);

    assert(coverage_requested == 3);
    assert(coverage_measured == 2);
    assert(coverage_entries != NULL);
    assert(!strcmp(coverage_entries->group_label, "ipc"));
    assert(!strcmp(coverage_entries->counter_label, "instructions"));
    assert(coverage_entries->available == 1);
    assert(coverage_entries->next->available == 0);
    assert(coverage_entries->next->open_errno == EACCES);

    coverage_reset();
    assert(coverage_requested == 0);
    assert(coverage_entries == NULL);

    printf("PASS: counter coverage tracking\n");
}

// Read the whole file into a malloc'd, NUL-terminated buffer for substring checks.
static char *slurp_file(const char *path) {
    FILE *fp;
    long size;
    char *buf;

    fp = fopen(path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc(size + 1);
    if (fread(buf, 1, size, fp) != (size_t) size) {
        fclose(fp);
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

void test_write_manifest() {
    struct manifest_info minfo;
    char *contents;
    const char *manifest_out = "/tmp/test_wspy_manifest.json";
    char *test_argv[] = { "sleep", "1" };
    struct manifest_counter_gap gaps[1] = {{ "ipc", "cpu-cycles", EACCES }};

    printf("Testing write_manifest...\n");

    if (!cpu_info) inventory_cpu();

    memset(&minfo, 0, sizeof(minfo));
    minfo.start_time.tv_sec = 1000;
    minfo.finish_time.tv_sec = 1001;
    minfo.argc = 2;
    minfo.argv = test_argv;
    minfo.exit_status.known = 1;
    minfo.exit_status.exited = 1;
    minfo.exit_status.exit_code = 0;
    minfo.counter_mask = COUNTER_IPC;
    minfo.csvflag = 1;
    minfo.output_path = "out.csv";
    minfo.tree_output_path = NULL;
    minfo.manifest_path = manifest_out;
    minfo.counters_requested = 3;
    minfo.counters_measured = 2;
    minfo.counters_unavailable_count = 1;
    minfo.counters_unavailable = gaps;

    if (write_manifest(manifest_out, &minfo) != 0) {
        fprintf(stderr, "FAIL: write_manifest returned an error\n");
        exit(1);
    }

    contents = slurp_file(manifest_out);
    if (!contents) {
        fprintf(stderr, "FAIL: could not read back manifest file\n");
        exit(1);
    }

    // Counter capability coverage: requested/measured counts and the
    // unavailable-counter detail must round-trip into the JSON.
    assert(strstr(contents, "\"requested\": 3") != NULL);
    assert(strstr(contents, "\"measured\": 2") != NULL);
    assert(strstr(contents, "\"group\": \"ipc\"") != NULL);
    assert(strstr(contents, "\"counter\": \"cpu-cycles\"") != NULL);

    // Schema version must be present and match the SemVer constant exactly --
    // downstream tooling depends on this field to detect shape changes.
    assert(strstr(contents, "\"schema_version\": \"" MANIFEST_SCHEMA_VERSION "\"") != NULL);
    assert(strstr(contents, "\"wspy_version\": \"3.0\"") != NULL);
    assert(strstr(contents, "\"argv\": [\"sleep\", \"1\"]") != NULL);
    assert(strstr(contents, "\"known\": true") != NULL);
    assert(strstr(contents, "\"exit_code\": 0") != NULL);
    assert(strstr(contents, "\"kind\": \"output\"") != NULL);
    assert(strstr(contents, "\"path\": \"out.csv\"") != NULL);
    assert(strstr(contents, "\"kind\": \"tree\"") == NULL); // not requested for this run

    free(contents);
    remove(manifest_out);

    // exit status not observed (e.g. --tree path): must be null, not garbage
    memset(&minfo, 0, sizeof(minfo));
    minfo.argc = 0;
    minfo.exit_status.known = 0;
    minfo.manifest_path = manifest_out;
    if (write_manifest(manifest_out, &minfo) != 0) {
        fprintf(stderr, "FAIL: write_manifest returned an error\n");
        exit(1);
    }
    contents = slurp_file(manifest_out);
    assert(contents != NULL);
    assert(strstr(contents, "\"known\": false") != NULL);
    assert(strstr(contents, "\"exited\": null") != NULL);
    free(contents);
    remove(manifest_out);

    printf("PASS: write_manifest\n");
}

// Extract the substring between the n-th and (n+1)-th '\n' (0-indexed), or
// NULL if the buffer doesn't have that many lines. Caller must free().
static char *nth_line(const char *contents, int n) {
    const char *start = contents;
    const char *end;
    int i;

    for (i = 0; i < n; i++) {
        start = strchr(start, '\n');
        if (!start) return NULL;
        start++;
    }
    end = strchr(start, '\n');
    if (!end) return NULL;
    return strndup(start, end - start);
}

void test_append_run_index() {
    struct manifest_info minfo;
    char *contents, *line0, *line1;
    const char *index_out = "/tmp/test_wspy_run_index.jsonl";
    char *test_argv[] = { "sleep", "1" };

    printf("Testing append_run_index...\n");

    if (!cpu_info) inventory_cpu();
    remove(index_out);

    memset(&minfo, 0, sizeof(minfo));
    minfo.start_time.tv_sec = 1000;
    minfo.finish_time.tv_sec = 1001;
    minfo.argc = 2;
    minfo.argv = test_argv;
    minfo.exit_status.known = 1;
    minfo.exit_status.exited = 1;
    minfo.exit_status.exit_code = 0;
    minfo.counter_mask = COUNTER_IPC;
    minfo.csvflag = 1;
    minfo.output_path = "out.csv";
    minfo.manifest_path = "run.manifest.json";
    minfo.counters_requested = 3;
    minfo.counters_measured = 1;

    if (append_run_index(index_out, &minfo) != 0) {
        fprintf(stderr, "FAIL: append_run_index returned an error (first record)\n");
        exit(1);
    }

    // Second record, distinct start_time so it gets a distinct run_id even
    // though it's appended by the same pid.
    minfo.start_time.tv_sec = 2000;
    minfo.finish_time.tv_sec = 2001;
    minfo.exit_status.exit_code = 0;
    if (append_run_index(index_out, &minfo) != 0) {
        fprintf(stderr, "FAIL: append_run_index returned an error (second record)\n");
        exit(1);
    }

    contents = slurp_file(index_out);
    if (!contents) {
        fprintf(stderr, "FAIL: could not read back run index file\n");
        exit(1);
    }

    // Append must not truncate: both records must be present as separate lines.
    line0 = nth_line(contents, 0);
    line1 = nth_line(contents, 1);
    if (!line0 || !line1) {
        fprintf(stderr, "FAIL: run index file does not have two lines\n");
        exit(1);
    }
    // A third line (beyond the trailing newline of line1) would mean stray output.
    assert(nth_line(contents, 2) == NULL);

    assert(strstr(line0, "\"schema_version\":\"" RUN_INDEX_SCHEMA_VERSION "\"") != NULL);
    assert(strstr(line1, "\"schema_version\":\"" RUN_INDEX_SCHEMA_VERSION "\"") != NULL);
    assert(strstr(line0, "\"command\":[\"sleep\",\"1\"]") != NULL);
    assert(strstr(line0, "\"manifest_path\":\"run.manifest.json\"") != NULL);
    assert(strstr(line0, "\"exit_code\":0") != NULL);
    assert(strstr(line0, "\"counter_coverage\":{\"requested\":3,\"measured\":1}") != NULL);

    // Each line must be independently valid, self-contained JSON (a curly
    // brace per line, no shared array wrapper).
    assert(line0[0] == '{' && line0[strlen(line0) - 1] == '}');
    assert(line1[0] == '{' && line1[strlen(line1) - 1] == '}');

    // Distinct start_time -> distinct run_id, even from the same pid.
    {
        char *run_id0 = strstr(line0, "\"run_id\":\"");
        char *run_id1 = strstr(line1, "\"run_id\":\"");
        assert(run_id0 != NULL && run_id1 != NULL);
        assert(strncmp(run_id0, run_id1, 24) != 0);
    }

    free(line0);
    free(line1);
    free(contents);
    remove(index_out);

    printf("PASS: append_run_index\n");
}

// Declared in topdown.c (linked in via test_topdown.o) but not exposed
// through wspy.h since only print_metrics() is part of the public surface.
extern void print_topdown(struct counter_group *cgroup, enum output_format oformat, int mask);

void test_topdown_confidence(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct counter_group cgroup;
    struct counter_info cinfo[5];
    char *contents;
    const char *tmp_out = "/tmp/test_wspy_topdown.txt";

    printf("Testing topdown confidence envelope + sanity checks...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_INTEL;
    cpu_info = &fake_cpu;

    memset(cinfo, 0, sizeof(cinfo));
    cinfo[0].label = "slots";
    cinfo[0].value = 1000000; cinfo[0].time_enabled = 1000000; cinfo[0].time_running = 1000000;
    cinfo[1].label = "core.topdown-retiring";
    cinfo[1].value = 400000; cinfo[1].time_enabled = 1000000; cinfo[1].time_running = 1000000;
    cinfo[2].label = "core.topdown-fe-bound";
    cinfo[2].value = 300000; cinfo[2].time_enabled = 1000000; cinfo[2].time_running = 1000000;
    // deliberately multiplexed: only half the enabled window was actually scheduled
    cinfo[3].label = "core.topdown-be-bound";
    cinfo[3].value = 250000; cinfo[3].time_enabled = 1000000; cinfo[3].time_running = 500000;
    cinfo[4].label = "core.topdown-bad-spec";
    cinfo[4].value = 50000; cinfo[4].time_enabled = 1000000; cinfo[4].time_running = 1000000;

    memset(&cgroup, 0, sizeof(cgroup));
    cgroup.label = "topdown";
    cgroup.ncounters = 5;
    cgroup.cinfo = cinfo;
    cgroup.mask = COUNTER_TOPDOWN;

    // retiring+frontend+backend+speculation == slots here (400k+300k+250k+50k == 1e6):
    // a clean decomposition, but backend's counter was only half-scheduled.
    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    assert(strstr(contents, "40.0,") != NULL);   // retiring % of slots
    assert(strstr(contents, "0.50,") != NULL);   // overall confidence: min() pulled down by backend
    assert(strstr(contents, "100.0,") != NULL);  // sanity: sum matches slots exactly
    free(contents);

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_NORMAL, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    // only backend's own counter was multiplexed -- the annotation should land on
    // that row, not on rows backed by fully-scheduled counters.
    assert(strstr(contents, "backend") != NULL);
    assert(strstr(contents, "low-confidence(50%)") != NULL);
    assert(strstr(contents, "sanity check") != NULL);
    free(contents);

    // Now break the decomposition: bump backend's value so the four
    // components no longer sum anywhere near slots.
    cinfo[3].value = 550000;
    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_NORMAL, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    assert(strstr(contents, "decomposition looks inconsistent") != NULL);
    free(contents);

    // Regression check: when every topdown counter failed to open (perf
    // permissions, unsupported event -- fd never opened, so read_counters()
    // never touched value/time_running/time_enabled and they stay at their
    // calloc()'d 0), the CSV row must still carry all 6 columns declared in
    // the header instead of silently vanishing and shifting every column
    // after this group left relative to the header.
    memset(cinfo, 0, sizeof(cinfo));
    cinfo[0].label = "slots";
    cinfo[1].label = "core.topdown-retiring";
    cinfo[2].label = "core.topdown-fe-bound";
    cinfo[3].label = "core.topdown-be-bound";
    cinfo[4].label = "core.topdown-bad-spec";

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    {
        int commas = 0;
        char *p;
        for (p = contents; *p; p++) if (*p == ',') commas++;
        // retire, frontend, backend, speculate, confidence, sanity == 6 fields
        if (commas != 6) {
            fprintf(stderr, "FAIL: expected 6 CSV columns when topdown counters are unavailable, got %d (row: %s)\n", commas, contents);
            exit(1);
        }
    }
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: topdown confidence envelope + sanity checks\n");
}

int main(int argc, char **argv) {
    printf("Running Wspy Test Suite...\n");
    test_wspy_parse_options();
    test_write_manifest();
    test_append_run_index();
    test_coverage();
    test_topdown_confidence();
    return 0;
}
