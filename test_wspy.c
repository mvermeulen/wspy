#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdint.h>

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
#include "provenance.h"
#include "multipass.h"

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

    // Test 7: --preflight also needs no workload command
    reset_wspy_globals();
    capabilitiesflag = 0; // still set from Test 6 -- reset_wspy_globals() doesn't clear it
    preflightflag = 0;
    char *argv7[] = {"wspy", "--preflight", "--topdown", NULL};
    int argc7 = 3;
    optind = 1;

    if (parse_options(argc7, argv7) != 4) {
        fprintf(stderr, "FAIL: parse_options should return the preflight sentinel\n");
        exit(1);
    }
    assert(preflightflag == 1);
    assert((counter_mask & COUNTER_TOPDOWN) != 0);

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

// Sets up a synthetic (not host-derived) provenance_info: a mix of available
// and unavailable fields, so manifest/run-index JSON assertions don't depend
// on what the CI/dev machine actually has (BIOS DMI files, a live cpufreq
// governor, etc. vary a lot between bare metal, containers, and VMs).
static void fill_fake_provenance(struct provenance_info *p) {
    memset(p, 0, sizeof(*p));
    p->virt_role.available = 1;
    strcpy(p->virt_role.value, "host");
    p->hypervisor_vendor.available = 0;
    strcpy(p->hypervisor_vendor.reason, "not applicable (host, not a guest)");
    p->microcode_version.available = 1;
    strcpy(p->microcode_version.value, "0xdeadbeef");
    p->bios_vendor.available = 0;
    strcpy(p->bios_vendor.reason, "No such file or directory");
    p->bios_version.available = 0;
    strcpy(p->bios_version.reason, "No such file or directory");
    p->bios_date.available = 0;
    strcpy(p->bios_date.reason, "No such file or directory");
    p->cpu_governor.available = 1;
    strcpy(p->cpu_governor.value, "performance");
    p->cpu_scaling_driver.available = 1;
    strcpy(p->cpu_scaling_driver.value, "acpi-cpufreq");
    p->cpu_governor_uniform = 1;
    p->mem_total_kb.available = 1;
    strcpy(p->mem_total_kb.value, "12345678");
    p->compiler_version.available = 1;
    strcpy(p->compiler_version.value, "GCC 13.2.0");
    p->libc_version.available = 1;
    strcpy(p->libc_version.value, "2.38");
}

