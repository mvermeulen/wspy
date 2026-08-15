/*
 * proctree_viewer.js -- item 3's interactive tree viewer + run-to-run diff
 * view (INVESTIGATION.md 4.2 Tier 1). Loaded only by /tree-viewer/... and
 * /tree-diff (via window.PTV_CONFIG = {mode, jsonUrl} set inline by
 * server.py's render_tree_viewer()/render_tree_diff()) rather than folded
 * into the shared app.js, so every other page doesn't pay for this page-
 * specific JS.
 *
 * Fetches the tree/diff JSON from jsonUrl (server.py's /api/tree-json or
 * /api/tree-diff-json, which just shell out to `proctree --json`/
 * `proctree --diff --json`) and renders it entirely client-side: a
 * collapsible tree, a search/filter box, and per-column toggle checkboxes
 * for whichever optional annotations (futex/io/vmsize/etc.) this run
 * actually collected -- auto-detected by scanning the fetched tree, not
 * hardcoded, since a column never collected this run simply shouldn't
 * appear as an option.
 *
 * Note: the whole tree is rendered into the DOM up front (children just
 * toggled via display:none when collapsed), not lazily constructed on
 * first expand -- fine up to the thousands-of-processes scale this
 * codebase's own real runs produce, but a future improvement for truly
 * enormous trees would be deferring child-DOM construction until a node is
 * actually expanded.
 *
 * Single-tree mode also has a second rendering of the exact same fetched
 * data: a "Timeline" view (state.viewMode, renderTimelineView() and
 * everything below it) alongside the default "Tree" hierarchy view. Unlike
 * web/static/timeline_viewer.js's combined tree+timeline page, this mode has
 * no --interval dependency at all -- the x-axis is each process's own
 * start/finish (already in every --tree run's JSON, no other flag needed),
 * not a periodic sample tick, so it works on any --tree run whatsoever, not
 * just the narrow same-invocation --tree+--interval combination that page
 * requires. Where a --target-matched process resolves to the topdown
 * 4-category breakdown (computeDisplayCounters() below), its bar is colored
 * by its own Retiring % (good/warn/bad, this app's existing verdict-bucket
 * convention) instead of the plain comm-based categorical color every other
 * bar gets -- promoting --target's per-process counters from a tooltip
 * afterthought (all this view's data already got in the tree hierarchy view)
 * to the actual visual encoding, since a lifetime-total is exactly as
 * meaningful as a single color for the process's whole bar, unlike a real
 * time series it can't offer without --target being extended to sample on
 * ticks -- see INVESTIGATION.md for that still-open follow-on.
 */
