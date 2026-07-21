/*
 * archetype.c - wspy-archetype: classifies a wspy run along a small set of
 * workload "archetype" axes -- INVESTIGATION.md's 4.2 Tier 1 "Archetype
 * scorecard" item, the second (and last) of the characterization-track
 * prerequisites started by store.c's run_features ("Feature normalization
 * prerequisites", shipped in PR #121).
 *
 * Reads store.c's runs/run_features tables directly via SQL, read-only --
 * this tool never writes, same convention as summary.c/core_report.c. No
 * taxonomy/threshold/confidence-formula spec existed anywhere in this repo
 * before this file (confirmed by search) -- every classification rule
 * below is a from-scratch v1 design, deliberately simple and documented as
 * a starting point, the same style phase.c/preflight.c already use for
 * their own heuristics.
 *
 * Four axes, confirmed with the user as the chosen design (the backlog's
 * "confidence + top-2 alternatives" language doesn't by itself say whether
 * it means one composite label or per-axis classifications):
 *   - resource_dominance: the headline axis, a ranked 4-way score over
 *     topdown L1 (retire/frontend/backend/speculate) -- the one axis with
 *     a natural percentage to rank alternatives by, so "confidence" and
 *     "top-2 alternatives" are scoped to it specifically.
 *   - parallelism_shape, control_flow_style, runtime_stability: simpler
 *     threshold-based supporting tags (each independently "unknown" when
 *     their source run_features value wasn't collected), whose combined
 *     availability feeds overall confidence but which don't themselves
 *     have "alternatives" to rank.
 *
 * Real prior art grounded this design further: a 2024 clustering analysis
 * (github.com/mvermeulen, ~240 Phoronix tests + 23 SPEC CPU2017
 * benchmarks, k-means/Lloyd's algorithm into 30 clusters) used exactly
 * retire/frontend/backend/speculation as its core clustering metrics --
 * directly validating the resource_dominance approach (its own cluster #1,
 * "backend bound... running on all cores", is exactly the kind of label
 * this axis produces) -- and separately used "on_cpu" (cores actively
 * used) as a first-class clustering dimension distinct from load balance,
 * which parallelism_proxy (run_features) didn't capture; active_core_count
 * (run_features, added alongside this file) is this codebase's own proxy
 * for that idea, not a reproduction of that work's undisclosed exact
 * formula. That same post's own restraint -- explicit cluster boundaries
 * were never reduced to hard thresholds, only relative distances -- is
 * exactly the caution summary.c's own header comment states about not
 * inventing a differentiated-threshold table without real data to justify
 * it; the thresholds below are offered in that same spirit, a starting
 * point rather than a validated boundary.
 *
 * Extensibility (a new axis, e.g. floating-point density once --float has
 * real cross-workload validation): a new *simple* (single-value,
 * threshold-based) axis is one more SIMPLE_AXES entry plus a
 * classify_simple_axis() call site reading the new run_features value --
 * no changes needed to compute_overall_confidence() or either output mode,
 * both of which already loop over however many simple axes exist. A new
 * *ranked* axis (like resource_dominance) needs its own small candidate
 * table/ranking function, following the same shape this file already
 * establishes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sqlite3.h>

/* run_features (store.c) didn't exist before store.c's schema version 4
 * (MIGRATION_V3_TO_V4) -- a database older than that has nothing for this
 * tool to score. */
#define ARCHETYPE_MIN_SCHEMA_VERSION 4

/* Resource-dominance confidence margin thresholds (percentage points
 * between the primary and runner-up topdown L1 category) and the
 * "known simple axes" floor each confidence level also requires --
 * deliberately simple v1 starting points, not derived from a formal study
 * (see file header). */
#define DOMINANCE_HIGH_MARGIN   20.0
#define DOMINANCE_MEDIUM_MARGIN 10.0

struct threshold_rule { double max_value; const char *label; }; /* ascending; last entry is the catch-all */

