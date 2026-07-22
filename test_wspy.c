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
    config_provenance_preset = NULL;
    config_provenance_configuration = NULL;
    config_provenance_options = NULL;
    config_provenance_noptions = 0;
    config_provenance_options_cap = 0;
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

    // Test 8: structured configuration provenance (--preset-name/--config-name/
    // --config-option) is metadata-only -- parses successfully, doesn't affect
    // counter_mask/csvflag/etc, and a malformed --config-option (no '=') is
    // warned about and skipped rather than rejected.
    reset_wspy_globals();
    capabilitiesflag = 0; // still set from Test 6 -- reset_wspy_globals() doesn't clear it
    preflightflag = 0;    // still set from Test 7 -- reset_wspy_globals() doesn't clear it
    char *argv8[] = {"wspy", "--preset-name", "deep-cpu",
                      "--config-name", "performance-counters",
                      "--config-option", "counter_groups=topdown",
                      "--config-option", "not-key-value",
                      "--config-option", "interval_seconds=1",
                      "ls", NULL};
    int argc8 = 12;
    optind = 1;

    if (parse_options(argc8, argv8) != 0) {
        fprintf(stderr, "FAIL: parse_options returned error for config provenance flags\n");
        exit(1);
    }
    assert(config_provenance_preset != NULL);
    assert(strcmp(config_provenance_preset, "deep-cpu") == 0);
    assert(config_provenance_configuration != NULL);
    assert(strcmp(config_provenance_configuration, "performance-counters") == 0);
    // Only the two well-formed key=value options are kept; the malformed one
    // is skipped (not fatal, not silently miscounted).
    assert(config_provenance_noptions == 2);
    assert(strcmp(config_provenance_options[0].name, "counter_groups") == 0);
    assert(strcmp(config_provenance_options[0].value, "topdown") == 0);
    assert(strcmp(config_provenance_options[1].name, "interval_seconds") == 0);
    assert(strcmp(config_provenance_options[1].value, "1") == 0);

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