(function () {
  "use strict";

  var cfg = window.PTV_CONFIG || {};
  var rootEl = document.getElementById("ptv-root");
  var controlsEl = document.getElementById("ptv-controls");

  if (!cfg.jsonUrl || !rootEl) {
    return;
  }

  // Optional per-node columns: "always" ones are present in every --tree
  // record regardless of flags (proctree.c's -M/-N/-P equivalent data);
  // the rest only ever have nonzero values if the matching wspy --tree-*
  // flag was used this run, so they're only offered once detect() finds
  // at least one nonzero occurrence anywhere in the fetched tree.
  var COLUMN_DEFS = [
    { key: "cmdline", label: "cmdline", always: true },
    { key: "ppid", label: "ppid", always: true },
    { key: "num_threads", label: "threads", always: true },
    { key: "vsize_kb", label: "vsize(kb)", always: true },
    { key: "rss_kb", label: "rss(kb)", always: true },
    { key: "futex_wait_seconds", label: "futex_wait", pairKey: "futex_wait_count" },
    { key: "io_read_wait_seconds", label: "io_read_wait", pairKey: "io_read_wait_count" },
    { key: "io_write_wait_seconds", label: "io_write_wait", pairKey: "io_write_wait_count" },
    { key: "io_rchar", label: "io_rchar" },
    { key: "io_wchar", label: "io_wchar" },
    { key: "io_read_bytes", label: "io_read_bytes" },
    { key: "io_write_bytes", label: "io_write_bytes" },
    { key: "sched_rundelay_seconds", label: "run_delay", pairKey: "sched_nr_timeslices" },
    { key: "vm_hwm_kb", label: "vm_hwm(kb)" },
    { key: "rss_anon_kb", label: "rss_anon(kb)" },
    { key: "rss_file_kb", label: "rss_file(kb)" },
    { key: "rss_shmem_kb", label: "rss_shmem(kb)" },
    { key: "vm_swap_kb", label: "vm_swap(kb)" },
    { key: "connect_seconds", label: "connect", pairKey: "connect_count" },
    { key: "nanosleep_seconds", label: "nanosleep", pairKey: "nanosleep_count" },
    { key: "wait_seconds", label: "wait", pairKey: "wait_count" },
    { key: "poll_seconds", label: "poll", pairKey: "poll_count" }
  ];

  // Auto-expand threshold for renderNode(): a subtree whose cumulative
  // utime+stime is at least this fraction of the whole run's total is
  // expanded by default (see EXPAND_TIME_SHARE's use in renderNode()).
  var EXPAND_TIME_SHARE = 0.05;

  var state = { search: "", columns: {}, showDeltas: false, minSharePercent: 0, viewMode: "tree" };

  // Single-tree mode only: sum of every node's utime+stime in the fetched
  // tree, set once by computeCumulative() before first render. Used as the
  // 100% baseline for each node's cumulative-time share (row display, the
  // auto-expand threshold, and the "hide branches under N%" filter).
  var totalCumSeconds = 0;

  // Timeline view mode only (see this file's own top-of-file comment):
  // [min,max] wall-clock extent across every node's start/finish in the
  // fetched tree, set once by renderSingle() before first render.
  // timelineFullDomain never changes after that; timelineXDomain is the
  // current, possibly zoomed, window -- same fullDomain/xDomain split
  // web/static/timeline_viewer.js uses for the identical reason (a "reset
  // zoom" button needs to remember the un-zoomed extent).
  var timelineFullDomain = null;
  var timelineXDomain = null;
  var totalWallSpan = 0;
  // Last controls-rendering inputs, so the view-mode toggle (which lives
  // inside renderControlsSingle()'s own output) can re-render the controls
  // panel on click without renderSingle() needing to re-run.
  var lastAvailableColumns = [];
  var lastSingleData = null;

  // --target=comm=<name>[,cmdline=<substr>] per-node columns (item 10,
  // INVESTIGATION.md): the distinct "group.label" keys found anywhere in the
  // fetched tree's own per-node target_counters arrays (as opposed to
  // collectTargetCounterKeys() below, which scans the top-level comm-rollup
  // rows) -- set once by renderSingle() before first render, toggled the
  // same way COLUMN_DEFS entries are (state.columns[key], a checkbox per
  // entry). Empty for older JSON / a run that never used --target.
  var targetColumnKeys = [];

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, function (ch) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch];
    });
  }

  function fmtSeconds(v) {
    return (typeof v === "number" ? v : 0).toFixed(3);
  }

  function detectColumn(node, key) {
    if (!node) return false;
    var v = node[key];
    if (v) return true;
    var children = node.children || [];
    for (var i = 0; i < children.length; i++) {
      if (detectColumn(children[i], key)) return true;
    }
    return false;
  }

  // Sums self utime/stime plus every descendant's, storing the result on
  // each node as _cumUtime/_cumStime/_cumTotal. Deliberately does NOT
  // reorder node.children -- callers keep the tree's original chronological
  // (fork-order) child ordering; only the displayed/filtered *time* figures
  // come from this pass, not sort order.
  function computeCumulative(node) {
    var cumUtime = node.utime_seconds || 0;
    var cumStime = node.stime_seconds || 0;
    var children = node.children || [];
    for (var i = 0; i < children.length; i++) {
      computeCumulative(children[i]);
      cumUtime += children[i]._cumUtime;
      cumStime += children[i]._cumStime;
    }
    node._cumUtime = cumUtime;
    node._cumStime = cumStime;
    node._cumTotal = cumUtime + cumStime;
    return node._cumTotal;
  }

  // Cumulative time is monotonically non-increasing down the tree (a child's
  // cumulative total can never exceed its parent's), so a node failing this
  // check means every descendant would too -- no need to search subtrees the
  // way search-text filtering does.
  function nodePassesTimeFilter(node) {
    if (!state.minSharePercent || totalCumSeconds <= 0) return true;
    return (node._cumTotal / totalCumSeconds) * 100 >= state.minSharePercent;
  }

  function formatColumnValue(node, def) {
    var v = node[def.key];
    if (def.pairKey) {
      var count = node[def.pairKey];
      return (typeof v === "number" ? v.toFixed(3) : v) + " (" + count + ")";
    }
    if (typeof v === "number" && def.key.indexOf("_seconds") !== -1) return v.toFixed(3);
    return v === null || v === undefined || v === "" ? "—" : String(v);
  }

  // Timeline mode's SVG is sized off container.clientWidth at render time (renderTimelineView()
  // below), so a window resize needs an explicit re-render to pick up the new width -- tree mode has
  // no such dependency (plain DOM flow layout), so this only fires when it'd actually matter.
  window.addEventListener("resize", debounce(function () {
    if (state.viewMode === "timeline") rerender();
  }, 150));

  fetch(cfg.jsonUrl)
    .then(function (r) { return r.json(); })
    .then(function (resp) {
      if (resp.error) {
        rootEl.textContent = "Error: " + resp.error;
        return;
      }
      if (cfg.mode === "diff") {
        renderDiff(resp.data);
      } else {
        renderSingle(resp.data);
      }
    })
    .catch(function (e) {
      rootEl.textContent = "Failed to load tree data: " + e;
    });

  // ---- single-tree mode ----

  function renderSingle(data) {
    if (data.tree_omitted) {
      renderControlsSingle([], data);
      renderTreeOmittedMessage(data);
      return;
    }

    totalCumSeconds = computeCumulative(data.tree);

    var extent = computeWallExtent(data.tree);
    totalWallSpan = Math.max(0, extent.max - extent.min);
    timelineFullDomain = [extent.min, extent.max > extent.min ? extent.max : extent.min + 1];
    timelineXDomain = timelineFullDomain.slice();

    var available = COLUMN_DEFS.filter(function (c) {
      return c.always || detectColumn(data.tree, c.key);
    });
    available.forEach(function (c) { state.columns[c.key] = false; });

    targetColumnKeys = [];
    collectTargetCounterKeysFromTree(data.tree, {}, targetColumnKeys);
    targetColumnKeys.forEach(function (key) { state.columns[key] = false; });

    lastAvailableColumns = available;
    lastSingleData = data;
    renderControlsSingle(available, data);
    renderTree(data.tree, null, []);
  }

  // Timeline view mode's own 100% baseline (recursive, not just the root's own start/finish, in
  // case a future collector ever nested something outside the root's own recorded span -- matches
  // web/static/timeline_viewer.js's computeTreeExtent()'s identical caution).
  function computeWallExtent(node) {
    var min = node.start, max = node.finish;
    (node.children || []).forEach(function (c) {
      var e = computeWallExtent(c);
      if (e.min < min) min = e.min;
      if (e.max > max) max = e.max;
    });
    return { min: min, max: max };
  }

  // Server (web/server.py's _api_tree_json) drops the "tree" key and sets
  // tree_omitted when the full JSON is too large to hand the browser --
  // shows the process-count line and the per-command summary table (both
  // small, comm-grouped, present regardless of tree size) but skips the
  // search/min-share/column controls, which only make sense once a tree is
  // actually rendered below.
  function renderTreeOmittedMessage(data) {
    rootEl.innerHTML = "";
    var p = document.createElement("p");
    var mb = data.tree_omitted_bytes ? (data.tree_omitted_bytes / (1024 * 1024)).toFixed(0) + " MB" : "too many bytes";
    p.appendChild(document.createTextNode(
        "Interactive tree view skipped: this run's proctree --json output is " + mb +
        ", too large to render as a tree in the browser. The summary table above covers " +
        "per-command totals; for the full per-process text view, "));
    if (data.summary_file_url) {
      var a = document.createElement("a");
      a.href = data.summary_file_url;
      a.textContent = "open " + data.summary_file_url.split("/").pop();
      p.appendChild(a);
      p.appendChild(document.createTextNode("."));
    } else {
      p.appendChild(document.createTextNode("see process.tree.summary.txt in the run directory."));
    }
    rootEl.appendChild(p);
  }

  function renderControlsSingle(available, data) {
    controlsEl.innerHTML = "";

    var info = document.createElement("p");
    info.className = "muted";
    info.textContent = data.process_count + " processes (max concurrent " +
        data.max_concurrent_processes + ")";
    controlsEl.appendChild(info);

    if (data.summary && data.summary.length) {
      controlsEl.appendChild(renderSummaryTable(data.summary));
    }

    if (data.tree_omitted) return;

    controlsEl.appendChild(makeViewModeToggle());
    controlsEl.appendChild(makeSearchInput());
    controlsEl.appendChild(makeMinShareInput());

    // The per-node column toggles below annotate tree-mode's inline row text -- they don't apply
    // the same way to timeline mode, which always shows a bar's full duration + target_counters in
    // its hover tooltip regardless of these checkboxes (see showTimelineTooltip()), so they're
    // hidden rather than left as dead controls.
    if (state.viewMode === "timeline") return;

    available.forEach(function (c) {
      controlsEl.appendChild(makeColumnToggle(c.key, c.label));
    });
    // --target's per-node columns (targetColumnKeys, set by renderSingle()):
    // same toggle-checkbox mechanism as the fixed COLUMN_DEFS entries above,
    // just driven by a dynamic key list instead of a static one.
    targetColumnKeys.forEach(function (key) {
      controlsEl.appendChild(makeColumnToggle(key, key));
    });
  }

  // Single-tree mode's "Tree" (default, existing hierarchy view) vs "Timeline" (this file's own
  // top-of-file comment) switch. Re-renders both the controls panel (some controls are tree-mode-
  // only, see renderControlsSingle() above) and the main view on click.
  function makeViewModeToggle() {
    var wrap = document.createElement("div");
    wrap.className = "ptv-mode-toggle";
    [["tree", "Tree"], ["timeline", "Timeline"]].forEach(function (pair) {
      var mode = pair[0], label = pair[1];
      var btn = document.createElement("button");
      btn.type = "button";
      btn.textContent = label;
      btn.className = "ptv-mode-btn" + (state.viewMode === mode ? " ptv-mode-btn-active" : "");
      btn.addEventListener("click", function () {
        if (state.viewMode === mode) return;
        state.viewMode = mode;
        renderControlsSingle(lastAvailableColumns, lastSingleData);
        rerender();
      });
      wrap.appendChild(btn);
    });
    return wrap;
  }

  function makeColumnToggle(key, label) {
    var wrapper = document.createElement("label");
    wrapper.className = "ptv-col-toggle";
    var cb = document.createElement("input");
    cb.type = "checkbox";
    cb.addEventListener("change", function () {
      state.columns[key] = cb.checked;
      rerender();
    });
    wrapper.appendChild(cb);
    wrapper.appendChild(document.createTextNode(" " + label));
    return wrapper;
  }

  function makeSearchInput() {
    var input = document.createElement("input");
    input.type = "text";
    input.className = "ptv-search";
    input.placeholder = "Search comm or pid...";
    input.value = state.search;
    input.addEventListener("input", function () {
      state.search = input.value.trim().toLowerCase();
      rerender();
    });
    return input;
  }

  function makeMinShareInput() {
    var label = document.createElement("label");
    label.className = "ptv-minshare";
    label.appendChild(document.createTextNode("Hide branches under "));
    var input = document.createElement("input");
    input.type = "number";
    input.className = "ptv-minshare-input";
    input.min = "0";
    input.max = "100";
    input.step = "1";
    input.value = String(state.minSharePercent);
    input.addEventListener("input", function () {
      var v = parseFloat(input.value);
      state.minSharePercent = isNaN(v) ? 0 : Math.max(0, Math.min(100, v));
      rerender();
    });
    label.appendChild(input);
    // Same number, different denominator per mode -- tree mode ranks by cumulative CPU time
    // (utime+stime), timeline mode by each process's own wall-clock span (what actually determines
    // whether a bar is even visible at the chart's time scale); the label always names which one is
    // currently in effect rather than leaving that implicit.
    label.appendChild(document.createTextNode(
        state.viewMode === "timeline" ? "% of the run's wall-clock span" : "% of total CPU time"));
    return label;
  }

  // Item 1: the run's already-computed per-comm rollup (proctree.c's
  // build_comm_table(), sorted descending by CPU time server-side), shown
  // as a "hot processes" list so the biggest consumers are visible without
  // expanding the tree at all. Clicking a row filters the tree to that comm
  // via the same search box/state used for text search, so "narrow to this
  // process and its descendants" is one click.
  // topdown's raw per-vendor label set (topdown.c's raw_counter_group("topdown", ...)):
  // Intel P-core has "core.topdown-{retiring,fe-bound,be-bound,bad-spec}" plus a "slots"
  // denominator; Gracemont has the same 4 names but no "slots" (falls back to cpu-cycles,
  // unused here since this collapse skips the slots-denominator entirely -- see below);
  // AMD has no equivalent friendly names at all ("ex_ret_ops"/"de_no_dispatch_per_slot.*"/
  // "de_src_op_disp.all"). Detected by presence, same convention topdown.c's own
  // find_ci_label() calls use to tell these apart server-side.
  function findLabel(entries, label) {
    return entries.filter(function (e) { return e.label === label; })[0];
  }

  function safeSub(a, b) { return a > b ? a - b : 0; }

  // Raw (unnormalized) {retiring, frontend, backend, speculation} for one
  // topdown group's entries, or null if this isn't a recognized topdown raw
  // counter set (an unsupported vendor, or partial data) -- same "zero
  // coverage rather than wrong coverage" convention as the rest of wspy
  // rather than guessing at a formula.
  function topdownRawCategories(entries) {
    var retiring = findLabel(entries, "core.topdown-retiring");
    if (retiring) {
      var frontend = findLabel(entries, "core.topdown-fe-bound");
      var backend = findLabel(entries, "core.topdown-be-bound");
      var speculation = findLabel(entries, "core.topdown-bad-spec");
      if (frontend && backend && speculation) {
        return { retiring: retiring.value, frontend: frontend.value,
                 backend: backend.value, speculation: speculation.value };
      }
      return null;
    }
    var exRetOps = findLabel(entries, "ex_ret_ops");
    if (exRetOps) {
      var feStall = findLabel(entries, "de_no_dispatch_per_slot.no_ops_from_frontend");
      var beStall = findLabel(entries, "de_no_dispatch_per_slot.backend_stalls");
      var dispatched = findLabel(entries, "de_src_op_disp.all");
      if (feStall && beStall && dispatched) {
        return { retiring: exRetOps.value, frontend: feStall.value, backend: beStall.value,
                 speculation: safeSub(dispatched.value, exRetOps.value) };
      }
    }
    return null;
  }

  // --target=comm=<name>[,cmdline=<substr>] (item 10, INVESTIGATION.md):
  // turns one node/row's raw target_counters array into the entries actually
  // displayed. Two special cases, neither of which shows the underlying raw
  // counters on their own since they're not individually meaningful:
  // - the "ipc" group's raw counters (instructions/cpu-cycles/ex_ret_ops on
  //   AMD -- whatever wspy attached to compute IPC from) collapse into a
  //   single derived "IPC" ratio when both "instructions" and "cpu-cycles"
  //   are present.
  // - the "topdown" group's raw counters (topdownRawCategories() above)
  //   collapse into the 4 major topdown categories (Retiring/Frontend
  //   Bound/Backend Bound/Bad Speculation), each shown as a percentage of
  //   the sum of all 4 -- deliberately *not* wspy's own %-of-pipeline-slots
  //   denominator (topdown.c's print_topdown()), which needs a per-core-type
  //   width multiplier (AMD Zen4 vs Zen5/5c is 6 vs 8 slots/cycle) this
  //   viewer has no way to know from the tree JSON alone; self-normalizing
  //   to the 4 categories' own sum sidesteps that guess entirely and always
  //   sums to 100%, at the cost of dropping wspy's contention/sanity-check
  //   nuance.
  // Every other group's counters (software's page_faults etc.) pass through
  // unchanged. No other group/counter-set gets this treatment (yet) -- see
  // INVESTIGATION.md if that changes.
  function computeDisplayCounters(rawList) {
    var byGroup = {};
    (rawList || []).forEach(function (tc) {
      (byGroup[tc.group] = byGroup[tc.group] || []).push(tc);
    });
    var out = [];
    Object.keys(byGroup).forEach(function (group) {
      var entries = byGroup[group];
      var instructions = findLabel(entries, "instructions");
      var cycles = findLabel(entries, "cpu-cycles");
      var topdownCats = group === "topdown" ? topdownRawCategories(entries) : null;
      if (group === "ipc" && instructions && cycles) {
        out.push({
          group: group, label: "IPC", isRatio: true,
          value: cycles.value > 0 ? instructions.value / cycles.value : 0
        });
      } else if (topdownCats) {
        var total = topdownCats.retiring + topdownCats.frontend +
                    topdownCats.backend + topdownCats.speculation;
        [["Retiring", topdownCats.retiring], ["Frontend Bound", topdownCats.frontend],
         ["Backend Bound", topdownCats.backend], ["Bad Speculation", topdownCats.speculation]
        ].forEach(function (pair) {
          out.push({
            group: group, label: pair[0], isPercent: true,
            value: total > 0 ? pair[1] / total * 100 : 0
          });
        });
      } else {
        entries.forEach(function (e) {
          out.push({ group: e.group, label: e.label, isRatio: false, value: e.value });
        });
      }
    });
    return out;
  }

  function targetCounterKey(entry) { return entry.group + "." + entry.label; }

  function formatTargetCounterValue(entry) {
    if (entry.isRatio) return entry.value.toFixed(3);
    if (entry.isPercent) return entry.value.toFixed(1) + "%";
    return String(entry.value);
  }

  function findTargetCounterEntry(row, key) {
    var entries = computeDisplayCounters(row.target_counters);
    for (var i = 0; i < entries.length; i++) {
      if (targetCounterKey(entries[i]) === key) return entries[i];
    }
    return null;
  }

  // Collects every distinct (group,label) key across all summary rows'
  // computeDisplayCounters() results -- there's no fixed column set to
  // hardcode, it depends on whatever counter_mask the producing wspy run
  // had active. Returns [] (no extra columns) for older JSON / runs that
  // never used --target, same "absent means not collected" convention as
  // the rest of this viewer.
  function collectTargetCounterKeys(summary) {
    var seen = {};
    var keys = [];
    summary.forEach(function (row) {
      computeDisplayCounters(row.target_counters).forEach(function (e) {
        var key = targetCounterKey(e);
        if (!seen[key]) {
          seen[key] = true;
          keys.push(key);
        }
      });
    });
    return keys;
  }

  // Same idea as collectTargetCounterKeys() above, but walking the whole
  // tree's own per-node target_counters (not just the top-level comm-rollup
  // rows) -- feeds the per-node column toggles, same auto-detect pattern
  // detectColumn() uses for the fixed COLUMN_DEFS entries.
  function collectTargetCounterKeysFromTree(node, seen, keys) {
    if (!node) return;
    computeDisplayCounters(node.target_counters).forEach(function (e) {
      var key = targetCounterKey(e);
      if (!seen[key]) {
        seen[key] = true;
        keys.push(key);
      }
    });
    (node.children || []).forEach(function (child) {
      collectTargetCounterKeysFromTree(child, seen, keys);
    });
  }

  function findTargetCounterValue(row, key) {
    var entry = findTargetCounterEntry(row, key);
    return entry ? formatTargetCounterValue(entry) : "0";
  }

  function renderSummaryTable(summary) {
    var table = document.createElement("table");
    table.className = "ptv-summary-table";
    var targetKeys = collectTargetCounterKeys(summary);
    var thead = document.createElement("tr");
    ["comm", "count", "utime", "stime", "total", "% of total"].concat(targetKeys).forEach(function (h) {
      var th = document.createElement("th");
      th.textContent = h;
      thead.appendChild(th);
    });
    table.appendChild(thead);
    summary.forEach(function (row) {
      var total = row.total_utime_seconds + row.total_stime_seconds;
      var pct = totalCumSeconds > 0 ? (total / totalCumSeconds * 100) : 0;
      var tr = document.createElement("tr");
      tr.className = "ptv-summary-row";
      [
        row.comm,
        String(row.count),
        row.total_utime_seconds.toFixed(3),
        row.total_stime_seconds.toFixed(3),
        total.toFixed(3),
        pct.toFixed(1) + "%"
      ].concat(targetKeys.map(function (key) {
        return String(findTargetCounterValue(row, key));
      })).forEach(function (val) {
        var td = document.createElement("td");
        td.textContent = val;
        tr.appendChild(td);
      });
      tr.addEventListener("click", function () {
        state.search = String(row.comm).toLowerCase();
        var searchInput = controlsEl.querySelector(".ptv-search");
        if (searchInput) searchInput.value = row.comm;
        rerender();
      });
      table.appendChild(tr);
    });
    return table;
  }

  var lastRenderArgs = null;

  function renderTree(tree, diffMetrics, summaryDiff) {
    lastRenderArgs = { tree: tree, diffMetrics: diffMetrics, summaryDiff: summaryDiff };
    rerender();
  }

  function rerender() {
    if (!lastRenderArgs) return;
    rootEl.innerHTML = "";
    if (lastRenderArgs.diffMetrics) {
      // diffMetrics is always an array (possibly empty, never undefined) in diff mode -- see
      // renderDiff() below -- so this branch, not state.viewMode, is what actually distinguishes
      // diff mode; the Tree/Timeline toggle is single-tree-mode-only (renderControlsDiff() never
      // renders it) and has no effect here regardless of its current value.
      rootEl.appendChild(renderDiffNode(lastRenderArgs.tree, 0));
    } else if (state.viewMode === "timeline") {
      renderTimelineView(lastRenderArgs.tree);
    } else {
      rootEl.appendChild(renderNode(lastRenderArgs.tree, 0));
    }
  }

  // ---- timeline view mode (single-tree mode only; see this file's own top-of-file comment) ----

  var SVG_NS = "http://www.w3.org/2000/svg";
  var TIMELINE_MARGIN = { top: 6, right: 16, bottom: 6, left: 190 }; // wide left gutter for labels
  var TIMELINE_ROW_HEIGHT = 18;
  // Hard cap on rendered rows, independent of what the min-share threshold computes to -- a
  // fork-heavy workload (hundreds of children starting/exiting within the same millisecond, e.g. a
  // parallel build) makes every duration tie at effectively the same value, degenerating a
  // percentile-based threshold to 0% and filtering nothing; same real bug (and same fix: an
  // unconditional cap keeping the longest-duration rows, ties broken by original depth-first order
  // since Array.prototype.sort is stable) timeline_viewer.js's own MAX_RENDERED_LANES already hit
  // and fixed -- ported here rather than re-derived.
  var TIMELINE_MAX_LANES = 150;

  var timelineTooltipEl = null;

  function svgEl(tag, attrs) {
    var el = document.createElementNS(SVG_NS, tag);
    for (var k in attrs) {
      if (Object.prototype.hasOwnProperty.call(attrs, k)) el.setAttribute(k, attrs[k]);
    }
    return el;
  }

  function debounce(fn, ms) {
    var timer = null;
    return function () { clearTimeout(timer); timer = setTimeout(fn, ms); };
  }

  function flattenAll(node, depth, out) {
    out.push({ node: node, depth: depth });
    (node.children || []).forEach(function (c) { flattenAll(c, depth + 1, out); });
  }

  function seriesColorTL(index) { return "var(--series-" + ((index % 8) + 1) + ")"; }

  function commColorIndexTL(comm, seen) {
    if (!Object.prototype.hasOwnProperty.call(seen, comm)) seen[comm] = Object.keys(seen).length;
    return seen[comm];
  }

  // Good/warn/bad verdict-bucket coloring (this app's existing convention, --good/--warn/--bad,
  // already used for phase bands elsewhere) by a --target-matched process's own Retiring % --
  // computeDisplayCounters() above already collapses topdown's raw counters into this self-
  // normalizing 4-category breakdown for the tree hierarchy view's per-node columns; reused as-is
  // here, just promoted from a toggleable text column to the bar's actual fill color. Returns null
  // (fall back to the plain comm-based categorical color) for anything that isn't a --target-matched
  // topdown group -- there's no universal "higher is better" direction for an arbitrary raw counter
  // (ipc, software page faults, ...) the way there is for topdown's own categories.
  function retiringColorFor(node) {
    var retiring = computeDisplayCounters(node.target_counters).filter(function (e) {
      return e.group === "topdown" && e.label === "Retiring";
    })[0];
    if (!retiring) return null;
    if (retiring.value >= 50) return "var(--good)";
    if (retiring.value >= 25) return "var(--warn)";
    return "var(--bad)";
  }

  // minSpan/search/cap filtering, independent of the current (possibly zoomed) timelineXDomain --
  // same reasoning timeline_viewer.js's own computeLaneRows()/drawSwimlane() split gives: keeping
  // this independent of zoom means row colors/identity stay stable as the user zooms, only which
  // rows are actually visible (renderTimelineView()'s own xDomain-overlap filter) changes.
  function computeTimelineRows(root) {
    var all = [];
    flattenAll(root, 0, all);
    var minSpan = totalWallSpan * (state.minSharePercent / 100);
    var search = state.search;
    var filtered = all.filter(function (r) {
      return (r.node.finish - r.node.start) >= minSpan && (!search || subtreeMatches(r.node, search));
    });
    var capped = filtered.length > TIMELINE_MAX_LANES;
    if (capped) {
      var byDuration = filtered.slice().sort(function (a, b) {
        return (b.node.finish - b.node.start) - (a.node.finish - a.node.start);
      }).slice(0, TIMELINE_MAX_LANES);
      var keepPids = {};
      byDuration.forEach(function (r) { keepPids[r.node.pid] = true; });
      filtered = filtered.filter(function (r) { return keepPids[r.node.pid]; });
    }
    var seenComm = {};
    var rows = filtered.map(function (r) {
      var counterColor = retiringColorFor(r.node);
      var color = counterColor || seriesColorTL(commColorIndexTL(r.node.comm || ("pid " + r.node.pid), seenComm));
      return { node: r.node, depth: r.depth, color: color, coloredByCounter: !!counterColor };
    });
    return { rows: rows, total: all.length, capped: capped };
  }

  function xScaleTL(width) {
    var span = (timelineXDomain[1] - timelineXDomain[0]) || 1;
    var plotWidth = width - TIMELINE_MARGIN.left - TIMELINE_MARGIN.right;
    return function (t) { return TIMELINE_MARGIN.left + (t - timelineXDomain[0]) / span * plotWidth; };
  }

  function clientXToTimeTL(svg, width, clientX) {
    var rect = svg.getBoundingClientRect();
    var scaleX = rect.width ? (width / rect.width) : 1;
    var svgX = (clientX - rect.left) * scaleX;
    var plotWidth = width - TIMELINE_MARGIN.left - TIMELINE_MARGIN.right;
    return timelineXDomain[0] + (svgX - TIMELINE_MARGIN.left) / (plotWidth || 1) * (timelineXDomain[1] - timelineXDomain[0]);
  }

  function ensureTimelineTooltip() {
    if (!timelineTooltipEl) {
      timelineTooltipEl = document.createElement("div");
      timelineTooltipEl.className = "itv-tooltip";
      timelineTooltipEl.style.display = "none";
      document.body.appendChild(timelineTooltipEl);
    }
    return timelineTooltipEl;
  }

  function hideTimelineTooltip() {
    if (timelineTooltipEl) timelineTooltipEl.style.display = "none";
  }

  function addTooltipRow(tip, name, value) {
    var row = document.createElement("div");
    row.className = "itv-tooltip-row";
    var val = document.createElement("span");
    val.className = "itv-tooltip-value";
    val.textContent = value;
    var label = document.createElement("span");
    label.className = "itv-tooltip-name";
    label.textContent = name;
    row.appendChild(val);
    row.appendChild(label);
    tip.appendChild(row);
  }

  // Full duration + every --target counter (via computeDisplayCounters()'s already-normalized
  // ratio/percent collapse, the same values the tree hierarchy view's own per-node column toggles
  // show) -- always shown regardless of state.columns, since this view has no per-column toggles of
  // its own (see renderControlsSingle()'s own comment on why).
  function showTimelineTooltip(node, clientX, clientY) {
    var tip = ensureTimelineTooltip();
    tip.innerHTML = "";
    var title = document.createElement("div");
    title.className = "itv-tooltip-time";
    title.textContent = (node.comm || "?") + " (pid " + node.pid + ", ppid " + node.ppid + ")";
    tip.appendChild(title);
    addTooltipRow(tip, "start", fmtSeconds(node.start) + "s");
    addTooltipRow(tip, "finish", fmtSeconds(node.finish) + "s");
    addTooltipRow(tip, "duration", fmtSeconds(node.finish - node.start) + "s");
    computeDisplayCounters(node.target_counters).forEach(function (e) {
      addTooltipRow(tip, e.group + "." + e.label, formatTargetCounterValue(e));
    });
    tip.style.display = "";
    tip.style.left = (clientX + 14) + "px";
    tip.style.top = (clientY + 14) + "px";
  }

  function wireTimelineInteractions(svg, xs, width, height, crosshair, zoomBand, rows) {
    var dragStartTime = null;
    svg.addEventListener("mousemove", function (ev) {
      var t = clientXToTimeTL(svg, width, ev.clientX);
      if (dragStartTime !== null) {
        var x0 = xs(dragStartTime), x1 = xs(t);
        zoomBand.setAttribute("x", Math.min(x0, x1));
        zoomBand.setAttribute("width", Math.abs(x1 - x0));
        zoomBand.style.display = "";
        return;
      }
      var x = xs(t);
      crosshair.setAttribute("x1", x);
      crosshair.setAttribute("x2", x);
      crosshair.style.display = "";
      var rect = svg.getBoundingClientRect();
      var scaleY = rect.height ? (height / rect.height) : 1;
      var svgY = (ev.clientY - rect.top) * scaleY;
      var rowIndex = Math.floor((svgY - TIMELINE_MARGIN.top) / TIMELINE_ROW_HEIGHT);
      var row = rows[rowIndex];
      if (!row || t < row.node.start || t > row.node.finish) { hideTimelineTooltip(); return; }
      showTimelineTooltip(row.node, ev.clientX, ev.clientY);
    });
    svg.addEventListener("mouseleave", function () {
      crosshair.style.display = "none";
      zoomBand.style.display = "none";
      hideTimelineTooltip();
      dragStartTime = null;
    });
    svg.addEventListener("mousedown", function (ev) {
      dragStartTime = clientXToTimeTL(svg, width, ev.clientX);
    });
    svg.addEventListener("mouseup", function (ev) {
      if (dragStartTime === null) return;
      var dragEndTime = clientXToTimeTL(svg, width, ev.clientX);
      zoomBand.style.display = "none";
      var span = timelineXDomain[1] - timelineXDomain[0];
      if (Math.abs(dragEndTime - dragStartTime) > span * 0.01) {
        timelineXDomain = [Math.min(dragStartTime, dragEndTime), Math.max(dragStartTime, dragEndTime)];
        dragStartTime = null;
        rerender();
        return;
      }
      dragStartTime = null;
    });
  }

  function renderTimelineView(root) {
    var result = computeTimelineRows(root);
    // Threshold/search/cap-filtered rows (computeTimelineRows(), independent of zoom) further
    // restricted to whatever overlaps the current (possibly zoomed) window -- so zooming in actually
    // shrinks the row list to what's relevant instead of leaving out-of-window rows' labels taking
    // up vertical space with no visible bar.
    var rows = result.rows.filter(function (r) {
      return r.node.finish >= timelineXDomain[0] && r.node.start <= timelineXDomain[1];
    });

    var note = document.createElement("p");
    note.className = "tlv-swimlane-note";
    note.textContent = "showing " + result.rows.length + " of " + result.total + " processes" +
        (state.minSharePercent > 0 ? " (own span ≥ " + state.minSharePercent + "% of run)" : "") +
        (result.capped ? " -- capped at the " + TIMELINE_MAX_LANES + " longest-running; raise the " +
            "threshold above to narrow further" : "");
    rootEl.appendChild(note);

    if (timelineXDomain[0] !== timelineFullDomain[0] || timelineXDomain[1] !== timelineFullDomain[1]) {
      var resetBtn = document.createElement("button");
      resetBtn.type = "button";
      resetBtn.textContent = "Reset zoom";
      resetBtn.addEventListener("click", function () {
        timelineXDomain = timelineFullDomain.slice();
        rerender();
      });
      rootEl.appendChild(resetBtn);
    }

    var container = document.createElement("div");
    rootEl.appendChild(container);

    var width = container.clientWidth || rootEl.clientWidth || 800;
    var height = TIMELINE_MARGIN.top + TIMELINE_MARGIN.bottom + Math.max(1, rows.length) * TIMELINE_ROW_HEIGHT;
    var svg = svgEl("svg", { class: "tlv-swimlane-svg", viewBox: "0 0 " + width + " " + height });
    var xs = xScaleTL(width);

    if (!rows.length) {
      var msg = svgEl("text", { class: "tlv-lane-label-muted", x: TIMELINE_MARGIN.left, y: TIMELINE_MARGIN.top + 12 });
      msg.textContent = "no processes at this threshold/search/zoom";
      svg.appendChild(msg);
    }

    rows.forEach(function (row, i) {
      var y = TIMELINE_MARGIN.top + i * TIMELINE_ROW_HEIGHT;
      var node = row.node;
      var x0 = xs(Math.max(node.start, timelineXDomain[0]));
      var x1 = xs(Math.min(node.finish, timelineXDomain[1]));
      if (x1 > x0) {
        svg.appendChild(svgEl("rect", {
          class: "tlv-bar" + (row.coloredByCounter ? " tlv-bar-target" : ""),
          x: x0, y: y + 2, width: Math.max(1, x1 - x0), height: TIMELINE_ROW_HEIGHT - 5, fill: row.color,
        }));
      }
      var indent = TIMELINE_MARGIN.left - 6 - Math.min(row.depth, 12) * 8;
      var label = svgEl("text", {
        class: "tlv-lane-label" + (nodeSelfMatches(node, state.search) && state.search ? " ptv-match" : ""),
        x: Math.max(4, indent), y: y + TIMELINE_ROW_HEIGHT - 6, "text-anchor": "end",
      });
      var name = (node.comm || "?") + " (" + node.pid + ")";
      label.textContent = name.length > 26 ? name.slice(0, 25) + "…" : name;
      svg.appendChild(label);
    });

    svg.appendChild(svgEl("line", {
      class: "itv-axis-line", x1: TIMELINE_MARGIN.left, x2: TIMELINE_MARGIN.left,
      y1: TIMELINE_MARGIN.top, y2: height - TIMELINE_MARGIN.bottom,
    }));

    var overlay = svgEl("rect", {
      x: TIMELINE_MARGIN.left, y: TIMELINE_MARGIN.top,
      width: Math.max(0, width - TIMELINE_MARGIN.left - TIMELINE_MARGIN.right),
      height: Math.max(0, height - TIMELINE_MARGIN.top - TIMELINE_MARGIN.bottom), fill: "transparent",
    });
    svg.appendChild(overlay);
    var crosshair = svgEl("line", {
      class: "itv-crosshair", x1: 0, x2: 0, y1: TIMELINE_MARGIN.top, y2: height - TIMELINE_MARGIN.bottom,
    });
    crosshair.style.display = "none";
    svg.appendChild(crosshair);
    var zoomBand = svgEl("rect", {
      class: "itv-zoom-band", x: 0, y: TIMELINE_MARGIN.top, width: 0,
      height: Math.max(0, height - TIMELINE_MARGIN.top - TIMELINE_MARGIN.bottom),
    });
    zoomBand.style.display = "none";
    svg.appendChild(zoomBand);

    container.innerHTML = "";
    container.appendChild(svg);

    wireTimelineInteractions(svg, xs, width, height, crosshair, zoomBand, rows);
  }

  function nodeSelfMatches(node, search) {
    if (!search) return true;
    if (String(node.pid).indexOf(search) !== -1) return true;
    if (node.comm && node.comm.toLowerCase().indexOf(search) !== -1) return true;
    return false;
  }

  function subtreeMatches(node, search) {
    if (nodeSelfMatches(node, search)) return true;
    var children = node.children || [];
    for (var i = 0; i < children.length; i++) {
      if (subtreeMatches(children[i], search)) return true;
    }
    return false;
  }

  function makeMetric(label, value) {
    var span = document.createElement("span");
    span.className = "ptv-metric";
    span.textContent = label + "=" + value;
    return span;
  }

  // --symbol-sample "Profile" drill-down (item 9, INVESTIGATION.md's
  // "Symbol-level profiling deep-dive"): a node carries target_maps only if
  // wspy's is_symbol_sample counter actually opened for it (topdown.c's
  // write_target_maps() is gated on that, same as the have_symbol_sample
  // check there) -- target_samples itself may still be empty (a short/idle
  // process can genuinely accrue zero samples), so the button's presence is
  // keyed on target_maps, not target_samples. Fetched result/expanded state
  // is cached directly on the node object (node._profile*) rather than in
  // component-local state, so it survives a rerender() triggered by
  // toggling an unrelated column checkbox -- unlike this file's plain
  // expand/collapse toggle, which currently does reset on rerender (an
  // existing, unrelated limitation, not something this feature needs to fix).
  function hasSymbolSampleData(node) {
    return !!(cfg.symbolizeUrl && node.target_maps && node.target_maps.length);
  }

  function fetchProfile(node, onDone) {
    if (node._profileResult || node._profileError || node._profileLoading) {
      onDone();
      return;
    }
    node._profileLoading = true;
    onDone();
    fetch(cfg.symbolizeUrl + "?pid=" + encodeURIComponent(node.pid))
      .then(function (r) { return r.json(); })
      .then(function (resp) {
        node._profileLoading = false;
        if (resp.error) {
          node._profileError = resp.error;
        } else {
          node._profileResult = resp.data;
        }
        onDone();
      })
      .catch(function (e) {
        node._profileLoading = false;
        node._profileError = String(e);
        onDone();
      });
  }

  function renderProfilePanel(node) {
    var panel = document.createElement("div");
    panel.className = "ptv-profile-panel";

    if (node._profileLoading) {
      panel.textContent = "Loading profile...";
      return panel;
    }
    if (node._profileError) {
      panel.textContent = "Profile error: " + node._profileError;
      return panel;
    }
    var data = node._profileResult;
    if (!data) {
      panel.textContent = "Profile not loaded.";
      return panel;
    }

    var info = document.createElement("p");
    info.className = "muted";
    info.textContent = "events=" + (data.events.join(",") || "(none)") +
        "  total=" + data.total_samples + "  resolved=" + data.resolved_samples +
        "  unresolved=" + data.unresolved_samples +
        (data.samples_lost ? "  lost=" + data.samples_lost : "") +
        (data.addr2line_unavailable ? "  (addr2line not found -- nothing could be resolved)" : "");
    panel.appendChild(info);

    if (!data.symbols.length && !data.unresolved.length) {
      var none = document.createElement("p");
      none.className = "muted";
      none.textContent = "No samples collected (a short/idle process can genuinely accrue zero).";
      panel.appendChild(none);
      return panel;
    }

    var table = document.createElement("table");
    table.className = "ptv-profile-table";
    var thead = document.createElement("tr");
    ["count", "% resolved", "symbol", "file", "source"].forEach(function (h) {
      var th = document.createElement("th");
      th.textContent = h;
      thead.appendChild(th);
    });
    table.appendChild(thead);
    data.symbols.forEach(function (row) {
      var tr = document.createElement("tr");
      [
        String(row.count), row.pct_of_resolved.toFixed(1) + "%",
        row.symbol, row.file, row.source || "—"
      ].forEach(function (val) {
        var td = document.createElement("td");
        td.textContent = val;
        tr.appendChild(td);
      });
      table.appendChild(tr);
    });
    data.unresolved.forEach(function (row) {
      var tr = document.createElement("tr");
      tr.className = "ptv-profile-unresolved";
      [String(row.count), "—", "<unresolved: " + row.reason + ">", row.file || "—", "—"]
        .forEach(function (val) {
          var td = document.createElement("td");
          td.textContent = val;
          tr.appendChild(td);
        });
      table.appendChild(tr);
    });
    panel.appendChild(table);
    return panel;
  }

  function renderNode(node, depth) {
    var search = state.search;
    var container = document.createElement("div");
    container.className = "ptv-node";
    if ((search && !subtreeMatches(node, search)) || !nodePassesTimeFilter(node)) {
      container.style.display = "none";
      return container;
    }

    var children = node.children || [];
    var hasChildren = children.length > 0;
    // Auto-expand hot subtrees (cumulative time share at/above the
    // threshold) instead of a fixed depth cutoff, so a deep hot chain opens
    // by default and a shallow idle one doesn't. Falls back to the old
    // depth<3 rule when there's no meaningful cumulative time to rank by
    // (e.g. a near-instant workload where every node's time rounds to 0).
    var cumShare = totalCumSeconds > 0 ? (node._cumTotal / totalCumSeconds) : 0;
    var expanded = (totalCumSeconds > 0 ? cumShare >= EXPAND_TIME_SHARE : depth < 3) ||
        (search && subtreeMatches(node, search));

    var row = document.createElement("div");
    row.className = "ptv-row";
    row.style.paddingLeft = (depth * 1.25) + "em";

    var toggle = document.createElement("span");
    toggle.className = "ptv-toggle";
    toggle.textContent = hasChildren ? (expanded ? "▼" : "▶") : "·";
    row.appendChild(toggle);

    var label = document.createElement("span");
    label.className = "ptv-label" + (nodeSelfMatches(node, search) && search ? " ptv-match" : "");
    label.textContent = node.pid + ") " + (node.comm || "??");
    row.appendChild(label);

    row.appendChild(makeMetric("cpu", fmtSeconds(node.utime_seconds) + "u/" + fmtSeconds(node.stime_seconds) + "s"));
    if (hasChildren) {
      var cumPct = totalCumSeconds > 0 ? (node._cumTotal / totalCumSeconds * 100) : 0;
      row.appendChild(makeMetric("Σcpu", fmtSeconds(node._cumUtime) + "u/" + fmtSeconds(node._cumStime) +
          "s (" + cumPct.toFixed(1) + "%)"));
    }

    COLUMN_DEFS.forEach(function (c) {
      if (state.columns[c.key]) {
        row.appendChild(makeMetric(c.label, formatColumnValue(node, c)));
      }
    });
    // --target's per-node counters (item 10, INVESTIGATION.md): this node's
    // own target_counters, not the comm-summed rollup the summary table
    // shows -- toggled the same way as the columns above.
    targetColumnKeys.forEach(function (key) {
      if (state.columns[key]) {
        var entry = findTargetCounterEntry(node, key);
        if (entry) row.appendChild(makeMetric(entry.label, formatTargetCounterValue(entry)));
      }
    });

    var profilePanelDiv = null;
    if (hasSymbolSampleData(node)) {
      var profileBtn = document.createElement("span");
      profileBtn.className = "ptv-profile-btn";
      profilePanelDiv = document.createElement("div");
      profilePanelDiv.className = "ptv-profile-panel-wrap";

      var updatePanel = function () {
        profileBtn.textContent = node._profileExpanded ? "▼ profile" : "▶ profile";
        profilePanelDiv.innerHTML = "";
        profilePanelDiv.style.display = node._profileExpanded ? "" : "none";
        if (node._profileExpanded) profilePanelDiv.appendChild(renderProfilePanel(node));
      };
      profileBtn.addEventListener("click", function () {
        node._profileExpanded = !node._profileExpanded;
        if (node._profileExpanded) fetchProfile(node, updatePanel);
        else updatePanel();
      });
      updatePanel();
      row.appendChild(profileBtn);
    }

    container.appendChild(row);
    if (profilePanelDiv) container.appendChild(profilePanelDiv);

    if (hasChildren) {
      var childrenDiv = document.createElement("div");
      childrenDiv.className = "ptv-children";
      childrenDiv.style.display = expanded ? "" : "none";
      children.forEach(function (child) {
        childrenDiv.appendChild(renderNode(child, depth + 1));
      });
      toggle.addEventListener("click", function () {
        var nowExpanded = childrenDiv.style.display === "none";
        childrenDiv.style.display = nowExpanded ? "" : "none";
        toggle.textContent = nowExpanded ? "▼" : "▶";
      });
      container.appendChild(childrenDiv);
    }

    return container;
  }

  // ---- diff mode ----

  function renderDiff(data) {
    renderControlsDiff(data);
    renderTree(data.diff_tree, data.diff_metrics || [], data.summary_diff || []);
  }

  function renderControlsDiff(data) {
    controlsEl.innerHTML = "";

    var info = document.createElement("p");
    info.className = "muted";
    info.textContent = "A: " + data.run_a.source_file + " (" + data.run_a.process_count + " processes) vs " +
        "B: " + data.run_b.source_file + " (" + data.run_b.process_count + " processes)";
    controlsEl.appendChild(info);

    controlsEl.appendChild(makeSearchInput());

    var label = document.createElement("label");
    label.className = "ptv-col-toggle";
    var cb = document.createElement("input");
    cb.type = "checkbox";
    cb.addEventListener("change", function () {
      state.showDeltas = cb.checked;
      rerender();
    });
    label.appendChild(cb);
    label.appendChild(document.createTextNode(" show deltas"));
    controlsEl.appendChild(label);

    if (data.summary_diff && data.summary_diff.length) {
      controlsEl.appendChild(renderSummaryDiffTable(data.summary_diff));
    }
  }

  function renderSummaryDiffTable(summaryDiff) {
    var table = document.createElement("table");
    table.className = "ptv-summary-diff";
    var thead = document.createElement("tr");
    ["comm", "status", "count A→B", "utime A→B", "stime A→B"].forEach(function (h) {
      var th = document.createElement("th");
      th.textContent = h;
      thead.appendChild(th);
    });
    table.appendChild(thead);
    summaryDiff.forEach(function (row) {
      var tr = document.createElement("tr");
      tr.className = "ptv-badge-" + row.status;
      [
        row.comm,
        row.status,
        row.count_a + "→" + row.count_b,
        row.total_utime_seconds_a.toFixed(3) + "→" + row.total_utime_seconds_b.toFixed(3),
        row.total_stime_seconds_a.toFixed(3) + "→" + row.total_stime_seconds_b.toFixed(3)
      ].forEach(function (val) {
        var td = document.createElement("td");
        td.textContent = val;
        tr.appendChild(td);
      });
      table.appendChild(tr);
    });
    return table;
  }

  function diffNodeSelfMatches(node, search) {
    if (!search) return true;
    var pid = (node.a && node.a.pid) || (node.b && node.b.pid) || "";
    if (String(pid).indexOf(search) !== -1) return true;
    if (node.comm && node.comm.toLowerCase().indexOf(search) !== -1) return true;
    return false;
  }

  function diffSubtreeMatches(node, search) {
    if (diffNodeSelfMatches(node, search)) return true;
    var children = node.children || [];
    for (var i = 0; i < children.length; i++) {
      if (diffSubtreeMatches(children[i], search)) return true;
    }
    return false;
  }

  function renderDiffNode(node, depth) {
    var search = state.search;
    var container = document.createElement("div");
    container.className = "ptv-node";
    if (search && !diffSubtreeMatches(node, search)) {
      container.style.display = "none";
      return container;
    }

    var children = node.children || [];
    var hasChildren = children.length > 0;
    var expanded = depth < 3 || node.status !== "same" || (search && diffSubtreeMatches(node, search));

    var row = document.createElement("div");
    row.className = "ptv-row ptv-badge-" + node.status;
    row.style.paddingLeft = (depth * 1.25) + "em";

    var toggle = document.createElement("span");
    toggle.className = "ptv-toggle";
    toggle.textContent = hasChildren ? (expanded ? "▼" : "▶") : "·";
    row.appendChild(toggle);

    var badge = document.createElement("span");
    badge.className = "ptv-status-badge ptv-status-" + node.status;
    badge.textContent = node.status;
    row.appendChild(badge);

    var pidLabel = node.a && node.b ? node.a.pid + "/" + node.b.pid :
        node.a ? String(node.a.pid) : String(node.b.pid);
    var label = document.createElement("span");
    label.className = "ptv-label" + (diffNodeSelfMatches(node, search) && search ? " ptv-match" : "");
    label.textContent = pidLabel + ") " + node.comm;
    row.appendChild(label);

    if (node.status === "matched" || node.status === "changed" || node.status === "same") {
      row.appendChild(makeMetric("utime", node.a.utime_seconds.toFixed(3) + "→" + node.b.utime_seconds.toFixed(3)));
      row.appendChild(makeMetric("stime", node.a.stime_seconds.toFixed(3) + "→" + node.b.stime_seconds.toFixed(3)));
      if (state.showDeltas && node.delta) {
        Object.keys(node.delta).forEach(function (key) {
          var d = node.delta[key];
          if (d !== 0) row.appendChild(makeMetric("Δ" + key, (d > 0 ? "+" : "") + d.toFixed(3)));
        });
      }
    }

    container.appendChild(row);

    if (hasChildren) {
      var childrenDiv = document.createElement("div");
      childrenDiv.className = "ptv-children";
      childrenDiv.style.display = expanded ? "" : "none";
      children.forEach(function (child) {
        childrenDiv.appendChild(renderDiffNode(child, depth + 1));
      });
      toggle.addEventListener("click", function () {
        var nowExpanded = childrenDiv.style.display === "none";
        childrenDiv.style.display = nowExpanded ? "" : "none";
        toggle.textContent = nowExpanded ? "▼" : "▶";
      });
      container.appendChild(childrenDiv);
    }

    return container;
  }
})();