static const struct threshold_rule PARALLELISM_RULES[] = {
  { 0.15, "balanced-parallel" },
  { HUGE_VAL, "imbalanced" },
};
static const struct threshold_rule CONTROL_FLOW_RULES[] = {
  { 2.0, "straight-line" },
  { HUGE_VAL, "branch-heavy" },
};
static const struct threshold_rule STABILITY_RULES[] = {
  { 0.4, "erratic" },
  { 0.8, "phased" },
  { HUGE_VAL, "steady" },
};

enum simple_axis_id { AXIS_PARALLELISM_SHAPE, AXIS_CONTROL_FLOW_STYLE, AXIS_RUNTIME_STABILITY, NUM_SIMPLE_AXES };
static const char *SIMPLE_AXIS_KEYS[NUM_SIMPLE_AXES] = {
  "parallelism_shape", "control_flow_style", "runtime_stability"
};

struct classified_axis { char label[24]; int available; };

/* Generic threshold classifier shared by every simple axis: walk `rules`
 * (ascending max_value, last entry is the unconditional catch-all) and
 * return the first one `value` doesn't exceed. `available` false (the
 * source run_features row was coverage='unavailable') always yields
 * "unknown" regardless of value. */
static void classify_simple_axis(double value,int available,
                                  const struct threshold_rule *rules,int nrules,
                                  struct classified_axis *out){
  int i;
  out->available = available;
  if (!available){ snprintf(out->label,sizeof(out->label),"unknown"); return; }
  for (i = 0; i < nrules; i++){
    if (value <= rules[i].max_value || i == nrules - 1){
      snprintf(out->label,sizeof(out->label),"%s",rules[i].label);
      return;
    }
  }
}

struct dominance_result {
  int available;
  char primary_label[24];
  double primary_pct;
  int has_alternative;
  char alternative_label[24];
  double alternative_pct;
};

/* The one ranked/multi-candidate axis: topdown L1's four mutually-
 * exclusive slot-destination percentages, renamed into plain-English
 * resource-dominance categories. Ranks only the candidates that were
 * actually measured (coverage='measured' in run_features) -- all four
 * normally co-occur (print_topdown() emits them together), so partial
 * availability is an edge case, not the common path. */
static void classify_resource_dominance(double retire_pct,int have_retire,
                                         double frontend_pct,int have_frontend,
                                         double backend_pct,int have_backend,
                                         double speculate_pct,int have_speculate,
                                         struct dominance_result *out){
  struct { const char *label; double value; int available; } cands[4] = {
    { "compute-bound",      retire_pct,    have_retire },
    { "frontend-bound",     frontend_pct,  have_frontend },
    { "memory-bound",       backend_pct,   have_backend },
    { "speculation-bound",  speculate_pct, have_speculate },
  };
  int i,best = -1,second = -1;

  memset(out,0,sizeof(*out));
  for (i = 0; i < 4; i++){
    if (!cands[i].available) continue;
    if (best == -1 || cands[i].value > cands[best].value){ second = best; best = i; }
    else if (second == -1 || cands[i].value > cands[second].value) second = i;
  }
  if (best == -1) return; /* no topdown L1 data at all this run */

  out->available = 1;
  snprintf(out->primary_label,sizeof(out->primary_label),"%s",cands[best].label);
  out->primary_pct = cands[best].value;
  if (second != -1){
    out->has_alternative = 1;
    snprintf(out->alternative_label,sizeof(out->alternative_label),"%s",cands[second].label);
    out->alternative_pct = cands[second].value;
  }
}

struct confidence_result {
  char level[24];        /* "high"/"medium"/"low"/"insufficient-data" (19 bytes incl. NUL) */
  char reasons[160];      /* comma-joined, fixed order, empty for "high" with nothing to flag */
};

/* Mirrors summary.c's compute_verdict() shape: deterministic booleans, a
 * fixed-order reason list, appended only when they apply -- here a
 * confidence *level* rather than a pass/fail verdict, since "confidence"
 * isn't binary the way repeatability is. resource_dominance being
 * unavailable is its own terminal case (nothing to score at all); otherwise
 * confidence is driven by how decisive the dominance margin is and how
 * many of the supporting simple axes had data. */
