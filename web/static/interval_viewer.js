/*
 * interval_viewer.js -- item 4's interactive timeline viewer (INVESTIGATION.md
 * 4.3 Tier 3 item 4's timeline half; the tree half was already interactive,
 * shipped in 4.2 -- see proctree_viewer.js). Loaded only by /interval-viewer/...
 * (via window.ITV_CONFIG = {jsonUrl} set inline by server.py's
 * render_interval_viewer()) rather than folded into the shared app.js, same
 * "page-specific JS stays out of every other page's payload" reasoning
 * proctree_viewer.js's own docstring gives.
 *
 * Fetches one --interval CSV's parsed columns from jsonUrl (server.py's
 * /api/interval-json, which shells out to joblib.parse_interval_csv() -- no
 * subprocess, just the stdlib csv module) and renders it entirely
 * client-side: no charting library, same hand-rolled approach
 * proctree_viewer.js already established for this codebase's stdlib/no-CDN
 * web tooling.
 *
 * Deliberately two chart groups rather than one chart with a second y-axis:
 * a run's non-dimension columns span wildly different units (percentages,
 * raw counts, MB, watts, MHz) -- cramming them onto two y-scales in one plot
 * is the most common charting mistake there is (a viewer can't tell which
 * axis a line belongs to, and an unbounded metric visually flattens a
 * bounded one). Percentage-shaped columns (the topdown axes plus GPU
 * busy/activity -- all naturally 0-100%) share ONE 0-100% chart, since
 * they're genuinely scale-comparable; every other numeric column gets its
 * own small-multiple chart, auto-scaled to its own range. All charts share
 * one x-domain (time) and one phase-shaded background, so zooming or
 * hovering on any one of them stays aligned with the rest -- the actual
 * "GPU phase overlay" the backlog item asks for is GPU busy/activity simply
 * being one more line on the shared percentage chart, colored and toggled
 * like any other series, with phase read straight off the shared background
 * bands rather than needing its own separate treatment.
 *
 * Known limitation: hover/crosshair is pointer-only (no keyboard-driven
 * equivalent) -- proctree_viewer.js's own interactive tree has the same
 * gap. A real keyboard-navigable crosshair would be a substantial addition
 * on its own; noted here rather than silently skipped.
 */
