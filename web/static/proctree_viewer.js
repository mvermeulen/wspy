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

  var state = { search: "", columns: {}, showDeltas: false };

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
    var available = COLUMN_DEFS.filter(function (c) {
      return c.always || detectColumn(data.tree, c.key);
    });
    available.forEach(function (c) { state.columns[c.key] = false; });

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

    controlsEl.appendChild(makeSearchInput());

    available.forEach(function (c) {
      var label = document.createElement("label");
      label.className = "ptv-col-toggle";
      var cb = document.createElement("input");
      cb.type = "checkbox";
      cb.addEventListener("change", function () {
        state.columns[c.key] = cb.checked;
        rerender();
      });
      label.appendChild(cb);
      label.appendChild(document.createTextNode(" " + c.label));
      controlsEl.appendChild(label);
    });
  }

  function makeSearchInput() {
    var input = document.createElement("input");
    input.type = "text";
    input.className = "ptv-search";
    input.placeholder = "Search comm or pid...";
    input.addEventListener("input", function () {
      state.search = input.value.trim().toLowerCase();
      rerender();
    });
    return input;
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
    if (search && !subtreeMatches(node, search)) {
      container.style.display = "none";
      return container;
    }

    var children = node.children || [];
    var hasChildren = children.length > 0;
    var expanded = depth < 3 || (search && subtreeMatches(node, search));

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

    COLUMN_DEFS.forEach(function (c) {
      if (state.columns[c.key]) {
        row.appendChild(makeMetric(c.label, formatColumnValue(node, c)));
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
