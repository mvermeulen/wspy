#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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

    printf("PASS: wspy parse_options\n");
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

    if (write_manifest(manifest_out, &minfo) != 0) {
        fprintf(stderr, "FAIL: write_manifest returned an error\n");
        exit(1);
    }

    contents = slurp_file(manifest_out);
    if (!contents) {
        fprintf(stderr, "FAIL: could not read back manifest file\n");
        exit(1);
    }

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

int main(int argc, char **argv) {
    printf("Running Wspy Test Suite...\n");
    test_wspy_parse_options();
    test_write_manifest();
    return 0;
}