static void compute_overall_confidence(const struct dominance_result *dom,
                                        const struct classified_axis *simple,int nsimple,
                                        struct confidence_result *out){
  double margin;
  int known = 0,i;
  size_t used;

  if (!dom->available){
    snprintf(out->level,sizeof(out->level),"insufficient-data");
    snprintf(out->reasons,sizeof(out->reasons),"no-topdown-data");
    return;
  }

  margin = dom->has_alternative ? (dom->primary_pct - dom->alternative_pct) : dom->primary_pct;
  for (i = 0; i < nsimple; i++) if (simple[i].available) known++;

  if (margin >= DOMINANCE_HIGH_MARGIN && known >= 2)
    snprintf(out->level,sizeof(out->level),"high");
  else if (margin >= DOMINANCE_MEDIUM_MARGIN && known >= 1)
    snprintf(out->level,sizeof(out->level),"medium");
  else
    snprintf(out->level,sizeof(out->level),"low");

  out->reasons[0] = '\0';
  used = 0;
  if (margin < DOMINANCE_HIGH_MARGIN)
    used += (size_t)snprintf(out->reasons+used,sizeof(out->reasons)-used,"narrow-margin");
  for (i = 0; i < nsimple; i++){
    if (simple[i].available) continue;
    used += (size_t)snprintf(out->reasons+used,sizeof(out->reasons)-used,"%smissing-%s-data",
                              used ? "," : "",SIMPLE_AXIS_KEYS[i]);
  }
}

/* One run's worth of run_features values, assembled by streaming
 * contiguous (run_id-ordered) rows out of the store -- see score_runs()/
 * trace_run_archetype(). Only the features this tool's axes actually need
 * are named fields; run_features carries several more (ipc_mean, cache/
 * TLB miss rates, fault_rate, ctxswitch_rate) not yet used by any axis. */
struct run_snapshot {
  sqlite3_int64 run_id;
  char hostname[256];
  char run_id_text[128];
  char command[512];
  double retire_pct,frontend_pct,backend_pct,speculate_pct;
  int have_retire,have_frontend,have_backend,have_speculate;
  double parallelism_proxy;      int have_parallelism;
  double active_core_count;      int have_active_core_count;
  double branch_mispredict_pct;  int have_branch;
  double phase_stability;        int have_phase;
  double smt_contention_pct;     int have_smt_contention;
};

static void run_snapshot_reset(struct run_snapshot *snap,sqlite3_int64 run_id,
                                const char *hostname,const char *run_id_text,const char *command){
  memset(snap,0,sizeof(*snap));
  snap->run_id = run_id;
  snprintf(snap->hostname,sizeof(snap->hostname),"%s",hostname ? hostname : "?");
  snprintf(snap->run_id_text,sizeof(snap->run_id_text),"%s",run_id_text ? run_id_text : "?");
  snprintf(snap->command,sizeof(snap->command),"%s",command ? command : "");
}

/* Applies one run_features (feature_name,value,coverage) row to the
 * snapshot being assembled. Feature names not recognized here (every other
 * run_features entry -- ipc_mean, cache/TLB rates, fault_rate,
 * ctxswitch_rate, and any future addition -- see file header's
 * "Extensibility" note) are silently ignored, not an error: this tool only
 * scores what its axes currently use. */