(function () {
  "use strict";

  var cfg = window.ITV_CONFIG || {};
  var rootEl = document.getElementById("itv-root");
  var controlsEl = document.getElementById("itv-controls");

  if (!cfg.jsonUrl || !rootEl) {
    return;
  }

  var SVG_NS = "http://www.w3.org/2000/svg";
  var MARGIN = { top: 10, right: 16, bottom: 22, left: 46 };
  var MAIN_HEIGHT = 260;
  var SMALL_HEIGHT = 130;
  var SERIES_COLOR_SLOTS = 8; // --series-1..8 custom properties, style.css

  // A column not already in data.gpu_columns still counts as "percentage-shaped"
  // if it ends in "_pct" (most topdown-derived columns do) or is one of these
  // known 0-100-bounded topdown headline scores, which don't carry the _pct
  // suffix themselves.
  var KNOWN_PCT_COLUMNS = ["retire", "frontend", "backend", "speculate", "confidence", "sanity"];

  // Default visible set on the shared percentage chart -- kept small on purpose
  // (past ~4 converging lines a single chart stops being readable; every other
  // percentage column is still one checkbox away, not hidden).
  var DEFAULT_VISIBLE = ["retire", "frontend", "backend", "speculate"];

  var data = null;          // full parsed response from the API
  var pctColumns = [];      // ordered non-dimension columns sharing the 0-100% chart
  var otherColumns = [];    // remaining numeric columns, one small-multiple chart each
  var visible = {};         // pctColumns[i] -> bool, which are checked on
  var phaseBands = [];      // [{start, end, phase}], run-length-grouped from series.phase
  var fullDomain = [0, 1];  // [minTime, maxTime] across the whole (possibly decimated) series
  var xDomain = [0, 1];     // current, possibly zoomed, [minTime, maxTime]
  var chartInstances = [];  // {svg, xs, width, height, crosshair, zoomBand}, one per rendered chart
  var tooltipEl = null;

  function isDimension(col) {
    return data.dimension_columns.indexOf(col) !== -1;
  }

  function isPctColumn(col) {
    return col.slice(-4) === "_pct" ||
      KNOWN_PCT_COLUMNS.indexOf(col) !== -1 ||
      data.gpu_columns.indexOf(col) !== -1;
  }

  function seriesColor(index) {
    return "var(--series-" + ((index % SERIES_COLOR_SLOTS) + 1) + ")";
  }

  function svgEl(tag, attrs) {
    var el = document.createElementNS(SVG_NS, tag);
    for (var k in attrs) {
      if (Object.prototype.hasOwnProperty.call(attrs, k)) {
        el.setAttribute(k, attrs[k]);
      }
    }
    return el;
  }

  function debounce(fn, ms) {
    var timer = null;
    return function () {
      clearTimeout(timer);
      timer = setTimeout(fn, ms);
    };
  }

  // ---------------------------------------------------------------------
  // Data loading
  // ---------------------------------------------------------------------

  function fetchData() {
    fetch(cfg.jsonUrl)
      .then(function (r) {
        return r.json().then(function (body) { return { ok: r.ok, body: body }; });
      })
      .then(function (res) {
        if (!res.ok) {
          renderError((res.body && res.body.error) || "failed to load timeline data");
          return;
        }
        onData(res.body);
      })
      .catch(function (err) {
        renderError(err.message);
      });
  }

  function renderError(message) {
    rootEl.innerHTML = "";
    var p = document.createElement("p");
    p.className = "muted";
    p.textContent = "Could not load timeline: " + message;
    rootEl.appendChild(p);
  }

  function onData(body) {
    data = body;
    pctColumns = [];
    otherColumns = [];
    for (var i = 0; i < data.columns.length; i++) {
      var col = data.columns[i];
      if (isDimension(col)) continue;
      if (isPctColumn(col)) pctColumns.push(col); else otherColumns.push(col);
    }

    visible = {};
    var haveDefault = false;
    pctColumns.forEach(function (col) {
      var isDefault = DEFAULT_VISIBLE.indexOf(col) !== -1;
      if (isDefault) haveDefault = true;
      visible[col] = isDefault;
    });
    if (!haveDefault) {
      pctColumns.slice(0, 4).forEach(function (col) { visible[col] = true; });
    }
    // GPU utilization defaults on -- it's the point of this feature's "phase
    // overlay" half, not an opt-in extra.
    data.gpu_columns.forEach(function (col) {
      if (pctColumns.indexOf(col) !== -1) visible[col] = true;
    });

    phaseBands = computePhaseBands();
    var times = data.series.time || [];
    fullDomain = times.length ? [times[0], times[times.length - 1]] : [0, 1];
    if (fullDomain[0] === fullDomain[1]) fullDomain = [fullDomain[0], fullDomain[0] + 1];
    xDomain = fullDomain.slice();

    renderControls();
    renderCharts();
    window.addEventListener("resize", debounce(renderCharts, 150));
  }

  function computePhaseBands() {
    var phase = data.series.phase;
    var time = data.series.time;
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

  // ---------------------------------------------------------------------
  // Controls (legend/toggles, decimation note, reset-zoom)
  // ---------------------------------------------------------------------

  function renderControls() {
    controlsEl.innerHTML = "";

    if (data.decimated) {
      var note = document.createElement("p");
      note.className = "muted";
      note.textContent = "Showing " + data.series.time.length + " of " + data.row_count +
        " rows (decimated for display).";
      controlsEl.appendChild(note);
    }

    if (pctColumns.length) {
      var legend = document.createElement("div");
      legend.className = "itv-legend";
      pctColumns.forEach(function (col, i) {
        var label = document.createElement("label");
        var cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = !!visible[col];
        cb.addEventListener("change", function () {
          visible[col] = cb.checked;
          renderCharts();
        });
        var swatch = document.createElement("span");
        swatch.className = "itv-swatch";
        swatch.style.background = seriesColor(i);
        var text = document.createElement("span");
        text.textContent = col;
        label.appendChild(cb);
        label.appendChild(swatch);
        label.appendChild(text);
        legend.appendChild(label);
      });
      controlsEl.appendChild(legend);
    }

    if (xDomain[0] !== fullDomain[0] || xDomain[1] !== fullDomain[1]) {
      var resetBtn = document.createElement("button");
      resetBtn.type = "button";
      resetBtn.textContent = "Reset zoom";
      resetBtn.addEventListener("click", function () {
        xDomain = fullDomain.slice();
        renderControls();
        renderCharts();
      });
      controlsEl.appendChild(resetBtn);
    }
  }

  // ---------------------------------------------------------------------
  // Chart drawing
  // ---------------------------------------------------------------------

  function xScale(width) {
    var span = (xDomain[1] - xDomain[0]) || 1;
    var plotWidth = width - MARGIN.left - MARGIN.right;
    return function (t) {
      return MARGIN.left + (t - xDomain[0]) / span * plotWidth;
    };
  }

  function yScale(height, yMin, yMax) {
    var span = (yMax - yMin) || 1;
    var plotHeight = height - MARGIN.top - MARGIN.bottom;
    return function (v) {
      return height - MARGIN.bottom - (v - yMin) / span * plotHeight;
    };
  }

  function drawChart(container, opts) {
    var width = container.clientWidth || 600;
    var height = opts.height;
    var svg = svgEl("svg", { class: "itv-chart-svg", viewBox: "0 0 " + width + " " + height });
    var xs = xScale(width);
    var ys = yScale(height, opts.yMin, opts.yMax);
    var plotTop = MARGIN.top;
    var plotBottom = height - MARGIN.bottom;

    phaseBands.forEach(function (band) {
      if (band.end < xDomain[0] || band.start > xDomain[1]) return;
      var x0 = xs(Math.max(band.start, xDomain[0]));
      var x1 = xs(Math.min(band.end, xDomain[1]));
      svg.appendChild(svgEl("rect", {
        class: "itv-phase-" + band.phase, x: x0, y: plotTop,
        width: Math.max(1, x1 - x0), height: Math.max(0, plotBottom - plotTop),
      }));
    });

    var tickCount = 4;
    for (var i = 0; i <= tickCount; i++) {
      var v = opts.yMin + (opts.yMax - opts.yMin) * i / tickCount;
      var y = ys(v);
      svg.appendChild(svgEl("line", {
        class: "itv-gridline", x1: MARGIN.left, x2: width - MARGIN.right, y1: y, y2: y,
      }));
      var label = svgEl("text", {
        class: "itv-axis-label", x: MARGIN.left - 6, y: y + 3, "text-anchor": "end",
      });
      label.textContent = opts.yFormat ? opts.yFormat(v) : String(Math.round(v));
      svg.appendChild(label);
    }
    svg.appendChild(svgEl("line", {
      class: "itv-axis-line", x1: MARGIN.left, x2: width - MARGIN.right,
      y1: plotBottom, y2: plotBottom,
    }));

    var times = data.series.time;
    opts.series.forEach(function (s) {
      var values = data.series[s.name];
      var points = [];
      for (var idx = 0; idx < times.length; idx++) {
        var t = times[idx];
        if (t < xDomain[0] || t > xDomain[1]) continue;
        var v = values[idx];
        if (v === null || v === undefined) continue;
        points.push(xs(t) + "," + ys(v));
      }
      if (points.length < 2) return;
      svg.appendChild(svgEl("path", { class: "itv-series-line", d: "M" + points.join("L"), stroke: s.color }));
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

    return { svg: svg, xs: xs, width: width, height: height, crosshair: crosshair, zoomBand: zoomBand };
  }

  function addChartBlock(title, drawOpts) {
    var block = document.createElement("div");
    block.className = "itv-chart-block";
    var titleEl = document.createElement("p");
    titleEl.className = "itv-chart-title";
    titleEl.textContent = title;
    block.appendChild(titleEl);
    var container = document.createElement("div");
    block.appendChild(container);
    rootEl.appendChild(block);
    return drawChart(container, drawOpts);
  }

  function renderCharts() {
    rootEl.innerHTML = "";
    chartInstances = [];

    if (pctColumns.length) {
      var mainSeries = [];
      pctColumns.forEach(function (col, i) {
        if (visible[col]) mainSeries.push({ name: col, color: seriesColor(i) });
      });
      chartInstances.push(addChartBlock("Percentage metrics (0-100%)", {
        series: mainSeries, yMin: 0, yMax: 100, height: MAIN_HEIGHT,
        yFormat: function (v) { return Math.round(v) + "%"; },
      }));
    }

    otherColumns.forEach(function (col) {
      var raw = data.series[col];
      var finite = raw.filter(function (v) { return v !== null && v !== undefined; });
      var yMin = finite.length ? Math.min.apply(null, finite) : 0;
      var yMax = finite.length ? Math.max.apply(null, finite) : 1;
      if (yMin === yMax) yMax = yMin + 1;
      chartInstances.push(addChartBlock(col, {
        series: [{ name: col, color: seriesColor(0) }], yMin: yMin, yMax: yMax, height: SMALL_HEIGHT,
      }));
    });

    wireInteractions();
  }

  // ---------------------------------------------------------------------
  // Hover crosshair + tooltip, shared across every chart so a reader can
  // correlate a CPU metric, GPU utilization, and any small-multiple at the
  // same instant regardless of which chart the pointer is actually over.
  // ---------------------------------------------------------------------

  function ensureTooltip() {
    if (!tooltipEl) {
      tooltipEl = document.createElement("div");
      tooltipEl.className = "itv-tooltip";
      tooltipEl.style.display = "none";
      document.body.appendChild(tooltipEl);
    }
    return tooltipEl;
  }

  function nearestIndexForTime(t) {
    var times = data.series.time;
    var lo = 0, hi = times.length - 1;
    if (t <= times[0]) return 0;
    if (t >= times[hi]) return hi;
    while (lo < hi) {
      var mid = (lo + hi) >> 1;
      if (times[mid] < t) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && Math.abs(times[lo - 1] - t) < Math.abs(times[lo] - t)) return lo - 1;
    return lo;
  }

  function visibleTooltipSeries() {
    var series = [];
    pctColumns.forEach(function (col, i) {
      if (visible[col]) series.push({ name: col, color: seriesColor(i) });
    });
    otherColumns.forEach(function (col) {
      series.push({ name: col, color: seriesColor(0) });
    });
    return series;
  }

  function showCrosshairAt(t, clientX, clientY) {
    var idx = nearestIndexForTime(t);
    var actualT = data.series.time[idx];
    chartInstances.forEach(function (c) {
      var x = c.xs(actualT);
      c.crosshair.setAttribute("x1", x);
      c.crosshair.setAttribute("x2", x);
      c.crosshair.style.display = "";
    });

    var tip = ensureTooltip();
    tip.innerHTML = "";
    var timeRow = document.createElement("div");
    timeRow.className = "itv-tooltip-time";
    timeRow.textContent = "time = " + actualT;
    tip.appendChild(timeRow);
    visibleTooltipSeries().forEach(function (s) {
      var v = data.series[s.name][idx];
      var row = document.createElement("div");
      row.className = "itv-tooltip-row";
      var key = document.createElement("span");
      key.className = "itv-swatch";
      key.style.background = s.color;
      var val = document.createElement("span");
      val.className = "itv-tooltip-value";
      val.textContent = (v === null || v === undefined) ? String.fromCharCode(8212) : String(v);
      var name = document.createElement("span");
      name.className = "itv-tooltip-name";
      name.textContent = s.name;
      row.appendChild(key);
      row.appendChild(val);
      row.appendChild(name);
      tip.appendChild(row);
    });
    tip.style.display = "";
    tip.style.left = (clientX + 14) + "px";
    tip.style.top = (clientY + 14) + "px";
  }

  function hideCrosshair() {
    chartInstances.forEach(function (c) { c.crosshair.style.display = "none"; });
    if (tooltipEl) tooltipEl.style.display = "none";
  }

  // ---------------------------------------------------------------------
  // Click-drag zoom, per chart -- rescales the shared xDomain, so zooming
  // in on (say) the GPU small-multiple also rescales the percentage chart
  // and every other small-multiple to the same time window.
  // ---------------------------------------------------------------------

  function clientXToTime(c, clientX) {
    var rect = c.svg.getBoundingClientRect();
    var scaleX = rect.width ? (c.width / rect.width) : 1;
    var svgX = (clientX - rect.left) * scaleX;
    var plotWidth = c.width - MARGIN.left - MARGIN.right;
    return xDomain[0] + (svgX - MARGIN.left) / (plotWidth || 1) * (xDomain[1] - xDomain[0]);
  }

  function wireInteractions() {
    chartInstances.forEach(function (c) {
      var dragStartTime = null;

      c.svg.addEventListener("mousemove", function (ev) {
        var t = clientXToTime(c, ev.clientX);
        if (dragStartTime !== null) {
          var x0 = c.xs(dragStartTime);
          var x1 = c.xs(t);
          c.zoomBand.setAttribute("x", Math.min(x0, x1));
          c.zoomBand.setAttribute("width", Math.abs(x1 - x0));
          c.zoomBand.style.display = "";
        } else {
          showCrosshairAt(t, ev.clientX, ev.clientY);
        }
      });
      c.svg.addEventListener("mouseleave", function () {
        hideCrosshair();
        c.zoomBand.style.display = "none";
        dragStartTime = null;
      });
      c.svg.addEventListener("mousedown", function (ev) {
        dragStartTime = clientXToTime(c, ev.clientX);
      });
      c.svg.addEventListener("mouseup", function (ev) {
        if (dragStartTime === null) return;
        var dragEndTime = clientXToTime(c, ev.clientX);
        c.zoomBand.style.display = "none";
        var span = xDomain[1] - xDomain[0];
        if (Math.abs(dragEndTime - dragStartTime) > span * 0.01) {
          xDomain = [Math.min(dragStartTime, dragEndTime), Math.max(dragStartTime, dragEndTime)];
          dragStartTime = null;
          renderControls();
          renderCharts();
          return;
        }
        dragStartTime = null;
      });
    });
  }

  fetchData();
}());
