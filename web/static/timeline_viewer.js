/*
 * timeline_viewer.js -- combined process-tree + --interval-timeline view. Loaded only by
 * /timeline-viewer/... (via window.TLV_CONFIG = {treeJsonUrl, intervalJsonUrl} set inline by
 * server.py's render_timeline_viewer()), same "page-specific JS stays out of every other page's
 * payload" reasoning proctree_viewer.js/interval_viewer.js's own docstrings give.
 *
 * server.py only ever links here when joblib.find_combined_timeline_csv() found a pass whose own
 * wspy manifest recorded both --tree and --interval together -- i.e. the two JSON payloads fetched
 * below (the same /api/tree-json and /api/interval-json interval_viewer.js/proctree_viewer.js
 * already use) came from the exact same wspy invocation and therefore share one clock origin
 * (topdown.c's single start_time, set once before the child launches). That's what makes it safe
 * to plot them on one shared x-domain here; see find_combined_timeline_csv()'s docstring for why a
 * --tree pass and a separate --interval pass in the same profile must NOT be paired this way.
 *
 * Deliberately narrower in scope than interval_viewer.js: only the shared 0-100% percentage chart
 * (topdown axes + GPU busy/activity) is reproduced here, not every small-multiple column -- the
 * point of this page is correlating *which process was running* against the phase/topdown shape,
 * not replacing the full-column interval viewer (linked from the page header for that). Below the
 * percentage chart, a new swimlane chart renders the process tree as horizontal fork->exit spans,
 * flattened depth-first (so a subtree stays contiguous) and filtered by a minimum-duration share
 * the same way proctree_viewer.js's own "hide branches under N%" control works, since a real tree
 * can have thousands of processes -- unlike that control (which filters by cumulative CPU time),
 * this one filters by wall-clock span, since that's what determines whether a bar is even visible
 * at this chart's time scale. A process filtered out by that threshold can leave a "gap" in a
 * visible child's indentation (its parent's own bar is hidden but the child's still qualifies) --
 * a known, accepted limitation, not a bug: the depth number itself is always still correct, only
 * the row immediately above it may not be its literal parent. The threshold alone is not treated as
 * a hard limit -- a fork-heavy workload where hundreds of children start and exit within the same
 * millisecond (a real, common case, e.g. a parallel build) makes every duration tie at effectively
 * the same value, degenerating the threshold to 0% and filtering nothing -- so computeLaneRows()
 * also enforces MAX_RENDERED_LANES directly as a backstop, independent of what the threshold computes.
 *
 * Both charts share one xDomain/crosshair, same multi-chart-sync idiom interval_viewer.js
 * established: chartEntries holds {svg, xs, crosshair}, one per chart, and moveCrosshairTo(t)
 * updates every entry's crosshair line together regardless of which chart the pointer is over.
 * Click-drag zoom rescales the shared xDomain and re-renders both charts. No charting library, same
 * hand-rolled-SVG precedent every other viewer in this codebase follows.
 */