static void run_snapshot_apply_feature(struct run_snapshot *snap,const char *feature_name,
                                        double value,int measured){
  if (!strcmp(feature_name,"retire_pct")){ snap->retire_pct = value; snap->have_retire = measured; }
  else if (!strcmp(feature_name,"frontend_pct")){ snap->frontend_pct = value; snap->have_frontend = measured; }
  else if (!strcmp(feature_name,"backend_pct")){ snap->backend_pct = value; snap->have_backend = measured; }
  else if (!strcmp(feature_name,"speculate_pct")){ snap->speculate_pct = value; snap->have_speculate = measured; }
  else if (!strcmp(feature_name,"parallelism_proxy")){ snap->parallelism_proxy = value; snap->have_parallelism = measured; }
  else if (!strcmp(feature_name,"active_core_count")){ snap->active_core_count = value; snap->have_active_core_count = measured; }
  else if (!strcmp(feature_name,"branch_mispredict_pct")){ snap->branch_mispredict_pct = value; snap->have_branch = measured; }
  else if (!strcmp(feature_name,"phase_stability")){ snap->phase_stability = value; snap->have_phase = measured; }
  else if (!strcmp(feature_name,"smt_contention_pct")){ snap->smt_contention_pct = value; snap->have_smt_contention = measured; }
}

/* The full scorecard for one already-assembled snapshot -- shared by both
 * output modes so bulk and --run can never compute different answers for
 * the same run. */
struct scorecard {
  struct dominance_result dominance;
  struct classified_axis simple[NUM_SIMPLE_AXES];
  struct confidence_result confidence;
};

static void score_snapshot(const struct run_snapshot *snap,struct scorecard *out){
  classify_resource_dominance(snap->retire_pct,snap->have_retire,
                               snap->frontend_pct,snap->have_frontend,
                               snap->backend_pct,snap->have_backend,
                               snap->speculate_pct,snap->have_speculate,
                               &out->dominance);
  classify_simple_axis(snap->parallelism_proxy,snap->have_parallelism,
                        PARALLELISM_RULES,2,&out->simple[AXIS_PARALLELISM_SHAPE]);
  classify_simple_axis(snap->branch_mispredict_pct,snap->have_branch,
                        CONTROL_FLOW_RULES,2,&out->simple[AXIS_CONTROL_FLOW_STYLE]);
  classify_simple_axis(snap->phase_stability,snap->have_phase,
                        STABILITY_RULES,3,&out->simple[AXIS_RUNTIME_STABILITY]);
  compute_overall_confidence(&out->dominance,out->simple,NUM_SIMPLE_AXES,&out->confidence);
}

static sqlite3 *open_archetype_db(const char *path){
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt;
  int user_version = 0;

  if (sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: unable to open database %s: %s\n",path,sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db,30000);

  if (sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK){
    if (sqlite3_step(stmt) == SQLITE_ROW) user_version = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
  }
  if (user_version < ARCHETYPE_MIN_SCHEMA_VERSION){
    fprintf(stderr,"wspy-archetype: %s: schema version %d predates run_features (needs >= %d) "
                    "-- re-ingest with a current wspy-store build\n",
            path,user_version,ARCHETYPE_MIN_SCHEMA_VERSION);
    sqlite3_close(db);
    return NULL;
  }
  return db;
}

static void print_csv_field(FILE *out,const char *s){
  int needs_quote = (strchr(s,',') != NULL) || (strchr(s,'"') != NULL);
  const char *p;

  if (!needs_quote){ fputs(s,out); return; }
  fputc('"',out);
  for (p = s; *p; p++){
    if (*p == '"') fputc('"',out);
    fputc(*p,out);
  }
  fputc('"',out);
}

static void print_scorecard_row(FILE *out,const struct run_snapshot *snap,
                                 const struct scorecard *sc,int csvflag){
  const char *dom_label = sc->dominance.available ? sc->dominance.primary_label : "unknown";
  const char *alt_label = sc->dominance.has_alternative ? sc->dominance.alternative_label : "";

  if (csvflag){
    print_csv_field(out,snap->hostname); fputc(',',out);
    print_csv_field(out,snap->run_id_text); fputc(',',out);
    print_csv_field(out,snap->command); fputc(',',out);
    print_csv_field(out,dom_label); fputc(',',out);
    if (sc->dominance.available) fprintf(out,"%.6g,",sc->dominance.primary_pct); else fputc(',',out);
    print_csv_field(out,alt_label); fputc(',',out);
    if (sc->dominance.has_alternative) fprintf(out,"%.6g,",sc->dominance.alternative_pct); else fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_PARALLELISM_SHAPE].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_CONTROL_FLOW_STYLE].label); fputc(',',out);
    print_csv_field(out,sc->simple[AXIS_RUNTIME_STABILITY].label); fputc(',',out);
    print_csv_field(out,sc->confidence.level); fputc(',',out);
    print_csv_field(out,sc->confidence.reasons);
    fputc('\n',out);
  } else {
    fprintf(out,"%-24.24s %-20.20s %-28.28s %-18s %-18s %-16s %-14s %-9s %-8s  %s\n",
            snap->hostname,snap->run_id_text,snap->command,
            dom_label,alt_label,
            sc->simple[AXIS_PARALLELISM_SHAPE].label,
            sc->simple[AXIS_CONTROL_FLOW_STYLE].label,
            sc->simple[AXIS_RUNTIME_STABILITY].label,
            sc->confidence.level,sc->confidence.reasons);
  }
}