void test_provenance() {
    struct provenance_info info;
    struct provenance_gap gaps[PROVENANCE_TRACKED_FIELD_COUNT];
    int ngaps, i;

    printf("Testing provenance_collect...\n");

    // Real collection against whatever host is running the tests: don't
    // assert on specific values (they vary by machine/container/CI), just
    // structural invariants -- cpuid-derived virt_role and the compiler
    // version macro always succeed, and the gap accounting is self-consistent.
    provenance_collect(&info);
    assert(info.virt_role.available);
    assert(!strcmp(info.virt_role.value, "host") || !strcmp(info.virt_role.value, "guest"));
    assert(info.compiler_version.available); // this binary was built by gcc/clang, both define __VERSION__

    ngaps = provenance_gaps(&info, gaps);
    assert(ngaps >= 0 && ngaps <= PROVENANCE_TRACKED_FIELD_COUNT);
    assert(provenance_count_available(&info) == PROVENANCE_TRACKED_FIELD_COUNT - ngaps);
    for (i = 0; i < ngaps; i++) {
        assert(gaps[i].field_name != NULL && gaps[i].field_name[0] != '\0');
        assert(gaps[i].reason != NULL && gaps[i].reason[0] != '\0');
    }

    printf("PASS: provenance_collect\n");
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

void test_preflight() {
    struct counter_group g_branch, g_ipc, g_l3, g_soft, g_cache;
    struct counter_info ci_branch[8], ci_ipc[2], ci_l3[1], ci_soft[1], ci_cache[2];
    struct preflight_result pf;
    int saved_nmi;
    int i;
    const char *tmp_out = "/tmp/test_wspy_preflight.txt";
    char *contents;

    printf("Testing counter-fit preflight...\n");

    saved_nmi = nmi_running;
    nmi_running = 0;

    // "ipc": 2 core general-purpose PMU counters
    memset(ci_ipc, 0, sizeof(ci_ipc));
    ci_ipc[0].device_type = PERF_TYPE_RAW;
    ci_ipc[1].device_type = PERF_TYPE_RAW;
    memset(&g_ipc, 0, sizeof(g_ipc));
    g_ipc.label = "ipc";
    g_ipc.type_id = PERF_TYPE_RAW;
    g_ipc.ncounters = 2;
    g_ipc.cinfo = ci_ipc;
    g_ipc.mask = COUNTER_IPC;

    // "l3 cache": uncore (device_type PERF_TYPE_L3, topdown.c's escape hatch
    // inside a PERF_TYPE_RAW group) -- must NOT count against the core
    // general-purpose budget.
    memset(ci_l3, 0, sizeof(ci_l3));
    ci_l3[0].device_type = PERF_TYPE_L3;
    memset(&g_l3, 0, sizeof(g_l3));
    g_l3.label = "l3 cache";
    g_l3.type_id = PERF_TYPE_RAW;
    g_l3.ncounters = 1;
    g_l3.cinfo = ci_l3;
    g_l3.mask = COUNTER_L3CACHE;

    // "software": PERF_TYPE_SOFTWARE has its own separate budget, also
    // must not count.
    memset(ci_soft, 0, sizeof(ci_soft));
    memset(&g_soft, 0, sizeof(g_soft));
    g_soft.label = "software";
    g_soft.type_id = PERF_TYPE_SOFTWARE;
    g_soft.ncounters = 1;
    g_soft.cinfo = ci_soft;
    g_soft.mask = COUNTER_SOFTWARE;

    // "cache": PERF_TYPE_HW_CACHE, 2 core counters
    memset(ci_cache, 0, sizeof(ci_cache));
    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.label = "cache";
    g_cache.type_id = PERF_TYPE_HW_CACHE;
    g_cache.ncounters = 2;
    g_cache.cinfo = ci_cache;
    g_cache.mask = COUNTER_DCACHE|COUNTER_ICACHE;

    g_ipc.next = &g_l3;
    g_l3.next = &g_soft;
    g_soft.next = &g_cache;
    g_cache.next = NULL;

    pf = preflight_evaluate_groups(&g_ipc);
    assert(pf.requested == 4); // 2 ipc + 2 cache; l3 and software excluded
    assert(pf.available == 6); // nmi watchdog not running
    assert(pf.nmi_watchdog_active == 0);
    assert(pf.fits == 1);
    preflight_result_free(&pf);

    // "branch": 8 more core counters pushes the total to 12, over the
    // 6-slot budget.
    memset(ci_branch, 0, sizeof(ci_branch));
    for (i=0;i<8;i++) ci_branch[i].device_type = PERF_TYPE_RAW;
    memset(&g_branch, 0, sizeof(g_branch));
    g_branch.label = "branch";
    g_branch.type_id = PERF_TYPE_RAW;
    g_branch.ncounters = 8;
    g_branch.cinfo = ci_branch;
    g_branch.mask = COUNTER_BRANCH;
    g_branch.next = &g_ipc;

    pf = preflight_evaluate_groups(&g_branch);
    assert(pf.requested == 12);
    assert(pf.fits == 0);
    assert(pf.groups != NULL);
    // largest contributor listed first
    assert(!strcmp(pf.groups->label, "branch"));
    assert(pf.groups->count == 8);

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_preflight_report(&pf);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    assert(strstr(contents, "WILL MULTIPLEX") != NULL);
    assert(strstr(contents, "drop branch") != NULL);
    assert(strstr(contents, "--no-branch") != NULL);
    free(contents);
    remove(tmp_out);
    outfile = NULL;

    preflight_result_free(&pf);

    // NMI watchdog active reserves 1 slot: 6 -> 5
    nmi_running = 1;
    pf = preflight_evaluate_groups(&g_ipc); // 2 ipc + 2 cache = 4, still fits
    assert(pf.available == 5);
    assert(pf.nmi_watchdog_active == 1);
    assert(pf.fits == 1);
    preflight_result_free(&pf);

    nmi_running = saved_nmi;
    printf("PASS: counter-fit preflight\n");
}

// Ground-truth counts below (AMD raw-event table, nmi_running=0, 6-slot
// budget) were confirmed by calling preflight_evaluate() directly against
// these exact masks before writing these assertions -- see the "Native
// multi-pass counter execution" feature's implementation notes, not
// re-derived here to keep this test focused on multipass_plan_build()'s own
// bin-packing behavior rather than re-testing preflight.c's arithmetic.
void test_multipass() {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    int saved_nmi;
    unsigned int bit;
    struct multipass_plan plan;

    printf("Testing native multi-pass counter execution (--passes)...\n");

    // multipass_lookup_group_name(): pure name->bit lookup, no cpu_info needed.
    assert(multipass_lookup_group_name("ipc", &bit) == 1 && bit == COUNTER_IPC);
    assert(multipass_lookup_group_name("topdown-optlb", &bit) == 1 && bit == COUNTER_TOPDOWN_OP);
    assert(multipass_lookup_group_name("software", &bit) == 1 && bit == COUNTER_SOFTWARE);
    assert(multipass_lookup_group_name("bogus", &bit) == 0);

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_AMD;
    cpu_info = &fake_cpu;
    saved_nmi = nmi_running;
    nmi_running = 0;

    // A small request that fits in one pass (ipc alone: 3 counters <= 6).
    plan = multipass_plan_build(COUNTER_IPC);
    assert(plan.npasses == 1);
    assert(plan.pass_mask[0] == COUNTER_IPC);
    multipass_plan_free(&plan);

    // ipc(3) + branch(6) = 9 > 6 combined -- needs 2 passes, neither of
    // which individually multiplexes (verified: ipc alone=3, branch alone=6,
    // both <= 6). Table order (wspy.h's COUNTER_* bit order) puts ipc
    // before branch, so pass 1 should be ipc alone and pass 2 branch alone.
    plan = multipass_plan_build(COUNTER_IPC|COUNTER_BRANCH);
    assert(plan.npasses == 2);
    assert(plan.pass_mask[0] == COUNTER_IPC);
    assert(plan.pass_mask[1] == COUNTER_BRANCH);
    multipass_plan_free(&plan);

    // dcache+icache share one underlying "instructions" counter
    // (cache_events[], topdown.c) -- combined cost is 5, not the naive
    // per-group sum of 3+3=6, and either way both land in the same pass
    // here (5 <= 6). Exercises multipass_plan_build() re-evaluating the
    // real tentative combined mask via preflight_evaluate() rather than
    // summing independently-computed per-bit costs.
    plan = multipass_plan_build(COUNTER_DCACHE|COUNTER_ICACHE);
    assert(plan.npasses == 1);
    assert(plan.pass_mask[0] == (COUNTER_DCACHE|COUNTER_ICACHE));
    multipass_plan_free(&plan);

    // Software counters (PERF_TYPE_SOFTWARE) never compete for the
    // general-purpose PMU budget setup_counter_groups() doesn't even build
    // a group for COUNTER_SOFTWARE by itself -- cache2 alone fits exactly
    // at the 6-slot budget, and software must fold into that same pass
    // rather than triggering a wasteful extra re-execution of the workload.
    plan = multipass_plan_build(COUNTER_L2CACHE|COUNTER_SOFTWARE);
    assert(plan.npasses == 1);
    assert(plan.pass_mask[0] == (COUNTER_L2CACHE|COUNTER_SOFTWARE));
    multipass_plan_free(&plan);

    // Software requested entirely alone still gets exactly one (trivial)
    // pass, without needing any preflight_evaluate() call to decide that.
    plan = multipass_plan_build(COUNTER_SOFTWARE);
    assert(plan.npasses == 1);
    assert(plan.pass_mask[0] == COUNTER_SOFTWARE);
    multipass_plan_free(&plan);

    // topdown2 alone already requests 12 counters, more than fit (6) --
    // still gets exactly one (multiplexing) pass rather than looping or
    // blocking the whole feature.
    plan = multipass_plan_build(COUNTER_TOPDOWN2);
    assert(plan.npasses == 1);
    assert(plan.pass_mask[0] == COUNTER_TOPDOWN2);
    multipass_plan_free(&plan);

    // branch alone (6 counters) fits the normal 6-slot budget exactly, but
    // not the NMI-watchdog-shrunk 5-slot budget -- still gets its own pass
    // (oversized-single-group path via a shrunk budget, not a naturally
    // oversized group like topdown2 above).
    nmi_running = 1;
    plan = multipass_plan_build(COUNTER_BRANCH);
    assert(plan.npasses == 1);
    assert(plan.pass_mask[0] == COUNTER_BRANCH);
    multipass_plan_free(&plan);
    nmi_running = 0;

    // A 3-group request that doesn't fit as a whole (dcache+icache+tlb ==
    // 9 combined, budget 6) must still partition into passes covering the
    // exact requested mask, with no bit duplicated across passes.
    {
        unsigned int requested = COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB;
        unsigned int union_mask = 0;
        int i;

        plan = multipass_plan_build(requested);
        assert(plan.npasses >= 2);
        for (i = 0; i < plan.npasses; i++){
            assert((union_mask & plan.pass_mask[i]) == 0); // no bit reused across passes
            union_mask |= plan.pass_mask[i];
        }
        assert(union_mask == requested);
        multipass_plan_free(&plan);
    }

    // multipass_plan_build_multiplexed() (--passes --multiplex): always
    // exactly one pass covering the whole requested mask, even when
    // multipass_plan_build() would have split it into 2+ passes above.
    {
        unsigned int requested = COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB;

        plan = multipass_plan_build_multiplexed(requested);
        assert(plan.npasses == 1);
        assert(plan.pass_mask[0] == requested);
        multipass_plan_free(&plan);

        // A request that comfortably fits still collapses to one pass --
        // --multiplex isn't "only when it wouldn't fit anyway".
        plan = multipass_plan_build_multiplexed(COUNTER_IPC);
        assert(plan.npasses == 1);
        assert(plan.pass_mask[0] == COUNTER_IPC);
        multipass_plan_free(&plan);
    }

    nmi_running = saved_nmi;
    cpu_info = saved_cpu_info;

    // close_counters(): real fds (from pipe(), so close() is meaningfully
    // exercised), assert every one becomes -1 and intel_group_id resets.
    {
        struct counter_group cgroup;
        struct counter_info cinfo[2];
        int fds[2];

        assert(pipe(fds) == 0);
        memset(cinfo, 0, sizeof(cinfo));
        cinfo[0].fd = fds[0];
        cinfo[0].label = "read-end";
        close(fds[1]);
        assert(pipe(fds) == 0);
        cinfo[1].fd = fds[0];
        cinfo[1].label = "read-end-2";
        close(fds[1]);

        memset(&cgroup, 0, sizeof(cgroup));
        cgroup.label = "test";
        cgroup.ncounters = 2;
        cgroup.cinfo = cinfo;
        cgroup.next = NULL;

        close_counters(&cgroup);
        assert(cinfo[0].fd == -1);
        assert(cinfo[1].fd == -1);
    }

    printf("PASS: native multi-pass counter execution\n");
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
    fill_fake_provenance(&minfo.provenance);

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
    {
      char expected_version[32];
      snprintf(expected_version,sizeof(expected_version),
        "\"wspy_version\": \"%d.%d\"",WSPY_VERSION_MAJOR,WSPY_VERSION_MINOR);
      assert(strstr(contents,expected_version) != NULL);
    }
    assert(strstr(contents, "\"argv\": [\"sleep\", \"1\"]") != NULL);
    assert(strstr(contents, "\"known\": true") != NULL);
    assert(strstr(contents, "\"exit_code\": 0") != NULL);
    assert(strstr(contents, "\"kind\": \"output\"") != NULL);
    assert(strstr(contents, "\"path\": \"out.csv\"") != NULL);
    assert(strstr(contents, "\"kind\": \"tree\"") == NULL); // not requested for this run

    // Environment/provenance capture: available fields serialize as their
    // value, unavailable ones as null (not omitted -- readers shouldn't
    // have to distinguish "absent key" from "checked, not available"), and
    // memory_total_kb is a bare JSON number (not a quoted string).
    assert(strstr(contents, "\"virt_role\": \"host\"") != NULL);
    assert(strstr(contents, "\"hypervisor_vendor\": null") != NULL);
    assert(strstr(contents, "\"microcode_version\": \"0xdeadbeef\"") != NULL);
    assert(strstr(contents, "\"bios_vendor\": null") != NULL);
    assert(strstr(contents, "\"cpu_governor\": \"performance\"") != NULL);
    assert(strstr(contents, "\"cpu_governor_uniform\": true") != NULL);
    assert(strstr(contents, "\"memory_total_kb\": 12345678") != NULL);
    assert(strstr(contents, "\"memory_total_kb\": \"12345678\"") == NULL);
    assert(strstr(contents, "\"compiler_version\": \"GCC 13.2.0\"") != NULL);
    // 3 of the 9 tracked fields (hypervisor_vendor is not tracked) were
    // marked unavailable above: bios_vendor, bios_version, bios_date.
    assert(strstr(contents, "\"environment_coverage\": {\n    \"captured\": 6,\n    \"probed\": 9") != NULL);
    assert(strstr(contents, "\"field\": \"bios_vendor\"") != NULL);
    assert(strstr(contents, "\"reason\": \"No such file or directory\"") != NULL);

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
    fill_fake_provenance(&minfo.provenance);

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

    // Environment/provenance: compact form (leaner than the manifest's
    // pretty-printed JSON), full field values plus a counts-only coverage
    // summary (no per-field gap list, mirroring counter_coverage's leaner
    // run-index treatment).
    assert(strstr(line0, "\"environment\":{\"virt_role\":\"host\"") != NULL);
    assert(strstr(line0, "\"hypervisor_vendor\":null") != NULL);
    assert(strstr(line0, "\"memory_total_kb\":12345678") != NULL);
    assert(strstr(line0, "\"cpu_governor_uniform\":true") != NULL);
    assert(strstr(line0, "\"environment_coverage\":{\"captured\":6,\"probed\":9}") != NULL);

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

void test_arm_topdown_equivalence(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct counter_group cgroup;
    struct counter_info cinfo[8];
    char *contents;
    const char *tmp_out = "/tmp/test_wspy_arm_topdown.txt";

    printf("Testing ARM topdown-equivalent decomposition...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_ARM;
    cpu_info = &fake_cpu;

    memset(cinfo, 0, sizeof(cinfo));
    cinfo[0].label = "op_retired";
    cinfo[0].value = 600000; cinfo[0].time_enabled = 1000000; cinfo[0].time_running = 1000000;
    cinfo[1].label = "stall_slot";
    cinfo[1].value = 400000; cinfo[1].time_enabled = 1000000; cinfo[1].time_running = 1000000;
    cinfo[2].label = "stall_slot_frontend";
    cinfo[2].value = 150000; cinfo[2].time_enabled = 1000000; cinfo[2].time_running = 1000000;
    cinfo[3].label = "stall_slot_backend";
    cinfo[3].value = 250000; cinfo[3].time_enabled = 1000000; cinfo[3].time_running = 1000000;
    cinfo[4].label = "op_spec";
    cinfo[4].value = 700000; cinfo[4].time_enabled = 1000000; cinfo[4].time_running = 1000000;
    cinfo[5].label = "stall_backend_mem";
    cinfo[5].value = 100000;
    cinfo[6].label = "stall_frontend";
    cinfo[6].value = 120000;
    cinfo[7].label = "br_mis_pred_retired";
    cinfo[7].value = 40000;

    memset(&cgroup, 0, sizeof(cgroup));
    cgroup.label = "topdown";
    cgroup.ncounters = 8;
    cgroup.cinfo = cinfo;
    cgroup.mask = COUNTER_TOPDOWN;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    // slots = op_retired + stall_slot = 1,000,000
    assert(strstr(contents, "60.0,") != NULL); // retire
    assert(strstr(contents, "15.0,") != NULL); // frontend
    assert(strstr(contents, "25.0,") != NULL); // backend
    assert(strstr(contents, "10.0,") != NULL); // speculate = (op_spec-op_retired)/slots
    assert(strstr(contents, "110.0,") != NULL); // sanity intentionally highlights overlap risk
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: ARM topdown-equivalent decomposition\n");
}

void test_arm_pmu_report(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct cpu_core_info coreinfo[4];
    char *contents;
    const char *tmp_out = "/tmp/test_wspy_arm_pmu_report.txt";

    printf("Testing ARM PMU topology report...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    memset(coreinfo, 0, sizeof(coreinfo));
    fake_cpu.vendor = VENDOR_ARM;
    fake_cpu.num_cores = 4;
    fake_cpu.num_pmu_clusters = 2;
    fake_cpu.mixed_pmu_types = 1;
    fake_cpu.coreinfo = coreinfo;
    coreinfo[0].pmu_cluster = 0; coreinfo[0].pmu_type = 10;
    coreinfo[1].pmu_cluster = 0; coreinfo[1].pmu_type = 10;
    coreinfo[2].pmu_cluster = 1; coreinfo[2].pmu_type = 11;
    coreinfo[3].pmu_cluster = 1; coreinfo[3].pmu_type = 11;
    cpu_info = &fake_cpu;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_cpu_pmu_report(outfile);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    assert(strstr(contents, "ARM PMU topology: 2 cluster(s)") != NULL);
    assert(strstr(contents, "cluster 0: cpus 0,1 (pmu_type=10)") != NULL);
    assert(strstr(contents, "cluster 1: cpus 2,3 (pmu_type=11)") != NULL);
    assert(strstr(contents, "multiple PMU types") != NULL);
    free(contents);
    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: ARM PMU topology report\n");
}

// Mirrors read_counters()'s own local "struct read_format" (topdown.c) --
// perf's PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING read
// layout. Not shared via a header since it's private to read_counters();
// this test writes raw bytes of this exact shape to a fake fd to drive it
// without needing a real perf_event fd.
struct test_read_format { uint64_t value, time_enabled, time_running, id; };

static void write_fake_perf_read(int fd, uint64_t value, uint64_t time_enabled, uint64_t time_running) {
    struct test_read_format rf = { value, time_enabled, time_running, 0 };
    ssize_t n;
    assert(lseek(fd, 0, SEEK_SET) == 0);
    n = write(fd, &rf, sizeof(rf));
    assert(n == (ssize_t) sizeof(rf));
    assert(lseek(fd, 0, SEEK_SET) == 0);
}

// INVESTIGATION_4.0.md 4.1 Tier 1 #4: a counter multiplexed off the PMU for
// part of a window must have its raw count extrapolated to the full window,
// not just its confidence flagged. Drives read_counters() against a real
// fd (a temp file standing in for a perf_event fd -- read_counters() only
// ever calls plain read() on it) across two simulated --interval ticks to
// confirm .value ends up scaled by *that tick's* running/enabled delta,
// not the ratio accumulated since the run started.
void test_read_counters_multiplex_scaling(void) {
    struct counter_group cgroup;
    struct counter_info cinfo[1];
    char tmpl[] = "/tmp/test_wspy_readctr_XXXXXX";
    int fd;

    printf("Testing read_counters() multiplex scaling...\n");

    fd = mkstemp(tmpl);
    assert(fd != -1);
    unlink(tmpl);

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo[0].label = "test";
    cinfo[0].fd = fd;

    memset(&cgroup, 0, sizeof(cgroup));
    cgroup.label = "test-group";
    cgroup.ncounters = 1;
    cgroup.cinfo = cinfo;

    // Tick 1: cumulative since counter open -- value=1000000, only half the
    // enabled window was actually scheduled (50% multiplexed) -> expect the
    // raw count doubled to estimate the full window.
    write_fake_perf_read(fd, 1000000, 1000000, 500000);
    read_counters(&cgroup, 0);
    assert(cinfo[0].value == 2000000);
    assert(cinfo[0].time_running == 500000);
    assert(cinfo[0].time_enabled == 1000000);

    // Tick 2: cumulative value=1300000 (raw delta 300000 since tick 1),
    // cumulative running=700000 (delta 200000 this tick),
    // cumulative enabled=2000000 (delta 1000000 this tick) -- a much worse
    // 20% multiplex ratio *this tick*, even though the ratio since the run
    // started (700000/2000000=35%) differs. The scaled value must reflect
    // this tick's own ratio, not the cumulative-since-start one.
    write_fake_perf_read(fd, 1300000, 2000000, 700000);
    read_counters(&cgroup, 0);
    assert(cinfo[0].value == 1500000); // 300000 * (1000000/200000)
    assert(cinfo[0].time_running == 200000);
    assert(cinfo[0].time_enabled == 1000000);

    // Tick 3: fully scheduled this tick (running delta == enabled delta) --
    // passes the raw delta through unscaled.
    write_fake_perf_read(fd, 1400000, 2100000, 800000);
    read_counters(&cgroup, 0);
    assert(cinfo[0].value == 100000);

    // Tick 4: never scheduled at all this tick (running delta == 0) -- must
    // not divide by zero, and the raw delta should be 0 (a counter that was
    // never running couldn't have counted any events).
    write_fake_perf_read(fd, 1400000, 2200000, 800000);
    read_counters(&cgroup, 0);
    assert(cinfo[0].value == 0);
    assert(cinfo[0].time_running == 0);

    close(fd);
    printf("PASS: read_counters() multiplex scaling\n");
}

int main(int argc, char **argv) {
    printf("Running Wspy Test Suite...\n");
    test_wspy_parse_options();
    test_write_manifest();
    test_append_run_index();
    test_coverage();
    test_preflight();
    test_multipass();
    test_provenance();
    test_topdown_confidence();
    test_arm_topdown_equivalence();
    test_arm_pmu_report();
    test_read_counters_multiplex_scaling();
    return 0;
}
