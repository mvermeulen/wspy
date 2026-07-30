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

  var state = { search: "", columns: {}, showDeltas: false, minSharePercent: 0 };

  // Single-tree mode only: sum of every node's utime+stime in the fetched
  // tree, set once by computeCumulative() before first render. Used as the
  // 100% baseline for each node's cumulative-time share (row display, the
  // auto-expand threshold, and the "hide branches under N%" filter).
  var totalCumSeconds = 0;

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
    totalCumSeconds = computeCumulative(data.tree);

    var available = COLUMN_DEFS.filter(function (c) {
      return c.always || detectColumn(data.tree, c.key);
    });
    available.forEach(function (c) { state.columns[c.key] = false; });

    targetColumnKeys = [];
    collectTargetCounterKeysFromTree(data.tree, {}, targetColumnKeys);
    targetColumnKeys.forEach(function (key) { state.columns[key] = false; });

    renderControlsSingle(available, data);
    renderTree(data.tree, null, []);
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

    controlsEl.appendChild(makeSearchInput());
    controlsEl.appendChild(makeMinShareInput());

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
    label.appendChild(document.createTextNode("% of total CPU time"));
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
      rootEl.appendChild(renderDiffNode(lastRenderArgs.tree, 0));
    } else {
      rootEl.appendChild(renderNode(lastRenderArgs.tree, 0));
    }
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

    container.appendChild(row);

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