/* Streams (run,feature_name,value,coverage) rows ordered by run id, so one
 * run's ~18 run_features rows are always contiguous (same convention
 * summary.c's summarize() uses for its own (group,metric) buckets) --
 * assemble a snapshot per run id and score it once the id changes. INNER
 * JOIN deliberately, not LEFT: a run with zero run_features rows (e.g.
 * --no-feature-extract was used and it's never been re-ingested since)
 * genuinely has nothing to score, which is different information than "all
 * axes came back unknown" (features computed, all unavailable) -- showing
 * it here would conflate the two. */
static int score_runs(sqlite3 *db,const char *command_filter,const char *hostname_filter,
                       int csvflag,FILE *out){
  const char *sql =
    "SELECT r.id, r.hostname, r.run_id, r.command, rf.feature_name, rf.value, rf.coverage "
    "FROM runs r JOIN run_features rf ON rf.run_id = r.id "
    "WHERE (?1 = '' OR r.command LIKE '%' || ?1 || '%') "
    "AND (?2 = '' OR r.hostname = ?2) "
    "ORDER BY r.id, rf.feature_name;";
  sqlite3_stmt *stmt;
  struct run_snapshot snap;
  int have_snap = 0,rows = 0;

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt,1,command_filter,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,hostname_filter,-1,SQLITE_TRANSIENT);

  if (csvflag)
    fprintf(out,"hostname,run_id,command,resource_dominance,resource_dominance_pct,"
                "alternative,alternative_pct,parallelism_shape,control_flow_style,"
                "runtime_stability,confidence,confidence_reasons\n");
  else
    fprintf(out,"%-24.24s %-20.20s %-28.28s %-18s %-18s %-16s %-14s %-9s %-8s  %s\n",
            "hostname","run_id","command","resource_dominance","alternative",
            "parallelism","control_flow","stability","conf.","reasons");

  while (sqlite3_step(stmt) == SQLITE_ROW){
    sqlite3_int64 run_id = sqlite3_column_int64(stmt,0);
    const unsigned char *hostname = sqlite3_column_text(stmt,1);
    const unsigned char *run_id_text = sqlite3_column_text(stmt,2);
    const unsigned char *command = sqlite3_column_text(stmt,3);
    const unsigned char *feature_name = sqlite3_column_text(stmt,4);
    double value = sqlite3_column_double(stmt,5);
    const unsigned char *coverage = sqlite3_column_text(stmt,6);
    int measured = coverage && !strcmp((const char *)coverage,"measured");

    if (!have_snap || snap.run_id != run_id){
      if (have_snap){ struct scorecard sc; score_snapshot(&snap,&sc); print_scorecard_row(out,&snap,&sc,csvflag); rows++; }
      run_snapshot_reset(&snap,run_id,(const char *)hostname,(const char *)run_id_text,(const char *)command);
      have_snap = 1;
    }
    if (feature_name) run_snapshot_apply_feature(&snap,(const char *)feature_name,value,measured);
  }
  if (have_snap){ struct scorecard sc; score_snapshot(&snap,&sc); print_scorecard_row(out,&snap,&sc,csvflag); rows++; }

  sqlite3_finalize(stmt);
  return rows;
}