(function () {
  "use strict";

  var cfg = window.TLV_CONFIG || {};
  var rootEl = document.getElementById("tlv-root");
  var controlsEl = document.getElementById("tlv-controls");

  if (!cfg.treeJsonUrl || !cfg.intervalJsonUrl || !rootEl) {
    return;
  }

  var SVG_NS = "http://www.w3.org/2000/svg";
  var MARGIN = { top: 10, right: 16, bottom: 22, left: 46 };      // percentage chart, same as interval_viewer.js
  var SWIM_MARGIN = { top: 6, right: 16, bottom: 6, left: 190 };  // swimlane chart: wide left gutter for labels
  var PCT_HEIGHT = 220;
  var ROW_HEIGHT = 18;
  var SERIES_COLOR_SLOTS = 8; // --series-1..8 custom properties, style.css

  var KNOWN_PCT_COLUMNS = ["retire", "frontend", "backend", "speculate", "confidence", "sanity"];
  var DEFAULT_VISIBLE = ["retire", "frontend", "backend", "speculate"];

  // Hard cap on rendered swimlane rows -- unlike a plain percentile threshold, this is enforced
  // unconditionally regardless of what minSharePercent computes to (see computeLaneRows()'s own
  // comment for why a threshold alone isn't a safe cap on its own), same "don't hand the browser
  // 50000 rows unasked" reasoning INTERVAL_VIEWER_MAX_ROWS documents for the plain interval viewer.
  var MAX_RENDERED_LANES = 150;

  var treeData = null;       // data.data from /api/tree-json (tree_omitted / process_count / tree / summary)
  var intervalData = null;   // full parsed response from /api/interval-json
  var pctColumns = [];
  var otherColumnsSkippedNote = false;
  var visible = {};
  var phaseBands = [];
  var fullDomain = [0, 1];
  var xDomain = [0, 1];
  var chartEntries = [];     // {svg, xs, crosshair}, one per rendered chart (pct + swimlane)
  var tooltipEl = null;

  var laneRows = [];         // flattened, filtered {node, depth, color} rows currently rendered
  var minSharePercent = 0;   // % of fullDomain span a node's own (start,finish) span must reach to show
  var laneTotalCount = 0;    // process_count before filtering, for the "showing N of M" note
  var laneCapped = false;    // true if MAX_RENDERED_LANES truncated the threshold-filtered result

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

  function isPctColumn(col) {
    return col.slice(-4) === "_pct" ||
      KNOWN_PCT_COLUMNS.indexOf(col) !== -1 ||
      intervalData.gpu_columns.indexOf(col) !== -1;
  }

  function seriesColor(index) {
    return "var(--series-" + ((index % SERIES_COLOR_SLOTS) + 1) + ")";
  }

  // ---------------------------------------------------------------------
  // Data loading
  // ---------------------------------------------------------------------

  function fetchJson(url) {
    return fetch(url).then(function (r) {
      return r.json().then(function (body) { return { ok: r.ok, body: body }; });
    });
  }

  function renderError(message) {
    rootEl.innerHTML = "";
    var p = document.createElement("p");
    p.className = "muted";
    p.textContent = "Could not load timeline: " + message;
    rootEl.appendChild(p);
  }

  Promise.all([fetchJson(cfg.treeJsonUrl), fetchJson(cfg.intervalJsonUrl)])
    .then(function (results) {
      var treeRes = results[0], intervalRes = results[1];
      if (!treeRes.ok) { renderError((treeRes.body && treeRes.body.error) || "failed to load tree"); return; }
      if (!intervalRes.ok) { renderError((intervalRes.body && intervalRes.body.error) || "failed to load timeline"); return; }
      onData(treeRes.body.data, intervalRes.body);
    })
    .catch(function (err) { renderError(err.message); });

  function onData(treeBody, intervalBody) {
    treeData = treeBody;
    intervalData = intervalBody;

    pctColumns = [];
    intervalData.columns.forEach(function (col) {
      if (intervalData.dimension_columns.indexOf(col) !== -1) return;
      if (isPctColumn(col)) pctColumns.push(col); else otherColumnsSkippedNote = true;
    });

    visible = {};
    var haveDefault = false;
    pctColumns.forEach(function (col) {
      var isDefault = DEFAULT_VISIBLE.indexOf(col) !== -1;
      if (isDefault) haveDefault = true;
      visible[col] = isDefault;
    });
    if (!haveDefault) pctColumns.slice(0, 4).forEach(function (col) { visible[col] = true; });
    intervalData.gpu_columns.forEach(function (col) {
      if (pctColumns.indexOf(col) !== -1) visible[col] = true;
    });

    phaseBands = computePhaseBands();

    var times = intervalData.series.time || [];
    var intervalExtent = times.length ? [times[0], times[times.length - 1]] : null;
    var treeExtent = treeData.tree_omitted ? null : computeTreeExtent(treeData.tree);
    fullDomain = unionExtent(intervalExtent, treeExtent) || [0, 1];
    if (fullDomain[0] === fullDomain[1]) fullDomain = [fullDomain[0], fullDomain[0] + 1];
    xDomain = fullDomain.slice();

    if (!treeData.tree_omitted) {
      minSharePercent = computeDefaultMinShare(treeData.tree, fullDomain[1] - fullDomain[0]);
    }

    renderControls();
    renderAll();
    window.addEventListener("resize", debounce(renderAll, 150));
  }

  function computePhaseBands() {
    var phase = intervalData.series.phase;
    var time = intervalData.series.time;
    if (!phase || !time || !phase.length) return [];
    var bands = [];
    var i = 0;
    while (i < phase.length) {
      var j = i;
      while (j + 1 < phase.length && phase[j + 1] === phase[i]) j++;
      bands.push({ start: time[i], end: time[j], phase: phase[i] });
      i = j + 1;
    }
    return bands;
  }

  function computeTreeExtent(node) {
    var min = node.start, max = node.finish;
    (node.children || []).forEach(function (c) {
      var e = computeTreeExtent(c);
      if (e.min < min) min = e.min;
      if (e.max > max) max = e.max;
    });
    return { min: min, max: max };
  }

  function unionExtent(intervalExtent, treeExtent) {
    if (!intervalExtent && !treeExtent) return null;
    if (!intervalExtent) return [treeExtent.min, treeExtent.max];
    if (!treeExtent) return intervalExtent.slice();
    return [Math.min(intervalExtent[0], treeExtent.min), Math.max(intervalExtent[1], treeExtent.max)];
  }

  // ---------------------------------------------------------------------
  // Swimlane row model
  // ---------------------------------------------------------------------

  function flattenTree(root) {
    var rows = [];
    (function walk(node, depth) {
      rows.push({ node: node, depth: depth });
      (node.children || []).forEach(function (c) { walk(c, depth + 1); });
    })(root, 0);
    return rows;
  }

  // Best-effort *starting value* for the min-duration control, not a guarantee -- computeLaneRows()
  // below enforces the actual MAX_RENDERED_LANES cap unconditionally. This percentile estimate can
  // legitimately land at 0% (filtering nothing) when durations don't vary enough to separate them,
  // most commonly a fork-heavy workload where hundreds of children start and exit within the same
  // millisecond -- real, not a bug -- which is exactly why the hard cap exists as a backstop rather
  // than relying on this threshold alone.
  function computeDefaultMinShare(root, totalSpan) {
    var all = flattenTree(root);
    laneTotalCount = all.length;
    if (all.length <= MAX_RENDERED_LANES || totalSpan <= 0) return 0;
    var durations = all.map(function (r) { return r.node.finish - r.node.start; })
      .sort(function (a, b) { return b - a; });
    var cutoff = durations[MAX_RENDERED_LANES - 1] || 0;
    return Math.max(0, cutoff / totalSpan * 100);
  }

  function commColorIndex(comm, indexByComm) {
    if (!Object.prototype.hasOwnProperty.call(indexByComm, comm)) {
      indexByComm[comm] = Object.keys(indexByComm).length;
    }
    return indexByComm[comm];
  }

  function computeLaneRows() {
    if (treeData.tree_omitted) { laneRows = []; laneCapped = false; return; }
    var all = flattenTree(treeData.tree);
    var totalSpan = fullDomain[1] - fullDomain[0];
    var minSpan = totalSpan * (minSharePercent / 100);
    var filtered = all.filter(function (r) {
      return (r.node.finish - r.node.start) >= minSpan;
    });
    laneCapped = filtered.length > MAX_RENDERED_LANES;
    if (laneCapped) {
      // Threshold filtering alone can't be trusted to hold the row count down -- see
      // computeDefaultMinShare()'s comment on why it can degenerate to 0% -- so enforce the cap
      // directly here: keep the MAX_RENDERED_LANES longest-duration rows. Array.prototype.sort is
      // stable (ES2019+, every browser this codebase targets), so duration ties keep their original
      // depth-first order -- a deterministic choice among ties, not an arbitrary one.
      var byDuration = filtered.slice().sort(function (a, b) {
        return (b.node.finish - b.node.start) - (a.node.finish - a.node.start);
      }).slice(0, MAX_RENDERED_LANES);
      var keepPids = {};
      byDuration.forEach(function (r) { keepPids[r.node.pid] = true; });
      filtered = filtered.filter(function (r) { return keepPids[r.node.pid]; });
    }
    var indexByComm = {};
    laneRows = filtered.map(function (r) {
      var comm = r.node.comm || ("pid " + r.node.pid);
      return { node: r.node, depth: r.depth, color: seriesColor(commColorIndex(comm, indexByComm)) };
    });
  }

  // ---------------------------------------------------------------------
  // Controls
  // ---------------------------------------------------------------------

  function renderControls() {
    controlsEl.innerHTML = "";

    if (intervalData.decimated) {
      var note = document.createElement("p");
      note.className = "muted";
      note.textContent = "Timeline showing " + intervalData.series.time.length + " of " +
        intervalData.row_count + " --interval rows (decimated for display).";
      controlsEl.appendChild(note);
    }
    if (otherColumnsSkippedNote) {
      var note2 = document.createElement("p");
      note2.className = "muted";
      note2.textContent = "Non-percentage columns (raw counts, MB, watts, ...) aren't shown here -- " +
        "use \"Full timeline (all columns)\" above for those.";
      controlsEl.appendChild(note2);
    }

    if (pctColumns.length) {
      var legend = document.createElement("div");
      legend.className = "itv-legend";
      pctColumns.forEach(function (col, i) {
        var label = document.createElement("label");
        var cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = !!visible[col];
        cb.addEventListener("change", function () { visible[col] = cb.checked; renderAll(); });
        var swatch = document.createElement("span");
        swatch.className = "itv-swatch";
        swatch.style.background = seriesColor(i);
        var text = document.createElement("span");
        text.textContent = col;
        label.appendChild(cb); label.appendChild(swatch); label.appendChild(text);
        legend.appendChild(label);
      });
      controlsEl.appendChild(legend);
    }

    if (!treeData.tree_omitted) {
      var shareLabel = document.createElement("label");
      shareLabel.textContent = "min process duration: ";
      var shareInput = document.createElement("input");
      shareInput.type = "number";
      shareInput.className = "tlv-minshare-input";
      shareInput.min = "0";
      shareInput.max = "100";
      shareInput.step = "0.1";
      shareInput.value = minSharePercent.toFixed(1);
      shareInput.addEventListener("change", function () {
        var v = parseFloat(shareInput.value);
        minSharePercent = isNaN(v) ? 0 : Math.max(0, Math.min(100, v));
        renderAll();
      });
      shareLabel.appendChild(shareInput);
      shareLabel.appendChild(document.createTextNode("% of run"));
      controlsEl.appendChild(shareLabel);
    }

    if (xDomain[0] !== fullDomain[0] || xDomain[1] !== fullDomain[1]) {
      var resetBtn = document.createElement("button");
      resetBtn.type = "button";
      resetBtn.textContent = "Reset zoom";
      resetBtn.addEventListener("click", function () {
        xDomain = fullDomain.slice();
        renderControls();
        renderAll();
      });
      controlsEl.appendChild(resetBtn);
    }
  }

  // ---------------------------------------------------------------------
  // Shared scale/crosshair plumbing
  // ---------------------------------------------------------------------

  function xScaleFor(width, margin) {
    var span = (xDomain[1] - xDomain[0]) || 1;
    var plotWidth = width - margin.left - margin.right;
    return function (t) { return margin.left + (t - xDomain[0]) / span * plotWidth; };
  }

  function clientXToTime(entry, margin, clientX) {
    var rect = entry.svg.getBoundingClientRect();
    var scaleX = rect.width ? (entry.width / rect.width) : 1;
    var svgX = (clientX - rect.left) * scaleX;
    var plotWidth = entry.width - margin.left - margin.right;
    return xDomain[0] + (svgX - margin.left) / (plotWidth || 1) * (xDomain[1] - xDomain[0]);
  }

  function ensureTooltip() {
    if (!tooltipEl) {
      tooltipEl = document.createElement("div");
      tooltipEl.className = "itv-tooltip";
      tooltipEl.style.display = "none";
      document.body.appendChild(tooltipEl);
    }
    return tooltipEl;
  }

  function hideTooltip() {
    if (tooltipEl) tooltipEl.style.display = "none";
  }

  function moveCrosshairTo(t) {
    chartEntries.forEach(function (e) {
      var x = e.xs(t);
      e.crosshair.setAttribute("x1", x);
      e.crosshair.setAttribute("x2", x);
      e.crosshair.style.display = "";
    });
  }

  function hideCrosshair() {
    chartEntries.forEach(function (e) { e.crosshair.style.display = "none"; });
    hideTooltip();
  }

  // ---------------------------------------------------------------------
  // Percentage chart (subset of interval_viewer.js's own chart, one instance)
  // ---------------------------------------------------------------------

  function nearestIndexForTime(t) {
    var times = intervalData.series.time;
    var lo = 0, hi = times.length - 1;
    if (!times.length) return -1;
    if (t <= times[0]) return 0;
    if (t >= times[hi]) return hi;
    while (lo < hi) {
      var mid = (lo + hi) >> 1;
      if (times[mid] < t) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && Math.abs(times[lo - 1] - t) < Math.abs(times[lo] - t)) return lo - 1;
    return lo;
  }

  function drawPctChart(container) {
    var width = container.clientWidth || 600;
    var height = PCT_HEIGHT;
    var svg = svgEl("svg", { class: "itv-chart-svg", viewBox: "0 0 " + width + " " + height });
    var xs = xScaleFor(width, MARGIN);
    var plotTop = MARGIN.top, plotBottom = height - MARGIN.bottom;

    function ys(v) { return plotBottom - v / 100 * (plotBottom - plotTop); }

    phaseBands.forEach(function (band) {
      if (band.end < xDomain[0] || band.start > xDomain[1]) return;
      var x0 = xs(Math.max(band.start, xDomain[0]));
      var x1 = xs(Math.min(band.end, xDomain[1]));
      svg.appendChild(svgEl("rect", {
        class: "itv-phase-" + band.phase, x: x0, y: plotTop,
        width: Math.max(1, x1 - x0), height: Math.max(0, plotBottom - plotTop),
      }));
    });

    for (var i = 0; i <= 4; i++) {
      var v = 25 * i, y = ys(v);
      svg.appendChild(svgEl("line", { class: "itv-gridline", x1: MARGIN.left, x2: width - MARGIN.right, y1: y, y2: y }));
      var label = svgEl("text", { class: "itv-axis-label", x: MARGIN.left - 6, y: y + 3, "text-anchor": "end" });
      label.textContent = v + "%";
      svg.appendChild(label);
    }
    svg.appendChild(svgEl("line", { class: "itv-axis-line", x1: MARGIN.left, x2: width - MARGIN.right, y1: plotBottom, y2: plotBottom }));

    var times = intervalData.series.time;
    pctColumns.forEach(function (col, i) {
      if (!visible[col]) return;
      var values = intervalData.series[col];
      var points = [];
      for (var idx = 0; idx < times.length; idx++) {
        var t = times[idx];
        if (t < xDomain[0] || t > xDomain[1]) continue;
        var v = values[idx];
        if (v === null || v === undefined) continue;
        points.push(xs(t) + "," + ys(v));
      }
      if (points.length < 2) return;
      svg.appendChild(svgEl("path", { class: "itv-series-line", d: "M" + points.join("L"), stroke: seriesColor(i) }));
    });

    var overlay = svgEl("rect", {
      x: MARGIN.left, y: plotTop, width: Math.max(0, width - MARGIN.left - MARGIN.right),
      height: Math.max(0, plotBottom - plotTop), fill: "transparent",
    });
    svg.appendChild(overlay);
    var crosshair = svgEl("line", { class: "itv-crosshair", x1: 0, x2: 0, y1: plotTop, y2: plotBottom });
    crosshair.style.display = "none";
    svg.appendChild(crosshair);
    var zoomBand = svgEl("rect", { class: "itv-zoom-band", x: 0, y: plotTop, width: 0, height: Math.max(0, plotBottom - plotTop) });
    zoomBand.style.display = "none";
    svg.appendChild(zoomBand);

    container.innerHTML = "";
    container.appendChild(svg);

    var entry = { svg: svg, xs: xs, width: width, height: height, crosshair: crosshair, zoomBand: zoomBand, margin: MARGIN };
    wireZoom(entry, MARGIN, function (t, clientX, clientY) {
      var idx = nearestIndexForTime(t);
      if (idx === -1) return;
      var actualT = intervalData.series.time[idx];
      moveCrosshairTo(actualT);
      var tip = ensureTooltip();
      tip.innerHTML = "";
      var timeRow = document.createElement("div");
      timeRow.className = "itv-tooltip-time";
      timeRow.textContent = "time = " + actualT;
      tip.appendChild(timeRow);
      pctColumns.forEach(function (col, i) {
        if (!visible[col]) return;
        var v = intervalData.series[col][idx];
        var row = document.createElement("div");
        row.className = "itv-tooltip-row";
        var key = document.createElement("span");
        key.className = "itv-swatch"; key.style.background = seriesColor(i);
        var val = document.createElement("span");
        val.className = "itv-tooltip-value";
        val.textContent = (v === null || v === undefined) ? String.fromCharCode(8212) : String(v);
        var name = document.createElement("span");
        name.className = "itv-tooltip-name"; name.textContent = col;
        row.appendChild(key); row.appendChild(val); row.appendChild(name);
        tip.appendChild(row);
      });
      tip.style.display = "";
      tip.style.left = (clientX + 14) + "px";
      tip.style.top = (clientY + 14) + "px";
    });
    return entry;
  }

  // ---------------------------------------------------------------------
  // Swimlane chart
  // ---------------------------------------------------------------------

  function formatSeconds(v) { return v.toFixed(3) + "s"; }

  function drawSwimlane(container) {
    computeLaneRows();

    var width = container.clientWidth || 600;
    // laneRows is filtered by the min-duration threshold only (relative to the whole run, so it
    // doesn't churn as the user zooms); further restrict to rows overlapping the *current* zoomed
    // xDomain so zooming in actually shrinks the row list to what's relevant, rather than leaving
    // every out-of-window row's label taking up vertical space with no visible bar.
    var rows = laneRows.filter(function (r) {
      return r.node.finish >= xDomain[0] && r.node.start <= xDomain[1];
    });
    var height = SWIM_MARGIN.top + SWIM_MARGIN.bottom + Math.max(1, rows.length) * ROW_HEIGHT;
    var svg = svgEl("svg", { class: "tlv-swimlane-svg", viewBox: "0 0 " + width + " " + height });
    var xs = xScaleFor(width, SWIM_MARGIN);

    if (!rows.length) {
      var msg = svgEl("text", { class: "tlv-lane-label-muted", x: SWIM_MARGIN.left, y: SWIM_MARGIN.top + 12 });
      msg.textContent = treeData.tree_omitted ? "tree too large to render" : "no processes at this threshold";
      svg.appendChild(msg);
    }

    rows.forEach(function (row, i) {
      var y = SWIM_MARGIN.top + i * ROW_HEIGHT;
      var node = row.node;
      var x0 = xs(Math.max(node.start, xDomain[0]));
      var x1 = xs(Math.min(node.finish, xDomain[1]));
      if (x1 > x0) {
        var barClass = (node.target_counters && node.target_counters.length) ? "tlv-bar tlv-bar-target" : "tlv-bar";
        svg.appendChild(svgEl("rect", {
          class: barClass, x: x0, y: y + 2, width: Math.max(1, x1 - x0), height: ROW_HEIGHT - 5,
          fill: row.color, "data-lane-index": i,
        }));
      }
      var indent = SWIM_MARGIN.left - 6 - Math.min(row.depth, 12) * 8;
      var label = svgEl("text", {
        class: "tlv-lane-label", x: Math.max(4, indent), y: y + ROW_HEIGHT - 6, "text-anchor": "end",
      });
      var name = (node.comm || "?") + " (" + node.pid + ")";
      label.textContent = name.length > 26 ? name.slice(0, 25) + "…" : name;
      svg.appendChild(label);
    });

    svg.appendChild(svgEl("line", {
      class: "itv-axis-line", x1: SWIM_MARGIN.left, x2: SWIM_MARGIN.left,
      y1: SWIM_MARGIN.top, y2: height - SWIM_MARGIN.bottom,
    }));

    var overlay = svgEl("rect", {
      x: SWIM_MARGIN.left, y: SWIM_MARGIN.top, width: Math.max(0, width - SWIM_MARGIN.left - SWIM_MARGIN.right),
      height: Math.max(0, height - SWIM_MARGIN.top - SWIM_MARGIN.bottom), fill: "transparent",
    });
    svg.appendChild(overlay);
    var crosshair = svgEl("line", { class: "itv-crosshair", x1: 0, x2: 0, y1: SWIM_MARGIN.top, y2: height - SWIM_MARGIN.bottom });
    crosshair.style.display = "none";
    svg.appendChild(crosshair);
    var zoomBand = svgEl("rect", { class: "itv-zoom-band", x: 0, y: SWIM_MARGIN.top, width: 0, height: Math.max(0, height - SWIM_MARGIN.top - SWIM_MARGIN.bottom) });
    zoomBand.style.display = "none";
    svg.appendChild(zoomBand);

    container.innerHTML = "";
    container.appendChild(svg);

    var entry = { svg: svg, xs: xs, width: width, height: height, crosshair: crosshair, zoomBand: zoomBand, margin: SWIM_MARGIN };
    wireZoom(entry, SWIM_MARGIN, function (t, clientX, clientY, clientYOffset) {
      moveCrosshairTo(t);
      var rect = entry.svg.getBoundingClientRect();
      var scaleY = rect.height ? (entry.height / rect.height) : 1;
      var svgY = (clientYOffset - rect.top) * scaleY;
      var rowIndex = Math.floor((svgY - SWIM_MARGIN.top) / ROW_HEIGHT);
      var row = rows[rowIndex];
      if (!row || t < row.node.start || t > row.node.finish) { hideTooltip(); return; }
      showLaneTooltip(row.node, clientX, clientY);
    });
    return entry;
  }

  function showLaneTooltip(node, clientX, clientY) {
    var tip = ensureTooltip();
    tip.innerHTML = "";
    var title = document.createElement("div");
    title.className = "itv-tooltip-time";
    title.textContent = (node.comm || "?") + " (pid " + node.pid + ", ppid " + node.ppid + ")";
    tip.appendChild(title);
    [
      ["start", formatSeconds(node.start)],
      ["finish", formatSeconds(node.finish)],
      ["duration", formatSeconds(node.finish - node.start)],
    ].forEach(function (pair) {
      var row = document.createElement("div");
      row.className = "itv-tooltip-row";
      var name = document.createElement("span"); name.className = "itv-tooltip-name"; name.textContent = pair[0];
      var val = document.createElement("span"); val.className = "itv-tooltip-value"; val.textContent = pair[1];
      row.appendChild(val); row.appendChild(name);
      tip.appendChild(row);
    });
    (node.target_counters || []).forEach(function (tc) {
      var row = document.createElement("div");
      row.className = "itv-tooltip-row";
      var name = document.createElement("span");
      name.className = "itv-tooltip-name"; name.textContent = tc.group + "." + tc.label;
      var val = document.createElement("span");
      val.className = "itv-tooltip-value"; val.textContent = String(tc.value);
      row.appendChild(val); row.appendChild(name);
      tip.appendChild(row);
    });
    tip.style.display = "";
    tip.style.left = (clientX + 14) + "px";
    tip.style.top = (clientY + 14) + "px";
  }

  // ---------------------------------------------------------------------
  // Shared zoom/hover wiring, generalized from interval_viewer.js's
  // wireInteractions() to take a per-chart hover callback (pct chart looks
  // up nearest interval row; swimlane looks up the bar under the pointer).
  // ---------------------------------------------------------------------

  function wireZoom(entry, margin, onHover) {
    var dragStartTime = null;
    entry.svg.addEventListener("mousemove", function (ev) {
      var t = clientXToTime(entry, margin, ev.clientX);
      if (dragStartTime !== null) {
        var x0 = entry.xs(dragStartTime), x1 = entry.xs(t);
        entry.zoomBand.setAttribute("x", Math.min(x0, x1));
        entry.zoomBand.setAttribute("width", Math.abs(x1 - x0));
        entry.zoomBand.style.display = "";
      } else {
        onHover(t, ev.clientX, ev.clientY, ev.clientY);
      }
    });
    entry.svg.addEventListener("mouseleave", function () {
      hideCrosshair();
      entry.zoomBand.style.display = "none";
      dragStartTime = null;
    });
    entry.svg.addEventListener("mousedown", function (ev) {
      dragStartTime = clientXToTime(entry, margin, ev.clientX);
    });
    entry.svg.addEventListener("mouseup", function (ev) {
      if (dragStartTime === null) return;
      var dragEndTime = clientXToTime(entry, margin, ev.clientX);
      entry.zoomBand.style.display = "none";
      var span = xDomain[1] - xDomain[0];
      if (Math.abs(dragEndTime - dragStartTime) > span * 0.01) {
        xDomain = [Math.min(dragStartTime, dragEndTime), Math.max(dragStartTime, dragEndTime)];
        dragStartTime = null;
        renderControls();
        renderAll();
        return;
      }
      dragStartTime = null;
    });
  }

  // ---------------------------------------------------------------------
  // Top-level render
  // ---------------------------------------------------------------------

  function addBlock(title, note) {
    var block = document.createElement("div");
    block.className = "itv-chart-block";
    var titleEl = document.createElement("p");
    titleEl.className = "itv-chart-title";
    titleEl.textContent = title;
    block.appendChild(titleEl);
    if (note) {
      var noteEl = document.createElement("p");
      noteEl.className = "tlv-swimlane-note";
      noteEl.textContent = note;
      block.appendChild(noteEl);
    }
    var container = document.createElement("div");
    block.appendChild(container);
    rootEl.appendChild(block);
    return container;
  }

  function renderAll() {
    rootEl.innerHTML = "";
    chartEntries = [];

    if (pctColumns.length) {
      var pctContainer = addBlock("Percentage metrics (0-100%)", null);
      chartEntries.push(drawPctChart(pctContainer));
    }

    var laneNote;
    if (treeData.tree_omitted) {
      laneNote = "process tree omitted (too large to render as a tree in the browser)";
    } else {
      computeLaneRows(); // recomputed again below by drawSwimlane(), but needed here for the count note
      laneNote = "showing " + laneRows.length + " of " + laneTotalCount + " processes" +
        (minSharePercent > 0 ? " (own duration ≥ " + minSharePercent.toFixed(1) + "% of run span)" : "") +
        (laneCapped ? " -- capped at the " + MAX_RENDERED_LANES + " longest-running; " +
          "raise the threshold above to narrow further" : "");
    }
    var swimContainer = addBlock("Process tree", laneNote);
    chartEntries.push(drawSwimlane(swimContainer));
  }
}());