// INVESTIGATION.md's 4.2 Tier 1 "Per-core energy support" item:
// power_core_counter_group() (power.c) marks a non-representative-CPU
// counter with POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE and pre-sets fd=-1;
// setup_counters() (topdown.c) must skip perf_event_open() *and*
// coverage_note() entirely for it -- not "requested but failed", which
// would skew counters_requested/measured and preflight.c's budget estimate
// for every SMT-sibling CPU a real hybrid --per-core run has. A real
// perf_event_open() attempt on this synthetic group would also just fail
// in this test environment regardless of privilege (device_type 9998 isn't
// a real dynamic PMU type), so this specifically proves the *skip* path
// was taken, not merely that the open failed.
void test_power_core_skip_not_attempted(void) {
    struct counter_group cgroup;
    struct counter_info cinfo[1];

    printf("Testing power_core skip counter -- setup_counters() doesn't attempt or count it...\n");

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo[0].label = "core_joules";
    cinfo[0].device_type = POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE;
    cinfo[0].fd = -1; /* power_core_counter_group() always pre-sets this for a skip counter */

    memset(&cgroup, 0, sizeof(cgroup));
    cgroup.label = "power-core";
    cgroup.type_id = PERF_TYPE_RAW;
    cgroup.mask = COUNTER_POWER_CORE;
    cgroup.target_cpu = 1;
    cgroup.ncounters = 1;
    cgroup.cinfo = cinfo;

    coverage_reset();
    setup_counters(&cgroup);

    assert(coverage_requested == 0);
    assert(coverage_measured == 0);
    assert(cgroup.cinfo[0].fd == -1);

    printf("PASS: power_core skip counter not attempted/not counted\n");
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
    // exercised), assert every one becomes -1.
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

// Regression test for a real bug (found 2026-07-15 via a production
// counters.txt full of zeros): setup_raw_events() only parses a raw_event
// table entry's .config when events[i].use intersects counter_mask -- but
// --passes leaves counter_mask at its untouched COUNTER_IPC default and
// carries the actual requested groups in passes_requested_mask instead, so
// any raw event needed only by a non-default pass group used to keep its
// zero-initialized .config, open via perf_event_open() as a meaningless
// always-zero raw event, and never get caught by coverage reporting (which
// only checks that the fd opened). Confirmed live via strace: `wspy
// --topdown-frontend` opened real configs like 0x10000188e; `wspy
// --passes=topdown-frontend` opened config=0 for the same counters. main()
// now fixes this by OR-ing passes_requested_mask into counter_mask before
// calling setup_raw_events() when multipass_flag is set -- this test pins
// that contract at the setup_raw_events()/table level (see also
// setup_raw_events()'s own comment in topdown.c, and run_capabilities_probe()
// in wspy.c, which had the same bug in reverse call order).
void test_multipass_raw_event_parsing(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    unsigned int saved_counter_mask;
    unsigned int saved_passes_requested_mask;
    int saved_multipass_flag;
    unsigned int i;
    int found;

    printf("Testing --passes raw event parsing covers non-default pass groups...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_AMD;
    cpu_info = &fake_cpu;

    saved_counter_mask = counter_mask;
    saved_passes_requested_mask = passes_requested_mask;
    saved_multipass_flag = multipass_flag;

    // Zero out the whole table first so a pass here can't be explained by
    // some earlier test (or setup_raw_events() call) having already parsed
    // these entries.
    for (i = 0; i < (unsigned int)amd_raw_events_count(); i++)
        amd_raw_events[i].raw.config = 0;

    // Simulate exactly `wspy --passes=topdown-frontend`: counter_mask
    // stays at its default (COUNTER_IPC), passes_requested_mask carries
    // the real request -- same state parse_options() leaves behind.
    counter_mask = COUNTER_IPC;
    passes_requested_mask = COUNTER_TOPDOWN_FE;
    multipass_flag = 1;

    // main()'s fix, applied the same way main() applies it.
    if (multipass_flag) counter_mask |= passes_requested_mask;
    setup_raw_events();

    // These 5 events are used only by COUNTER_TOPDOWN_FE, not COUNTER_IPC
    // -- before the fix, every one of them stayed at .raw.config == 0.
    // Expected values cross-checked against union amd_raw_cpu_format's
    // bitfield layout (event:8, umask:8@bit8, event2:4@bit32), e.g.
    // event=0x18e,umask=0x18 -> event2=0x1,event=0x8e,umask=0x18 ->
    // (0x1<<32)|(0x18<<8)|0x8e == 0x10000188e.
    struct { const char *name; unsigned long expected_config; } cases[] = {
        { "ic_tag_hit_miss.instruction_cache_miss",      0x10000188eUL },
        { "ic_tag_hit_miss.instruction_cache_accesses",  0x100001f8eUL },
        { "bp_l1_tlb_miss_l2_tlb_hit",                   0x84UL },
        { "bp_l1_tlb_miss_l2_tlb_miss.all",              0xfUL },
        { "ls_tlb_flush.all",                            0xff78UL },
    };
    unsigned int c;

    for (c = 0; c < sizeof(cases)/sizeof(cases[0]); c++){
        found = 0;
        for (i = 0; i < (unsigned int)amd_raw_events_count(); i++){
            if (!strcmp(amd_raw_events[i].name,cases[c].name)){
                found = 1;
                assert(amd_raw_events[i].raw.config == cases[c].expected_config);
                break;
            }
        }
        assert(found); // table entry must exist, or this test is checking nothing
    }

    counter_mask = saved_counter_mask;
    passes_requested_mask = saved_passes_requested_mask;
    multipass_flag = saved_multipass_flag;
    cpu_info = saved_cpu_info;

    printf("PASS: --passes raw event parsing covers non-default pass groups\n");
}

void test_write_manifest() {
    struct manifest_info minfo;
    char *contents;
    const char *manifest_out = "/tmp/test_wspy_manifest.json";
    char *test_argv[] = { "sleep", "1" };
    struct manifest_counter_gap gaps[1] = {{ "ipc", "cpu-cycles", EACCES }};
    struct manifest_config_option options[2] = {
        { "counter_groups", "topdown" },
        { "interval_seconds", "1" },
    };

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
    minfo.config_provenance.preset = "deep-cpu";
    minfo.config_provenance.configuration = "performance-counters";
    minfo.config_provenance.noptions = 2;
    minfo.config_provenance.options = options;
    minfo.gpu.gpu_metrics_requested = 1;
    minfo.gpu.amd_device_index = 1;
    minfo.gpu.nvidia_device_index = -1;
    minfo.gpu.amd_sysfs_busy_valid = 1;
    minfo.gpu.amd_sysfs_metrics_valid = 1;
    minfo.gpu.amd_smi_metrics_valid = 0;
    minfo.gpu.amd_smi_memory_valid = 1;
    minfo.cgroup.available = 1;
    minfo.cgroup.path = "/user.slice/user-1000.slice/session-2.scope";
    minfo.cgroup.cpu_max_available = 1;
    minfo.cgroup.cpu_quota_us = 200000;
    minfo.cgroup.cpu_period_us = 100000;
    minfo.cgroup.cpu_weight_available = 0; /* e.g. cpu controller not enabled on this leaf */
    minfo.cgroup.memory_max_available = 1;
    minfo.cgroup.memory_max_bytes = -1; /* "max", unlimited */
    minfo.cgroup.memory_high_available = 1;
    minfo.cgroup.memory_high_bytes = 1073741824LL;
    minfo.cgroup.throttle_available = 1;
    minfo.cgroup.nr_periods_delta = 42;
    minfo.cgroup.nr_throttled_delta = 7;
    minfo.cgroup.throttled_usec_delta = 123456;

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

    // Structured configuration provenance (INVESTIGATION.md's "What
    // shipped in 4.1"):
    // preset/configuration round-trip as given, and both options appear in
    // order as { "name", "value" } objects.
    assert(strstr(contents, "\"schema_version\": \"" MANIFEST_SCHEMA_VERSION "\"") != NULL);
    assert(strstr(contents, "\"preset\": \"deep-cpu\"") != NULL);
    assert(strstr(contents, "\"configuration\": \"performance-counters\"") != NULL);
    assert(strstr(contents, "\"name\": \"counter_groups\", \"value\": \"topdown\"") != NULL);
    assert(strstr(contents, "\"name\": \"interval_seconds\", \"value\": \"1\"") != NULL);

    // GPU telemetry provenance (INVESTIGATION.md's 4.2 Tier 1 "manifest/
    // index/profile pipeline extended to GPU runs" item): only --gpu-metrics
    // was requested, so it alone is true; amd_device_index round-trips as a
    // bare number, nvidia_device_index (-1, not requested) as null, and
    // backend_valid reflects each backend's own last-read outcome
    // independently (amd_smi_metrics failed while amd_sysfs_metrics/
    // amd_smi_memory succeeded -- a real combination confirmed on hardware).
    assert(strstr(contents, "\"busy\": false") != NULL);
    assert(strstr(contents, "\"metrics\": true") != NULL);
    assert(strstr(contents, "\"smi\": false") != NULL);
    assert(strstr(contents, "\"nvidia\": false") != NULL);
    assert(strstr(contents, "\"amd_device_index\": 1") != NULL);
    assert(strstr(contents, "\"nvidia_device_index\": null") != NULL);
    assert(strstr(contents, "\"amd_sysfs_busy\": true") != NULL);
    assert(strstr(contents, "\"amd_sysfs_metrics\": true") != NULL);
    assert(strstr(contents, "\"amd_smi_metrics\": false") != NULL);
    assert(strstr(contents, "\"amd_smi_memory\": true") != NULL);
    assert(strstr(contents, "\"nvidia_metrics\": false") != NULL);

    // cgroup identity/limits/throttling (INVESTIGATION.md's 4.2 Tier 1
    // "cgroup identity + limits in manifest, cpu.stat throttling stats"
    // item): path/limits round-trip, cpu_weight (unavailable this time --
    // e.g. cpu controller not enabled on this leaf) reads "available":
    // false with no "value" key at all (not a bare 0/null placeholder),
    // and the throttle delta (not the raw start/end snapshots) is what's
    // reported.
    assert(strstr(contents, "\"available\": true,\n    \"path\": \"/user.slice/user-1000.slice/session-2.scope\"") != NULL);
    assert(strstr(contents, "\"cpu_max\": { \"available\": true, \"quota_us\": 200000, \"period_us\": 100000 }") != NULL);
    assert(strstr(contents, "\"cpu_weight\": { \"available\": false }") != NULL);
    assert(strstr(contents, "\"cpu_weight\": { \"available\": false, \"value\"") == NULL);
    assert(strstr(contents, "\"memory_max_bytes\": { \"available\": true, \"value\": -1 }") != NULL);
    assert(strstr(contents, "\"memory_high_bytes\": { \"available\": true, \"value\": 1073741824 }") != NULL);
    assert(strstr(contents, "\"throttle\": { \"available\": true, \"nr_periods_delta\": 42, "
                            "\"nr_throttled_delta\": 7, \"throttled_usec_delta\": 123456 }") != NULL);

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
    // No launcher metadata given this time: preset/configuration null, no options.
    assert(strstr(contents, "\"preset\": null") != NULL);
    assert(strstr(contents, "\"configuration\": null") != NULL);
    assert(strstr(contents, "\"options\": [\n    ]") != NULL);
    // No GPU flag given at all (a zeroed struct, same as a plain wspy
    // invocation with no --gpu-* flags): both device indices must be null,
    // not garbage/0-looks-like-device-0, and every requested/backend_valid
    // flag reads false.
    assert(strstr(contents, "\"amd_device_index\": null") != NULL);
    assert(strstr(contents, "\"nvidia_device_index\": null") != NULL);
    // No cgroup path either (a zeroed struct, same as a host with no
    // cgroup v2 unified hierarchy): available false, path null, no other
    // cgroup sub-object claims to have a value.
    assert(strstr(contents, "\"cgroup\": {\n    \"available\": false,\n    \"path\": null") != NULL);
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
    minfo.config_provenance.preset = "deep-cpu";
    minfo.config_provenance.configuration = "performance-counters";
    minfo.gpu.gpu_metrics_requested = 1;
    minfo.gpu.amd_device_index = 1;
    minfo.gpu.nvidia_device_index = -1;
    minfo.gpu.amd_sysfs_busy_valid = 1;
    minfo.gpu.amd_sysfs_metrics_valid = 1;
    minfo.gpu.amd_smi_metrics_valid = 0;
    minfo.gpu.amd_smi_memory_valid = 1;
    minfo.cgroup.available = 1;
    minfo.cgroup.path = "/user.slice/user-1000.slice/session-2.scope";
    minfo.cgroup.cpu_max_available = 1;
    minfo.cgroup.cpu_quota_us = 200000;
    minfo.cgroup.cpu_period_us = 100000;
    minfo.cgroup.memory_max_available = 1;
    minfo.cgroup.memory_max_bytes = -1;
    minfo.cgroup.memory_high_available = 1;
    minfo.cgroup.memory_high_bytes = 1073741824LL;
    minfo.cgroup.throttle_available = 1;
    minfo.cgroup.nr_periods_delta = 42;
    minfo.cgroup.nr_throttled_delta = 7;
    minfo.cgroup.throttled_usec_delta = 123456;

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

    // Structured configuration provenance (INVESTIGATION.md's "What
    // shipped in 4.1"):
    // compact form, preset/configuration given, no options this time.
    assert(strstr(line0, "\"schema_version\":\"" RUN_INDEX_SCHEMA_VERSION "\"") != NULL);
    assert(strstr(line0, "\"configuration_provenance\":{\"preset\":\"deep-cpu\",\"configuration\":\"performance-counters\",\"options\":[]}") != NULL);

    // GPU telemetry provenance: compact form, mirroring the manifest's own
    // pretty-printed shape (see test_write_manifest() for the same scenario
    // in that form).
    assert(strstr(line0, "\"gpu\":{\"requested\":{\"busy\":false,\"metrics\":true,\"smi\":false,\"nvidia\":false},"
                         "\"amd_device_index\":1,\"nvidia_device_index\":null,"
                         "\"backend_valid\":{\"amd_sysfs_busy\":true,\"amd_sysfs_metrics\":true,"
                         "\"amd_smi_metrics\":false,\"amd_smi_memory\":true,\"nvidia_metrics\":false}}") != NULL);

    // cgroup identity/limits/throttling: compact form, mirroring the
    // manifest's own pretty-printed shape.
    assert(strstr(line0, "\"cgroup\":{\"available\":true,\"path\":\"/user.slice/user-1000.slice/session-2.scope\","
                         "\"cpu_max\":{\"available\":true,\"quota_us\":200000,\"period_us\":100000},"
                         "\"cpu_weight\":{\"available\":false},"
                         "\"memory_max_bytes\":{\"available\":true,\"value\":-1},"
                         "\"memory_high_bytes\":{\"available\":true,\"value\":1073741824},"
                         "\"throttle\":{\"available\":true,\"nr_periods_delta\":42,"
                         "\"nr_throttled_delta\":7,\"throttled_usec_delta\":123456}}") != NULL);

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
extern void print_topdown_be(struct counter_group *cgroup, enum output_format oformat, int mask);
extern struct counter_group *raw_counter_group(char *name, unsigned int mask);
extern struct counter_group *cache_counter_group(char *name, unsigned int mask);

static void free_test_cgroup(struct counter_group *cgroup) {
    if (!cgroup) return;
    free(cgroup->cinfo);
    free(cgroup->label);
    free(cgroup);
}

// INVESTIGATION.md's 4.3 Tier 0 #1: the single-shared-Intel-group design used
// to funnel every non-topdown Intel raw/cache counter into one perf event
// group with no size limit, cascading into wholesale EINVAL loss once a
// combined group exceeded real hardware PMU capacity. The fix makes
// cache_counter_group()/raw_counter_group() chunk Intel counters into
// hardware-budget-respecting groups (is_group_leader every
// available_counters/num_counters_available counters) exactly like they
// already did for AMD/ARM, while keeping the topdown/topdown2 Perf Metrics
// family as a single dedicated group regardless of size (a kernel-enforced
// "literal slots leader" requirement, not a PMC-budget one -- see
// raw_counter_group()'s own comment). This test locks in both halves of
// that behavior directly against the real intel_raw_events[]/cache_events[]
// tables, without needing real hardware/perf_event_open() access.
void test_intel_counter_grouping(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    int saved_nmi;
    struct counter_group *cgroup;
    int i;

    printf("Testing Intel counter-group budget chunking (4.3 Tier 0 #1)...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_INTEL;
    cpu_info = &fake_cpu;
    saved_nmi = nmi_running;
    nmi_running = 0;

    // topdown2 alone requests 9 raw events (slots + 8 core.topdown-* sub-
    // events) -- more than the 6-slot general-purpose budget, but the whole
    // family must stay one group led by "slots" regardless: only index 0 is
    // a leader.
    cgroup = raw_counter_group("topdown2", COUNTER_TOPDOWN2);
    assert(cgroup != NULL);
    assert(cgroup->ncounters == 9);
    assert(cgroup->cinfo[0].is_group_leader == 1);
    for (i = 1; i < cgroup->ncounters; i++)
        assert(cgroup->cinfo[i].is_group_leader == 0);
    free_test_cgroup(cgroup);

    // branch alone (5 counters: "instructions" + 4 br_* events) fits the
    // 6-slot budget -- one group, one leader.
    cgroup = raw_counter_group("branch", COUNTER_BRANCH);
    assert(cgroup != NULL);
    assert(cgroup->ncounters == 5);
    assert(cgroup->cinfo[0].is_group_leader == 1);
    for (i = 1; i < cgroup->ncounters; i++)
        assert(cgroup->cinfo[i].is_group_leader == 0);
    free_test_cgroup(cgroup);

    // A combined, non-topdown mask (branch|l2cache|topdown-be) requests 13
    // raw events in table order -- exceeding the 6-slot budget twice over.
    // This is exactly the "wholesale loss once combined group exceeds
    // capacity" bug: leaders must land at counts 0, 6, and 12 (three groups
    // of 6/6/1), not one 13-member group the kernel would refuse outright.
    cgroup = raw_counter_group("combined", COUNTER_BRANCH|COUNTER_L2CACHE|COUNTER_TOPDOWN_BE);
    assert(cgroup != NULL);
    assert(cgroup->ncounters == 13);
    for (i = 0; i < cgroup->ncounters; i++){
        int expect_leader = (i == 0 || i == 6 || i == 12);
        assert(cgroup->cinfo[i].is_group_leader == expect_leader);
    }
    free_test_cgroup(cgroup);

    // cache_counter_group() (PERF_TYPE_HW_CACHE dcache/icache/tlb): 9 events,
    // same 6-slot chunking -- leaders at counts 0 and 6.
    cgroup = cache_counter_group("cache", COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB);
    assert(cgroup != NULL);
    assert(cgroup->ncounters == 9);
    for (i = 0; i < cgroup->ncounters; i++){
        int expect_leader = (i == 0 || i == 6);
        assert(cgroup->cinfo[i].is_group_leader == expect_leader);
    }
    free_test_cgroup(cgroup);

    nmi_running = saved_nmi;
    cpu_info = saved_cpu_info;

    printf("PASS: Intel counter-group budget chunking\n");
}

// Regression test for INVESTIGATION.md's "RAPL/energy-pkg opened with the
// wrong scope" item (4.3 Tier 0): AMD L3's raw events are the one entry in
// amd_raw_events[] carrying device_type == PERF_TYPE_L3 -- an uncore PMU
// whose driver rejects a process-scoped perf_event_open() outright.
// raw_counter_group() must mark those (and only those) cinfo entries
// requires_system_wide so setup_counters() routes them through pid=-1
// instead of the generic per-process branch (previously that routing relied
// on an incidental numeric collision between a host's real dynamic PMU type
// and the PERF_TYPE_L3 sentinel value -- see struct counter_info's comment).
void test_raw_counter_group_system_wide_marking(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    int saved_nmi;
    struct counter_group *cgroup;
    int i;

    printf("Testing raw_counter_group() marks AMD L3 (and only L3) requires_system_wide...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_AMD;
    cpu_info = &fake_cpu;
    saved_nmi = nmi_running;
    nmi_running = 0;

    // The "l3 cache" group also pulls in amd_raw_events[]'s shared
    // "instructions" denominator entry (its .use bitmask includes
    // COUNTER_L3CACHE alongside many other groups) -- that entry's own
    // device_type is PERF_TYPE_RAW, not PERF_TYPE_L3, so it must NOT be
    // marked; only the two real l3_lookup_state.* entries should be.
    cgroup = raw_counter_group("l3 cache", COUNTER_L3CACHE);
    assert(cgroup != NULL);
    for (i = 0; i < cgroup->ncounters; i++){
        int expect_marked = !strncmp(cgroup->cinfo[i].label, "l3_lookup_state.", 16);
        assert(cgroup->cinfo[i].requires_system_wide == expect_marked);
    }
    free_test_cgroup(cgroup);

    // A non-L3 raw group (ordinary general-purpose PMU events) must NOT be
    // marked -- only the uncore/system PMU escape hatch is.
    cgroup = raw_counter_group("branch", COUNTER_BRANCH);
    assert(cgroup != NULL);
    for (i = 0; i < cgroup->ncounters; i++)
        assert(cgroup->cinfo[i].requires_system_wide == 0);
    free_test_cgroup(cgroup);

    nmi_running = saved_nmi;
    cpu_info = saved_cpu_info;

    printf("PASS: raw_counter_group() AMD L3 requires_system_wide marking\n");
}

// Regression test for a real bug found via a live Raptor Lake HX run
// (2026-07-22): cache_counter_group()'s synthetic "instructions" entry (a
// genuine PERF_TYPE_HARDWARE/PERF_COUNT_HW_INSTRUCTIONS event smuggled into
// the otherwise-PERF_TYPE_HW_CACHE cache_events[] table, used only as
// print_cache()'s "N per 1000 inst" denominator) never had its device_type
// set, so setup_counters() opened it at the whole group's fixed
// PERF_TYPE_HW_CACHE type instead -- and PERF_COUNT_HW_INSTRUCTIONS (1)
// numerically collides with L1I-read-access's own HW_CACHE encoding (also
// 1, per <linux/perf_event.h>), so it silently requested a duplicate of
// "l1i-read" instead of real instruction retirement. Vendor-agnostic (every
// vendor builds this same table via cache_counter_group()), unlike the
// grouping fix above. This pins the fix at the data level: "instructions"
// must carry device_type == PERF_TYPE_HARDWARE while every other cache_events
// entry keeps PERF_TYPE_HW_CACHE.
void test_cache_group_instructions_real_type(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct counter_group *cgroup;
    int i;
    int found_instructions = 0;

    printf("Testing cache_counter_group()'s \"instructions\" entry gets its real PERF_TYPE_HARDWARE type...\n");

    // Vendor-agnostic bug/fix, but cache_counter_group() dereferences
    // cpu_info->vendor unconditionally (for its is_group_leader chunking),
    // and the global cpu_info defaults to NULL until some test sets it --
    // any real vendor works here.
    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_AMD;
    cpu_info = &fake_cpu;

    cgroup = cache_counter_group("cache", COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB);
    assert(cgroup != NULL);
    for (i = 0; i < cgroup->ncounters; i++){
        if (!strcmp(cgroup->cinfo[i].label, "instructions")){
            assert(cgroup->cinfo[i].device_type == PERF_TYPE_HARDWARE);
            found_instructions = 1;
        } else {
            assert(cgroup->cinfo[i].device_type == PERF_TYPE_HW_CACHE);
        }
    }
    assert(found_instructions == 1);
    free_test_cgroup(cgroup);

    cpu_info = saved_cpu_info;

    printf("PASS: cache_counter_group() \"instructions\" entry real type\n");
}

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
    // New L1->L2 hierarchical columns (INVESTIGATION.md's 4.2 "Hierarchical
    // topdown schema"): this fixture has no L2 raw events (core.topdown-
    // mem-bound etc.) and Intel has no SMT-contention counter, so all 9 new
    // trailing columns should read 0.0 -- checked as one exact suffix so the
    // match is bound to position, not just presence.
    assert(strstr(contents, " 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,") != NULL);
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
        // retire, frontend, backend, speculate, confidence, sanity (6) +
        // contention_pct, retire_ucode_pct, retire_fastpath_pct,
        // frontend_latency_pct, frontend_bandwidth_pct, backend_cpu_pct,
        // backend_memory_pct, spec_branch_pct, spec_pipeline_pct (9) == 15 fields
        if (commas != 15) {
            fprintf(stderr, "FAIL: expected 15 CSV columns when topdown counters are unavailable, got %d (row: %s)\n", commas, contents);
            exit(1);
        }
    }
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: topdown confidence envelope + sanity checks\n");
}

// Regression test for a real bug found via a live Raptor Lake HX run
// (2026-07-22, INVESTIGATION.md 4.3 Tier 0 #2): print_topdown()'s VENDOR_INTEL
// branch computed L2 splits (backend_cpu, speculation_pipeline,
// frontend_bandwidth, retire_fastpath) as plain unsigned long subtraction
// instead of safe_sub(). Since a level-2 "child" counter (e.g.
// core.topdown-br-mispredict) and its level-1 "parent" (core.topdown-bad-spec)
// are independently-read/independently-multiplexed perf counters, the child
// can read slightly larger than the parent from measurement noise alone --
// observed live as spec_pipeline_pct=72407003082176.4, reproduced again here
// as a fixture after switching a branch-heavy workload's bad-spec/br-mispredict
// values: the unsigned wraparound turned a ~0% field into a
// multi-trillion-percent one instead of clamping to 0.
void test_intel_topdown_l2_underflow(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct counter_group cgroup;
    struct counter_info cinfo[7];
    char *contents;
    const char *tmp_out = "/tmp/test_wspy_intel_l2_underflow.txt";

    printf("Testing Intel L2 topdown decomposition clamps child>parent instead of wrapping...\n");

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
    cinfo[3].label = "core.topdown-be-bound";
    cinfo[3].value = 250000; cinfo[3].time_enabled = 1000000; cinfo[3].time_running = 1000000;
    // speculation (level 1) reads slightly below speculation_branches (level
    // 2) -- the exact condition that wrapped pre-fix.
    cinfo[4].label = "core.topdown-bad-spec";
    cinfo[4].value = 50000; cinfo[4].time_enabled = 1000000; cinfo[4].time_running = 1000000;
    cinfo[5].label = "core.topdown-br-mispredict";
    cinfo[5].value = 50001; cinfo[5].time_enabled = 1000000; cinfo[5].time_running = 1000000;

    memset(&cgroup, 0, sizeof(cgroup));
    cgroup.label = "topdown";
    cgroup.ncounters = 6;
    cgroup.cinfo = cinfo;
    cgroup.mask = COUNTER_TOPDOWN;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    {
        // spec_pipeline_pct (the last of the 9 trailing L2 columns) must
        // clamp to 0.0, not wrap to a near-ULONG_MAX-derived value -- confirm
        // by parsing every comma-separated field instead of pattern-matching
        // one, since a wrapped value's exact digits aren't predictable.
        char *p = contents;
        char *comma;
        while ((comma = strchr(p, ','))) {
            char field[64];
            size_t len = (size_t)(comma - p);
            if (len < sizeof(field)) {
                memcpy(field, p, len);
                field[len] = '\0';
                double v = atof(field);
                if (v > 1000.0) {
                    fprintf(stderr, "FAIL: field '%s' looks like an unclamped underflow (%.1f)\n", field, v);
                    exit(1);
                }
            }
            p = comma + 1;
        }
    }
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: Intel L2 topdown decomposition clamps child>parent instead of wrapping\n");
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
    // New L1->L2 hierarchical columns: contention_pct=0.0 (ARM has no SMT-
    // contention counter), retire_ucode_pct/retire_fastpath_pct=0.0 (ARM
    // never splits retire), frontend_latency=12.0%/bandwidth=3.0% (of
    // frontend's 150000), backend_cpu=15.0%/memory=10.0% (of backend's
    // 250000), spec_branch=4.0%/pipeline=6.0% (of speculation's 100000) --
    // all as % of slots_no_contention (== slots here, no contention
    // adjustment on ARM). Checked as one exact suffix, bound to position.
    assert(strstr(contents, " 0.0, 0.0, 0.0,12.0, 3.0,15.0,10.0, 4.0, 6.0,") != NULL);
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: ARM topdown-equivalent decomposition\n");
}

// AMD is the only vendor that currently populates SMT contention (de_no_
// dispatch_per_slot.smt_contention), which drives both the new contention_pct
// CSV column (a fraction of *raw* slots) and slots_no_contention (the
// denominator every other L1/L2 percentage in this row is a fraction of).
void test_topdown_amd_contention(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct cpu_core_info coreinfo[1];
    struct counter_group cgroup;
    struct counter_info cinfo[6];
    char *contents;
    const char *tmp_out = "/tmp/test_wspy_amd_contention.txt";

    printf("Testing AMD topdown SMT-contention decomposition...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    memset(coreinfo, 0, sizeof(coreinfo));
    fake_cpu.vendor = VENDOR_AMD;
    coreinfo[0].vendor = CORE_AMD_ZEN;  // 6 slots/cycle
    fake_cpu.coreinfo = coreinfo;
    cpu_info = &fake_cpu;

    memset(cinfo, 0, sizeof(cinfo));
    cinfo[0].label = "cpu-cycles";
    cinfo[0].value = 100000; cinfo[0].time_enabled = 1000000; cinfo[0].time_running = 1000000;
    // slots = cpu-cycles * 6 = 600000
    cinfo[1].label = "ex_ret_ops";
    cinfo[1].value = 240000; cinfo[1].time_enabled = 1000000; cinfo[1].time_running = 1000000;
    cinfo[2].label = "de_no_dispatch_per_slot.no_ops_from_frontend";
    cinfo[2].value = 120000; cinfo[2].time_enabled = 1000000; cinfo[2].time_running = 1000000;
    cinfo[3].label = "de_no_dispatch_per_slot.backend_stalls";
    cinfo[3].value = 180000; cinfo[3].time_enabled = 1000000; cinfo[3].time_running = 1000000;
    // 60000 of the raw 600000 slots lost to SMT contention -> 10.0%
    cinfo[4].label = "de_no_dispatch_per_slot.smt_contention";
    cinfo[4].value = 60000; cinfo[4].time_enabled = 1000000; cinfo[4].time_running = 1000000;
    // speculation = de_src_op_disp.all - ex_ret_ops = 300000-240000 = 60000
    cinfo[5].label = "de_src_op_disp.all";
    cinfo[5].value = 300000; cinfo[5].time_enabled = 1000000; cinfo[5].time_running = 1000000;

    memset(&cgroup, 0, sizeof(cgroup));
    cgroup.label = "topdown";
    cgroup.ncounters = 6;
    cgroup.cinfo = cinfo;
    cgroup.mask = COUNTER_TOPDOWN;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    // slots_no_contention = 600000-60000 = 540000: retiring=240000/540000=
    // 44.4%, frontend=120000/540000=22.2%, backend=180000/540000=33.3%,
    // speculate=60000/540000=11.1%, confidence=1.00 (nothing multiplexed),
    // sanity=600000/540000=111.1% (this fixture isn't a clean decomposition,
    // not the point of this test) -- then contention_pct=60000/600000=10.0%
    // (fraction of *raw* slots), and the 8 L2 columns at 0.0 (this fixture
    // supplies no L2 raw events, e.g. ex_no_retire.*/ex_ret_brn_misp).
    assert(strstr(contents, "44.4,22.2,33.3,11.1,1.00,111.1,10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,") != NULL);
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: AMD topdown SMT-contention decomposition\n");
}

// INVESTIGATION.md's 4.2 Tier 1, "Full L1->L2->L3 topdown hierarchy" item:
// print_topdown_be()'s new *_slots_pct columns read topdown.c's module-static
// shared_slots_no_contention, populated by a prior print_topdown() call on
// the *same* row (see that function's own comment on the ordering guarantee
// this depends on). Drives print_topdown() first to populate it, then
// print_topdown_be() on a separate synthetic cgroup (a genuinely different
// perf counter group in real wspy), and checks the 5 new trailing columns
// land on print_topdown()'s denominator, not print_topdown_be()'s own
// independent cpu-cycles reading.
void test_topdown_be_shared_denominator(void) {
    struct cpu_info fake_cpu;
    struct cpu_info *saved_cpu_info;
    struct counter_group l1_cgroup, be_cgroup;
    struct counter_info l1_cinfo[5], be_cinfo[6];
    char *contents;
    const char *tmp_out = "/tmp/test_wspy_topdown_be.txt";

    printf("Testing print_topdown_be() slots_pct columns share print_topdown()'s denominator...\n");

    saved_cpu_info = cpu_info;
    memset(&fake_cpu, 0, sizeof(fake_cpu));
    fake_cpu.vendor = VENDOR_INTEL;
    cpu_info = &fake_cpu;

    // First, populate shared_slots_no_contention via a real print_topdown()
    // call: slots=1,000,000 (Intel: slots_no_contention == slots, no
    // contention counter exists on Intel).
    memset(l1_cinfo, 0, sizeof(l1_cinfo));
    l1_cinfo[0].label = "slots";
    l1_cinfo[0].value = 1000000; l1_cinfo[0].time_enabled = 1000000; l1_cinfo[0].time_running = 1000000;
    l1_cinfo[1].label = "core.topdown-retiring";
    l1_cinfo[1].value = 400000; l1_cinfo[1].time_enabled = 1000000; l1_cinfo[1].time_running = 1000000;
    l1_cinfo[2].label = "core.topdown-fe-bound";
    l1_cinfo[2].value = 300000; l1_cinfo[2].time_enabled = 1000000; l1_cinfo[2].time_running = 1000000;
    l1_cinfo[3].label = "core.topdown-be-bound";
    l1_cinfo[3].value = 250000; l1_cinfo[3].time_enabled = 1000000; l1_cinfo[3].time_running = 1000000;
    l1_cinfo[4].label = "core.topdown-bad-spec";
    l1_cinfo[4].value = 50000; l1_cinfo[4].time_enabled = 1000000; l1_cinfo[4].time_running = 1000000;
    memset(&l1_cgroup, 0, sizeof(l1_cgroup));
    l1_cgroup.label = "topdown";
    l1_cgroup.ncounters = 5;
    l1_cgroup.cinfo = l1_cinfo;
    l1_cgroup.mask = COUNTER_TOPDOWN;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&l1_cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);
    remove(tmp_out);

    // Separate synthetic "topdown-be" cgroup, its own independent
    // cpu-cycles=500,000 -- deliberately different from slots_no_contention
    // above, so a test that accidentally read the wrong denominator would
    // produce visibly different percentages.
    memset(be_cinfo, 0, sizeof(be_cinfo));
    be_cinfo[0].label = "cpu-cycles";
    be_cinfo[0].value = 500000; be_cinfo[0].time_enabled = 1000000; be_cinfo[0].time_running = 1000000;
    be_cinfo[1].label = "exe_activity.bound_on_loads";
    be_cinfo[1].value = 300000; be_cinfo[1].time_enabled = 1000000; be_cinfo[1].time_running = 1000000;
    be_cinfo[2].label = "exe_activity.bound_on_stores";
    be_cinfo[2].value = 50000; be_cinfo[2].time_enabled = 1000000; be_cinfo[2].time_running = 1000000;
    be_cinfo[3].label = "memory_activity.stalls_l1d_miss";
    be_cinfo[3].value = 250000; be_cinfo[3].time_enabled = 1000000; be_cinfo[3].time_running = 1000000;
    be_cinfo[4].label = "memory_activity.stalls_l2_miss";
    be_cinfo[4].value = 150000; be_cinfo[4].time_enabled = 1000000; be_cinfo[4].time_running = 1000000;
    be_cinfo[5].label = "memory_activity.stalls_l3_miss";
    be_cinfo[5].value = 100000; be_cinfo[5].time_enabled = 1000000; be_cinfo[5].time_running = 1000000;
    memset(&be_cgroup, 0, sizeof(be_cgroup));
    be_cgroup.label = "topdown-be";
    be_cgroup.ncounters = 6;
    be_cgroup.cinfo = be_cinfo;
    be_cgroup.mask = COUNTER_TOPDOWN_BE;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown_be(&be_cgroup, PRINT_CSV, COUNTER_TOPDOWN_BE);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    // l1_bound=300000-250000=50000, l2_bound=250000-150000=100000,
    // l3_bound=150000-100000=50000, dram_bound=100000, store_bound=50000.
    // Original columns: % of this group's own cpu-cycles (500000) ->
    // 10.0,20.0,10.0,20.0,10.0. New columns: % of the *shared*
    // slots_no_contention (1,000,000) from print_topdown() above ->
    // 5.0,10.0,5.0,10.0,5.0 -- half the original percentages, since the
    // shared denominator is exactly 2x this group's own cpu-cycles here.
    assert(strstr(contents, "10.0,20.0,10.0,20.0,10.0, 5.0,10.0, 5.0,10.0, 5.0,") != NULL);
    free(contents);
    remove(tmp_out);

    // Now force shared_slots_no_contention_valid back to 0: a print_topdown()
    // call with an unrecognized vendor hits its "default: return;" branch,
    // which runs *after* the reset-to-invalid at the top of that function
    // but *before* the set-to-valid after its vendor switch -- exactly the
    // "topdown counters weren't available/requested this row" case
    // print_topdown_be()'s own new columns need to degrade gracefully for
    // (e.g. --topdown-backend used without --topdown/--topdown2).
    fake_cpu.vendor = VENDOR_UNKNOWN;
    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown(&l1_cgroup, PRINT_CSV, COUNTER_TOPDOWN);
    fclose(outfile);
    remove(tmp_out);
    fake_cpu.vendor = VENDOR_INTEL;

    outfile = fopen(tmp_out, "w");
    if (!outfile) { fprintf(stderr, "FAIL: could not open temp file\n"); exit(1); }
    print_topdown_be(&be_cgroup, PRINT_CSV, COUNTER_TOPDOWN_BE);
    fclose(outfile);

    contents = slurp_file(tmp_out);
    assert(contents != NULL);
    // Original cpu-cycles-based columns are unaffected by the shared state
    // at all; the 5 new slots_pct columns must degrade to 0.0 rather than
    // reading a stale value from the first print_topdown_be() call above.
    assert(strstr(contents, "10.0,20.0,10.0,20.0,10.0, 0.0, 0.0, 0.0, 0.0, 0.0,") != NULL);
    free(contents);

    remove(tmp_out);
    outfile = NULL;
    cpu_info = saved_cpu_info;

    printf("PASS: print_topdown_be() slots_pct columns share print_topdown()'s denominator\n");
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

// INVESTIGATION.md's "What shipped in 4.1", the multiplex-scaling
// correctness fix: a counter multiplexed off the PMU for
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
    test_power_core_skip_not_attempted();
    test_preflight();
    test_multipass();
    test_multipass_raw_event_parsing();
    test_provenance();
    test_topdown_confidence();
    test_intel_topdown_l2_underflow();
    test_arm_topdown_equivalence();
    test_topdown_amd_contention();
    test_topdown_be_shared_denominator();
    test_intel_counter_grouping();
    test_raw_counter_group_system_wide_marking();
    test_cache_group_instructions_real_type();
    test_arm_pmu_report();
    test_read_counters_multiplex_scaling();
    return 0;
}