static void print_trace_field(FILE *out,const char *key,const char *value){
  const char *p;
  fprintf(out,"%s=",key);
  for (p = value; *p; p++) fputc((*p == '\n' || *p == '\r') ? ' ' : *p,out);
  fputc('\n',out);
}

/* --run <hostname>:<run_id>: detailed single-run scorecard, key=value lines
 * like summary.c's --trace. Existence is checked against `runs` directly
 * first (not inferred from the JOIN below coming back empty), so a run
 * that exists but has zero run_features rows is correctly reported as
 * "found, nothing to score" rather than "not found" -- see score_runs()'s
 * own comment on the same distinction. Returns 0 if found (regardless of
 * how much could be scored), 1 if the (hostname,run_id) isn't in the store
 * at all, matching summary.c's --trace exit-code convention (data-missing,
 * not a usage error). */
static int trace_run_archetype(sqlite3 *db,const char *hostname,const char *run_id_text,FILE *out){
  sqlite3_stmt *stmt;
  sqlite3_int64 run_id;
  char command[512] = "";
  struct run_snapshot snap;
  struct scorecard sc;
  char pct_buf[32];

  if (sqlite3_prepare_v2(db,"SELECT id,command FROM runs WHERE hostname=? AND run_id=?;",
                         -1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-archetype: query failed: %s\n",sqlite3_errmsg(db));
    return 1;
  }
  sqlite3_bind_text(stmt,1,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,run_id_text,-1,SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW){
    sqlite3_finalize(stmt);
    fprintf(stderr,"wspy-archetype: no run found for %s:%s\n",hostname,run_id_text);
    return 1;
  }
  run_id = sqlite3_column_int64(stmt,0);
  {
    const unsigned char *c = sqlite3_column_text(stmt,1);
    snprintf(command,sizeof(command),"%s",c ? (const char *)c : "");
  }
  sqlite3_finalize(stmt);

  run_snapshot_reset(&snap,run_id,hostname,run_id_text,command);

  if (sqlite3_prepare_v2(db,"SELECT feature_name,value,coverage FROM run_features WHERE run_id=?;",
                         -1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_int64(stmt,1,run_id);
    while (sqlite3_step(stmt) == SQLITE_ROW){
      const unsigned char *feature_name = sqlite3_column_text(stmt,0);
      double value = sqlite3_column_double(stmt,1);
      const unsigned char *coverage = sqlite3_column_text(stmt,2);
      int measured = coverage && !strcmp((const char *)coverage,"measured");
      if (feature_name) run_snapshot_apply_feature(&snap,(const char *)feature_name,value,measured);
    }
    sqlite3_finalize(stmt);
  }

  score_snapshot(&snap,&sc);

  print_trace_field(out,"hostname",hostname);
  print_trace_field(out,"run_id",run_id_text);
  print_trace_field(out,"command",command);
  print_trace_field(out,"resource_dominance",sc.dominance.available ? sc.dominance.primary_label : "unknown");
  if (sc.dominance.available){
    snprintf(pct_buf,sizeof(pct_buf),"%.2f",sc.dominance.primary_pct);
    print_trace_field(out,"resource_dominance_pct",pct_buf);
  }
  if (sc.dominance.has_alternative){
    print_trace_field(out,"alternative",sc.dominance.alternative_label);
    snprintf(pct_buf,sizeof(pct_buf),"%.2f",sc.dominance.alternative_pct);
    print_trace_field(out,"alternative_pct",pct_buf);
  }
  print_trace_field(out,"parallelism_shape",sc.simple[AXIS_PARALLELISM_SHAPE].label);
  print_trace_field(out,"control_flow_style",sc.simple[AXIS_CONTROL_FLOW_STYLE].label);
  print_trace_field(out,"runtime_stability",sc.simple[AXIS_RUNTIME_STABILITY].label);
  print_trace_field(out,"confidence",sc.confidence.level);
  print_trace_field(out,"confidence_reasons",sc.confidence.reasons);
  return 0;
}

#ifndef TEST_ARCHETYPE
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s --db <path> [options]\n"
    "\n"
    "Classifies wspy runs recorded in a normalized store (wspy-store --db\n"
    "<path>) along four workload \"archetype\" axes, scored from store.c's\n"
    "run_features -- INVESTIGATION.md's 4.2 \"Archetype scorecard\" item.\n"
    "\n"
    "resource_dominance is the headline axis (compute-bound/frontend-bound/\n"
    "memory-bound/speculation-bound, ranked from topdown L1 percentages),\n"
    "with a top-2 alternative and an overall confidence level (high/medium/\n"
    "low/insufficient-data, with reasons). parallelism_shape/control_flow_style/\n"
    "runtime_stability are simpler supporting tags, \"unknown\" when their\n"
    "source run_features value wasn't collected (needs --per-core/--branch/\n"
    "--interval respectively).\n"
    "\n"
    "Options:\n"
    "  --db <path>          normalized store database (required)\n"
    "  --command <substr>   only include runs whose command matches this substring\n"
    "  --hostname <name>    only include runs from this host\n"
    "  --csv                machine-readable CSV output instead of the human table\n"
    "  -h, --help           show this help\n"
    "\n"
    "  --run <host>:<run_id>  standalone mode: detailed scorecard for one run\n"
    "                       (as named by a run-index record), ignoring every\n"
    "                       other option above except --db. Prints key=value\n"
    "                       lines.\n"
    "\n"
    "Only runs that have gone through feature extraction (default on in\n"
    "wspy-store, opt out via --no-feature-extract) are scored -- a run with\n"
    "no run_features rows at all has nothing to classify, distinct from one\n"
    "whose axes all came back \"unknown\"/\"unavailable\".\n"
    "\n"
    "Exit status: 0 normally; 1 with --run if no such run is recorded; 2 on\n"
    "a usage error or if the database could not be opened.\n",
    prog);
}

int main(int argc,char **argv){
  int opt;
  const char *db_path = NULL;
  const char *command_filter = "";
  const char *hostname_filter = "";
  const char *run_key = NULL;
  int csvflag = 0;
  sqlite3 *db;
  int rc;

  static struct option long_options[] = {
    { "db",        required_argument, 0, 'd' },
    { "command",   required_argument, 0, 'c' },
    { "hostname",  required_argument, 0, 'H' },
    { "csv",       no_argument,       0, 'C' },
    { "run",       required_argument, 0, 'r' },
    { "help",      no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  while ((opt = getopt_long(argc,argv,"h",long_options,NULL)) != -1){
    switch (opt){
    case 'd': db_path = optarg; break;
    case 'c': command_filter = optarg; break;
    case 'H': hostname_filter = optarg; break;
    case 'C': csvflag = 1; break;
    case 'r': run_key = optarg; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if (!db_path){
    fprintf(stderr,"wspy-archetype: --db <path> is required\n\n");
    usage(argv[0]);
    return 2;
  }

  if (run_key){
    const char *colon = strchr(run_key,':');
    char hostname_buf[256];

    if (!colon || colon == run_key || colon[1] == '\0'){
      fprintf(stderr,"wspy-archetype: --run expects <hostname>:<run_id>, got '%s'\n\n",run_key);
      usage(argv[0]);
      return 2;
    }
    snprintf(hostname_buf,sizeof(hostname_buf),"%.*s",(int)(colon - run_key),run_key);

    db = open_archetype_db(db_path);
    if (!db) return 2;
    rc = trace_run_archetype(db,hostname_buf,colon + 1,stdout);
    sqlite3_close(db);
    return rc;
  }

  db = open_archetype_db(db_path);
  if (!db) return 2;

  rc = score_runs(db,command_filter,hostname_filter,csvflag,stdout);
  sqlite3_close(db);
  return (rc < 0) ? 2 : 0;
}
#endif /* TEST_ARCHETYPE */
