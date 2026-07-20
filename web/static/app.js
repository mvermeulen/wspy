(function () {
  "use strict";

  // Mirrors ALL_GROUPS' name list in web/server.py -- only the names are
  // duplicated here (for building checkbox ids); the actual flag-building
  // logic stays server-side (build_configuration_passes()) so the preview
  // endpoint is always the single source of truth for what will run.
  var GROUP_NAMES = ["ipc", "topdown", "topdown2", "topdown-frontend", "topdown-backend",
    "topdown-optlb", "branch", "cache1", "cache2", "cache3", "dcache", "icache",
    "tlb", "memory", "opcache", "software", "float"];

  function byId(id) { return document.getElementById(id); }
  function getChecked(id) { var el = byId(id); return !!(el && el.checked); }
  function getValue(id) { var el = byId(id); return el ? el.value.trim() : ""; }
  // Store-recorded text (workload command, filesystem paths, ...) is
  // user-influenced -- e.g. typed into the Run tab's workload field -- and
  // ends up rendered back into another viewer's page by the trace form
  // below, so it's escaped before ever touching innerHTML.
  function escapeHtml(s) {
    return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
  }
  function selectedGroups(prefix) {
    return GROUP_NAMES.filter(function (name) { return getChecked(prefix + "_" + name); });
  }

  // ---------------------------------------------------------------------
  // Tabs
  // ---------------------------------------------------------------------
  function wireTabs() {
    var buttons = document.querySelectorAll(".tab-btn");
    if (!buttons.length) return;
    buttons.forEach(function (btn) {
      btn.addEventListener("click", function () {
        buttons.forEach(function (b) { b.classList.remove("active"); });
        btn.classList.add("active");
        document.querySelectorAll(".tab-panel").forEach(function (p) { p.hidden = true; });
        var panel = byId("tab-" + btn.dataset.tab);
        if (panel) panel.hidden = false;
      });
    });
  }

  // ---------------------------------------------------------------------
  // Run tab: preset + configuration/option checklist (item 9)
  // ---------------------------------------------------------------------
  function buildChecklist() {
    return {
      tree: {
        enabled: getChecked("tree_enabled"),
        cmdline: getChecked("tree_cmdline"),
        open: getChecked("tree_open"),
        futex: getChecked("tree_futex"),
        io: getChecked("tree_io"),
        io_wait: getChecked("tree_io_wait"),
        schedstat: getChecked("tree_schedstat"),
        vmsize: getChecked("tree_vmsize"),
        connect: getChecked("tree_connect"),
        wait: getChecked("tree_wait"),
        poll: getChecked("tree_poll"),
        nanosleep: getChecked("tree_nanosleep"),
        software: getChecked("tree_software"),
        timeout_secs: getValue("tree_timeout"),
      },
      counters: {
        enabled: getChecked("counters_enabled"),
        groups: selectedGroups("counters"),
        interval_secs: getValue("counters_interval"),
        per_core: getChecked("counters_per_core"),
        rusage: getChecked("counters_rusage"),
        csv: getChecked("counters_csv"),
        power: getChecked("counters_power"),
      },
      system: {
        enabled: getChecked("system_enabled"),
        interval_secs: getValue("system_interval"),
        csv: getChecked("system_csv"),
        power: getChecked("system_power"),
      },
      gpu: {
        enabled: getChecked("gpu_enabled"),
        busy: getChecked("gpu_busy"),
        metrics: getChecked("gpu_metrics"),
        smi: getChecked("gpu_smi"),
        nvidia: getChecked("gpu_nvidia"),
        device: getValue("gpu_device"),
        interval_secs: getValue("gpu_interval"),
        csv: getChecked("gpu_csv"),
      },
      ibs: {
        enabled: getChecked("ibs_enabled"),
        profile: byId("ibs_profile") ? byId("ibs_profile").value : "basic",
        interval_secs: getValue("ibs_interval"),
        maxcnt: getValue("ibs_maxcnt"),
        ldlat: getValue("ibs_ldlat"),
        fetchlat: getValue("ibs_fetchlat"),
      },
    };
  }

  function buildToggles() {
    return {
      manifest: getChecked("toggle_manifest"),
      run_index: getChecked("toggle_run_index"),
      store_ingest: getChecked("toggle_store_ingest"),
    };
  }

  // Custom plots (item 12's wspy-plot --plot/--only-custom exposed in the
  // UI): a repeatable list of {name, columns} rows built up client-side,
  // independent of preset vs. custom mode above -- wspy-plot always runs
  // after any run, whichever launched it, so this section is never
  // disabled by updateModeUI() the way the checklist is.
  function addCustomPlotRow() {
    var container = byId("custom-plots-list");
    if (!container) return;
    var row = document.createElement("div");
    row.className = "row custom-plot-row";
    row.innerHTML =
      '<label>Plot name <input type="text" class="cp-name" placeholder="e.g. my-counters"></label>' +
      '<label>Columns (comma-separated CSV header names) ' +
      '<input type="text" class="cp-columns" placeholder="e.g. retire,ipc"></label>' +
      '<button type="button" class="cp-remove">remove</button>';
    row.querySelector(".cp-remove").addEventListener("click", function () {
      row.remove();
      schedulePreview();
    });
    container.appendChild(row);
  }

  function buildCustomPlots() {
    var rows = document.querySelectorAll("#custom-plots-list .custom-plot-row");
    var result = [];
    rows.forEach(function (row) {
      var name = row.querySelector(".cp-name").value.trim();
      var columns = row.querySelector(".cp-columns").value.split(",")
        .map(function (s) { return s.trim(); }).filter(Boolean);
      if (name || columns.length) result.push({ name: name, columns: columns });
    });
    return result;
  }

  // ---------------------------------------------------------------------
  // Run tab: CPU affinity card (item 20, INVESTIGATION.md's "Core/thread
  // affinity control") -- independent of preset vs. custom mode, same as
  // custom plots above, since --affinity applies to every pass alike.
  // ---------------------------------------------------------------------
  function getAffinitySpec() {
    var checked = document.querySelector('input[name="affinity_mode"]:checked');
    var mode = checked ? checked.value : "all";
    if (mode === "nosmt") return "nosmt";
    if (mode === "domain") {
      var sel = byId("affinity_domain_select");
      return sel && sel.value !== "" ? "domain=" + sel.value : "";
    }
    if (mode === "coretype") {
      var ctSel = byId("affinity_coretype_select");
      return ctSel && ctSel.value !== "" ? "coretype=" + ctSel.value : "";
    }
    if (mode === "cpuset") {
      var boxes = document.querySelectorAll("#affinity-cpu-checkboxes input:checked");
      var ids = Array.prototype.map.call(boxes, function (b) { return b.value; });
      return ids.length ? "cpuset=" + ids.join(",") : "";
    }
    return ""; // "all" -- the implicit default, omitted from every request body
  }

  function renderAffinityTopology(topology) {
    var domainSelect = byId("affinity_domain_select");
    var coretypeSelect = byId("affinity_coretype_select");
    var cpuContainer = byId("affinity-cpu-checkboxes");
    if (!domainSelect || !cpuContainer) return;
    domainSelect.innerHTML = "";
    (topology.domains || []).forEach(function (d) {
      var opt = document.createElement("option");
      opt.value = d.id;
      opt.textContent = "Domain " + d.id + " — " + d.size_mib.toFixed(1) +
        " MiB L3 (cpus " + d.cpus + ")";
      domainSelect.appendChild(opt);
    });
    if (coretypeSelect) {
      coretypeSelect.innerHTML = "";
      (topology.core_types || []).forEach(function (t) {
        var opt = document.createElement("option");
        opt.value = t.id;
        opt.textContent = "Core type " + t.id + " — implementer " + t.implementer +
          " part " + t.part + " (cpus " + t.cpus + ")";
        coretypeSelect.appendChild(opt);
      });
    }
    cpuContainer.innerHTML = "";
    (topology.cpus || []).forEach(function (c) {
      var label = document.createElement("label");
      label.className = "inline-check";
      var input = document.createElement("input");
      input.type = "checkbox";
      input.value = String(c.id);
      input.addEventListener("change", schedulePreview);
      label.appendChild(input);
      label.appendChild(document.createTextNode(
        " cpu" + c.id + (c.primary_thread ? "" : " (SMT)") + " [domain " + c.l3_domain +
        (c.core_type >= 0 ? ", type " + c.core_type : "") + "]"));
      cpuContainer.appendChild(label);
    });
  }

  function wireAffinityCard() {
    var discoverBtn = byId("affinity-discover");
    var status = byId("affinity-discover-status");
    var domainPicker = byId("affinity-domain-picker");
    var coretypePicker = byId("affinity-coretype-picker");
    var cpusetPicker = byId("affinity-cpuset-picker");
    var radios = document.querySelectorAll('input[name="affinity_mode"]');
    if (!radios.length) return;

    function updatePickers() {
      var checked = document.querySelector('input[name="affinity_mode"]:checked');
      var mode = checked ? checked.value : "all";
      if (domainPicker) domainPicker.hidden = mode !== "domain";
      if (coretypePicker) coretypePicker.hidden = mode !== "coretype";
      if (cpusetPicker) cpusetPicker.hidden = mode !== "cpuset";
    }
    radios.forEach(function (r) {
      r.addEventListener("change", function () { updatePickers(); schedulePreview(); });
    });
    updatePickers();

    if (discoverBtn) {
      discoverBtn.addEventListener("click", function () {
        if (status) status.textContent = "discovering...";
        fetch("/api/discovery/affinity-topology", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: "{}",
        })
          .then(function (r) { return r.json(); })
          .then(function (data) {
            if (!data.topology) {
              if (status) status.textContent = "no topology data -- is wspy built? see Discovery tab for details.";
              return;
            }
            renderAffinityTopology(data.topology);
            if (status) {
              status.textContent = data.topology.cpus.length + " cpu(s), " +
                data.topology.domains.length + " L3 domain(s), " +
                (data.topology.core_types || []).length + " core type(s) discovered.";
            }
            schedulePreview();
          })
          .catch(function () { if (status) status.textContent = "discovery failed (transient?)"; });
      });
    }
  }

  function checklistInputs() {
    return document.querySelectorAll(
      "#checklist .config-card:not(.config-reserved) input, " +
      "#checklist .config-card:not(.config-reserved) select"
    );
  }

  function updateModeUI() {
    var presetEl = byId("preset");
    var indicator = byId("mode-indicator");
    var checklistEl = byId("checklist");
    if (!presetEl || !indicator || !checklistEl) return;
    var preset = presetEl.value;
    if (preset) {
      checklistEl.classList.add("checklist-disabled");
      checklistInputs().forEach(function (el) { el.disabled = true; });
      indicator.textContent = "Preset: " + preset + " → runs via wspy-run; the checklist below is ignored.";
      indicator.className = "mode-indicator mode-preset";
    } else {
      checklistEl.classList.remove("checklist-disabled");
      checklistInputs().forEach(function (el) { el.disabled = false; });
      indicator.textContent = "Custom: composes whatever's checked below into separate wspy command lines.";
      indicator.className = "mode-indicator mode-custom";
    }
  }

  var previewTimer = null;
  function schedulePreview() {
    clearTimeout(previewTimer);
    previewTimer = setTimeout(refreshPreview, 150);
  }

  // Reflects the server's autofit_checklist_for_custom_plots() result back
  // into the actual checkboxes/fields -- so "auto-enabled the dcache group"
  // isn't just a sentence in the notes area while the checkbox still looks
  // unchecked. Setting .checked/.value directly (not via a synthesized
  // "change" event) doesn't re-trigger schedulePreview(), so this can't
  // loop against the very preview request that produced it.
  function applyResolvedChecklist(resolved) {
    if (!resolved) return;
    var counters = resolved.counters || {};
    if (counters.enabled) {
      var ce = byId("counters_enabled");
      if (ce) ce.checked = true;
    }
    (counters.groups || []).forEach(function (name) {
      var el = byId("counters_" + name);
      if (el) el.checked = true;
    });
    if (counters.interval_secs) {
      var ci = byId("counters_interval");
      if (ci && !ci.value) ci.value = counters.interval_secs;
    }
    if (counters.power) {
      var cp = byId("counters_power");
      if (cp) cp.checked = true;
    }
    var system = resolved.system || {};
    if (system.enabled) {
      var se = byId("system_enabled");
      if (se) se.checked = true;
    }
    if (system.interval_secs) {
      var si = byId("system_interval");
      if (si && !si.value) si.value = system.interval_secs;
    }
    if (system.power) {
      var sp = byId("system_power");
      if (sp) sp.checked = true;
    }
  }

  function refreshPreview() {
    var pre = byId("preview");
    var notesEl = byId("preview-notes");
    if (!pre) return;
    var preset = getValue("preset");
    var body = {
      workload: getValue("workload"),
      suite: getValue("suite"),
      benchmark: getValue("benchmark"),
      run_id: getValue("run_id"),
      preset: preset,
      checklist: buildChecklist(),
      toggles: buildToggles(),
      custom_plots: buildCustomPlots(),
      only_custom: getChecked("only_custom"),
      affinity: getAffinitySpec(),
    };
    fetch("/api/preview", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    })
      .then(function (r) { return r.json(); })
      .then(function (data) {
        if (data.error) {
          pre.textContent = "Error: " + data.error;
          if (notesEl) notesEl.textContent = "";
          return;
        }
        var lines = data.lines || [];
        pre.textContent = lines.length ? lines.map(function (l) { return "$ " + l; }).join("\n")
          : "(nothing will run yet)";
        if (notesEl) notesEl.textContent = (data.notes || []).join(" ");
        applyResolvedChecklist(data.resolved_checklist);
      })
      .catch(function () { /* transient -- next keystroke will retry */ });
  }

  function wireRunTab() {
    var form = byId("run-form");
    if (!form) return;
    var presetEl = byId("preset");
    var liveOutput = byId("live-output");
    var runButton = byId("run-button");
    var runResult = byId("run-result");
    var addPlotBtn = byId("add-custom-plot");

    form.addEventListener("input", schedulePreview);
    form.addEventListener("change", schedulePreview);
    if (presetEl) {
      presetEl.addEventListener("change", function () {
        updateModeUI();
        schedulePreview();
      });
    }
    if (addPlotBtn) {
      addPlotBtn.addEventListener("click", function () {
        addCustomPlotRow();
        schedulePreview();
      });
    }
    wireAffinityCard();
    updateModeUI();
    refreshPreview();

    form.addEventListener("submit", function (ev) {
      ev.preventDefault();
      var preset = getValue("preset");
      runButton.disabled = true;
      runResult.textContent = "";

      if (getChecked("queue_job")) {
        // Item 13: queue instead of run -- one endpoint handles both preset
        // and custom mode (like /api/preview does), since a job file is
        // just the same state captured instead of executed.
        liveOutput.hidden = true;
        var jobBody = {
          workload: getValue("workload"),
          suite: getValue("suite"),
          benchmark: getValue("benchmark"),
          run_id: getValue("run_id"),
          preset: preset,
          checklist: buildChecklist(),
          toggles: buildToggles(),
          custom_plots: buildCustomPlots(),
          only_custom: getChecked("only_custom"),
          affinity: getAffinitySpec(),
        };
        fetch("/api/enqueue-job", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(jobBody),
        })
          .then(function (resp) {
            return resp.json().then(function (data) {
              if (!resp.ok) throw new Error(data.error || ("HTTP " + resp.status));
              return data;
            });
          })
          .then(function (data) {
            runButton.disabled = false;
            runResult.textContent = "Queued as " + data.job_id + ". " + data.message;
          })
          .catch(function (err) {
            runButton.disabled = false;
            runResult.textContent = "Error: " + err.message;
          });
        return;
      }

      liveOutput.hidden = false;
      liveOutput.textContent = "";

      var endpoint, body;
      if (preset) {
        endpoint = "/api/run-profile";
        body = {
          profile: preset,
          workload: getValue("workload"),
          suite: getValue("suite"),
          benchmark: getValue("benchmark"),
          run_id: getValue("run_id"),
          toggles: buildToggles(),
          custom_plots: buildCustomPlots(),
          only_custom: getChecked("only_custom"),
          affinity: getAffinitySpec(),
        };
      } else {
        endpoint = "/api/run-custom";
        body = {
          workload: getValue("workload"),
          suite: getValue("suite"),
          benchmark: getValue("benchmark"),
          run_id: getValue("run_id"),
          checklist: buildChecklist(),
          toggles: buildToggles(),
          custom_plots: buildCustomPlots(),
          only_custom: getChecked("only_custom"),
          affinity: getAffinitySpec(),
        };
      }

      fetch(endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      })
        .then(function (resp) {
          return resp.json().then(function (data) {
            if (!resp.ok) throw new Error(data.error || ("HTTP " + resp.status));
            return data;
          });
        })
        .then(function (data) {
          if (byId("run_id")) byId("run_id").value = data.run_id;
          var es = new EventSource(data.events_url);
          es.addEventListener("log", function (ev) {
            var line = JSON.parse(ev.data);
            liveOutput.textContent += line + "\n";
            liveOutput.scrollTop = liveOutput.scrollHeight;
          });
          es.addEventListener("done", function (ev) {
            var payload = JSON.parse(ev.data);
            es.close();
            runButton.disabled = false;
            if (payload.status === "done") {
              runResult.innerHTML = 'Done. <a href="' + payload.report_url + '">View report</a>.';
            } else {
              runResult.innerHTML = 'Run finished with errors -- see output above. ' +
                '<a href="' + payload.report_url + '">View report</a> for whatever was produced.';
            }
          });
          es.onerror = function () {
            runButton.disabled = false;
          };
        })
        .catch(function (err) {
          runButton.disabled = false;
          runResult.textContent = "Error: " + err.message;
        });
    });
  }

  // ---------------------------------------------------------------------
  // Run tab "Check" button (item 18): a synchronous, read-only look at
  // perf-counter-access sysctls and -- for a phoronix-test-suite workload --
  // that test's install status and an estimated/measured single-run time.
  // Never launches anything, so it's plain fetch+render like the Discovery
  // family below, just triggered from the Run tab next to the Run button.
  // ---------------------------------------------------------------------
  function statusClass(status) {
    return status === "ok" ? "status-ok" : status === "warn" ? "status-warn" : "status-unknown";
  }

  function renderPerfCheck(perf) {
    var labels = { perf_event_paranoid: "perf_event_paranoid", nmi_watchdog: "nmi_watchdog" };
    var html = '<div class="check-section"><strong>Perf counter access</strong>';
    ["perf_event_paranoid", "nmi_watchdog"].forEach(function (key) {
      var p = perf[key];
      if (!p) return;
      var valueText = (p.value === null || p.value === undefined) ? "unknown" : String(p.value);
      html += '<div class="' + statusClass(p.status) + '">' + escapeHtml(labels[key]) + " = "
        + escapeHtml(valueText) + " (" + escapeHtml(p.status) + ")</div>";
      if (p.detail) html += '<div class="muted">' + escapeHtml(p.detail) + "</div>";
    });
    html += "</div>";
    return html;
  }

  function renderPhoronixTest(t) {
    var html = '<div class="check-test"><code>' + escapeHtml(t.command || "") + "</code>";
    if (t.error) {
      html += '<div class="status-warn">' + escapeHtml(t.error) + "</div>";
    } else {
      if (t.queried_name) {
        html += '<div class="muted">"' + escapeHtml(t.name) + '" is a build-suite subset -- '
          + "querying full test \"" + escapeHtml(t.queried_name) + "\" for its estimate</div>";
      }
      html += "<div>" + escapeHtml(t.name) + ": installed=" + escapeHtml(t.installed || "unknown")
        + ", times run=" + escapeHtml(t.times_run || "0")
        + (t.last_run ? ", last run=" + escapeHtml(t.last_run) : "") + "</div>";
      var est = t.estimate || {};
      html += "<div><strong>" + escapeHtml(est.text || "no estimate available") + "</strong>"
        + (est.source ? " (" + escapeHtml(est.source) + ")" : "") + "</div>";
      if (est.detail) html += '<div class="muted">' + escapeHtml(est.detail) + "</div>";
    }
    html += "</div>";
    return html;
  }

  function renderCounterProbe(p) {
    var html = '<div class="check-test"><code>' + escapeHtml(p.command || "") + "</code>";
    html += '<div class="' + statusClass(p.status) + '">' + escapeHtml(p.profile) + ": "
      + escapeHtml(p.status) + "</div>";
    if (p.detail) html += '<div class="muted">' + escapeHtml(p.detail) + "</div>";
    html += "</div>";
    return html;
  }

  function renderGpuBuildProbe(p) {
    var html = '<div class="check-test"><code>' + escapeHtml(p.command || "") + "</code>";
    html += '<div class="' + statusClass(p.status) + '">' + escapeHtml(p.flag) + ": "
      + escapeHtml(p.status) + "</div>";
    if (p.detail) html += '<div class="muted">' + escapeHtml(p.detail) + "</div>";
    html += "</div>";
    return html;
  }

  function renderToolsCheck(tools) {
    var g = (tools || {}).gnuplot;
    if (!g) return "";
    var html = '<div class="check-section"><strong>Tooling</strong>';
    html += '<div class="' + statusClass(g.status) + '">gnuplot: ' + escapeHtml(g.status) + "</div>";
    if (g.detail) html += '<div class="muted">' + escapeHtml(g.detail) + "</div>";
    html += "</div>";
    return html;
  }

  function renderPhoronixBatchMode(bm) {
    if (!bm) return "";
    var html = '<div class="check-test"><strong>Batch-mode config</strong> '
      + '<span class="muted">(' + escapeHtml(bm.path) + ")</span>";
    html += '<div class="' + statusClass(bm.status) + '">' + escapeHtml(bm.status) + "</div>";
    if (bm.detail) html += '<div class="muted">' + escapeHtml(bm.detail) + "</div>";
    html += "</div>";
    return html;
  }

  function renderPhoronixResultNotifier(rn) {
    // Only present at all when result_notifier's hooks are actually
    // registered on this host (server.py's phoronix_result_notifier_
    // hooks_registered()) -- otherwise the buggy code path this checks for
    // can never be reached, and showing it would just be alarming noise.
    if (!rn) return "";
    var html = '<div class="check-test"><strong>result_notifier.php hook bug check</strong> '
      + '<span class="muted">(' + escapeHtml(rn.path) + ")</span>";
    html += '<div class="' + statusClass(rn.status) + '">' + escapeHtml(rn.status) + "</div>";
    if (rn.detail) html += '<div class="muted">' + escapeHtml(rn.detail) + "</div>";
    html += "</div>";
    return html;
  }

  function renderCheckResults(data) {
    var html = renderPerfCheck(data.perf || {});
    html += renderToolsCheck(data.tools);
    if (data.ibs && data.ibs.length) {
      html += '<div class="check-section"><strong>AMD IBS probe</strong> '
        + '<span class="muted">(actually opens the counter(s) against a trivial workload -- '
        + "sysfs presence alone can't catch a runtime perf_event_open() failure)</span>";
      data.ibs.forEach(function (p) { html += renderCounterProbe(p); });
      html += "</div>";
    }
    if (data.power && data.power.length) {
      html += '<div class="check-section"><strong>CPU power probe</strong> '
        + '<span class="muted">(actually opens pkg_joules against a trivial workload -- RAPL/'
        + "power access needs root or CAP_PERFMON, stricter than perf_event_paranoid alone, "
        + "which --capabilities' sysfs-only discovery can't catch)</span>";
      data.power.forEach(function (p) { html += renderCounterProbe(p); });
      html += "</div>";
    }
    if (data.gpu_build && data.gpu_build.length) {
      html += '<div class="check-section"><strong>GPU build check</strong> '
        + '<span class="muted">(is wspy actually linked with AMDGPU=1/NVIDIA=1 for the GPU flags '
        + "this request uses -- a GPU flag on a build without it silently no-ops instead of "
        + "failing, so this only catches that up front, not hardware/driver availability)</span>";
      data.gpu_build.forEach(function (p) { html += renderGpuBuildProbe(p); });
      html += "</div>";
    }
    var ph = data.phoronix;
    html += '<div class="check-section"><strong>Runtime estimate</strong>';
    if (!ph || !ph.detected) {
      html += '<div class="muted">' + escapeHtml((ph && ph.note) || "no estimate available") + "</div>";
    } else {
      html += renderPhoronixBatchMode(ph.batch_mode);
      html += renderPhoronixResultNotifier(ph.result_notifier);
      (ph.tests || []).forEach(function (t) { html += renderPhoronixTest(t); });
      if (ph.total_seconds !== null && ph.total_seconds !== undefined && (ph.tests || []).length > 1) {
        html += "<div><strong>Total estimated: " + Math.round(ph.total_seconds) + "s</strong></div>";
      }
      if (ph.truncated) {
        html += '<div class="muted">only the first ' + (ph.tests || []).length
          + " test(s) shown; the command lists more</div>";
      }
    }
    html += "</div>";
    return html;
  }

  function wireCheckButton() {
    var btn = byId("check-button");
    var outputEl = byId("check-results");
    if (!btn || !outputEl) return;
    btn.addEventListener("click", function () {
      btn.disabled = true;
      outputEl.hidden = false;
      outputEl.textContent = "checking…";
      fetch("/api/check-run", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          workload: getValue("workload"),
          preset: getValue("preset"),
          checklist: buildChecklist(),
        }),
      })
        .then(function (r) { return r.json(); })
        .then(function (data) {
          btn.disabled = false;
          if (data.error) {
            outputEl.textContent = "Error: " + data.error;
            return;
          }
          outputEl.innerHTML = renderCheckResults(data);
        })
        .catch(function (err) {
          btn.disabled = false;
          outputEl.textContent = "Error: " + err.message;
        });
    });
  }

  // ---------------------------------------------------------------------
  // Validate / Store & Summary / Discovery tabs: synchronous request/render,
  // no SSE needed since none of these launch a workload (see run_sync() in
  // server.py).
  // ---------------------------------------------------------------------
  function runSyncEndpoint(opts) {
    var btn = byId(opts.buttonId);
    var outputEl = byId(opts.outputId);
    if (!btn || !outputEl) return;
    var cmdEl = opts.cmdlineId ? byId(opts.cmdlineId) : null;
    btn.addEventListener("click", function () {
      var body;
      try {
        body = opts.buildBody();
      } catch (e) {
        outputEl.hidden = false;
        outputEl.textContent = "Error: " + e.message;
        return;
      }
      btn.disabled = true;
      outputEl.hidden = false;
      outputEl.textContent = "running…";
      if (cmdEl) cmdEl.hidden = true;
      fetch(opts.endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      })
        .then(function (r) { return r.json(); })
        .then(function (data) {
          btn.disabled = false;
          if (data.error) {
            outputEl.textContent = "Error: " + data.error;
            return;
          }
          if (cmdEl && data.command) {
            cmdEl.hidden = false;
            cmdEl.textContent = "$ " + data.command;
          }
          var text = data.output || "(no output)";
          if (data.timed_out) text += "\n[timed out]";
          text += "\n[exit " + (data.exit_code === null ? "?" : data.exit_code) + "]";
          outputEl.textContent = text;
        })
        .catch(function (err) {
          btn.disabled = false;
          outputEl.textContent = "Error: " + err.message;
        });
    });
  }

  function wireValidateTab() {
    document.querySelectorAll(".add-manifest-chip").forEach(function (chip) {
      chip.addEventListener("click", function () {
        var ta = byId("validate-paths");
        if (!ta) return;
        var val = ta.value;
        ta.value = (val ? val.replace(/\n+$/, "") + "\n" : "") + chip.dataset.path;
      });
    });
    runSyncEndpoint({
      buttonId: "validate-run",
      outputId: "validate-output",
      cmdlineId: "validate-cmdline",
      endpoint: "/api/discovery/validate",
      buildBody: function () {
        var paths = (getValue("validate-paths") || "").split("\n")
          .map(function (s) { return s.trim(); }).filter(Boolean);
        return { paths: paths, strict: getChecked("validate-strict"), quiet: getChecked("validate-quiet") };
      },
    });
  }

  function wireStoreTab() {
    runSyncEndpoint({
      buttonId: "store-ingest-run",
      outputId: "store-ingest-output",
      cmdlineId: "store-ingest-cmdline",
      endpoint: "/api/discovery/store-ingest",
      buildBody: function () {
        var runIndex = (getValue("store-run-index") || "").split("\n")
          .map(function (s) { return s.trim(); }).filter(Boolean);
        return {
          db: getValue("store-db"),
          run_index: runIndex,
          no_manifest_enrich: getChecked("store-no-manifest-enrich"),
          no_metrics_ingest: getChecked("store-no-metrics-ingest"),
          strict: getChecked("store-strict"),
        };
      },
    });
    runSyncEndpoint({
      buttonId: "summary-run",
      outputId: "summary-output",
      cmdlineId: "summary-cmdline",
      endpoint: "/api/discovery/summary",
      buildBody: function () {
        var metrics = (getValue("summary-metrics") || "").split(",")
          .map(function (s) { return s.trim(); }).filter(Boolean);
        return {
          db: getValue("summary-db"),
          command: getValue("summary-command"),
          hostname: getValue("summary-hostname"),
          metrics: metrics,
          group_by: byId("summary-group-by") ? byId("summary-group-by").value : "command",
          outlier_stddev: getValue("summary-outlier"),
          min_runs: getValue("summary-min-runs"),
          csv: getChecked("summary-csv"),
          show_runs: getChecked("summary-show-runs"),
          strict: getChecked("summary-strict"),
        };
      },
    });
    wireTraceForm();
  }

  // Item 14 "Traceability links": resolves a hostname:run_id (as printed by
  // the summary query's "show contributing runs" column) to its manifest/
  // raw CSV/tree/plots artifact chain. Rendered as real links (not a plain
  // <pre> dump like the other Discovery-family endpoints above) whenever
  // the resolved paths live under this server's own output-root, per
  // _resolve_trace_links() in server.py -- otherwise falls back to showing
  // the bare filesystem path text.
  function wireTraceForm() {
    var btn = byId("trace-run");
    var outputEl = byId("trace-output");
    var cmdEl = byId("trace-cmdline");
    if (!btn || !outputEl) return;

    // {display label, fields.*_exists key, fields.*_path key, links.*_url
    // key} -- one row per artifact, driven by this table rather than three
    // separate hand-typed calls below so a copy/paste slip (e.g. the wrong
    // *_exists key for a given *_path) can't silently cross-wire two rows.
    var ARTIFACT_ROWS = [
      { label: "Manifest", existsField: "manifest_exists", pathField: "manifest_path", urlField: "manifest_url" },
      { label: "Raw CSV", existsField: "output_exists", pathField: "output_path", urlField: "output_url" },
      { label: "Tree artifact", existsField: "tree_exists", pathField: "tree_output_path", urlField: "tree_url" },
    ];

    function artifactRow(row, fields, links) {
      var exists = fields[row.existsField] === "1";
      var path = fields[row.pathField] || "";
      if (!path) return "<div>" + row.label + ": <span class=\"muted\">not recorded</span></div>";
      // links.* is server-built from already-validated filesystem segments
      // (valid_segment()/valid_relpath() in web/joblib.py), so it can't
      // contain quote/angle-bracket characters -- escaped anyway as
      // defense in depth against that assumption ever loosening.
      var escapedPath = escapeHtml(path);
      var target = links[row.urlField]
        ? "<a href=\"" + escapeHtml(links[row.urlField]) + "\" target=\"_blank\">" + escapedPath + "</a>"
        : escapedPath;
      var status = exists ? "" : " <span class=\"muted\">(missing on this machine)</span>";
      return "<div>" + row.label + ": " + target + status + "</div>";
    }

    btn.addEventListener("click", function () {
      var db = getValue("trace-db");
      var key = getValue("trace-key");
      if (!key || key.indexOf(":") < 0) {
        outputEl.hidden = false;
        outputEl.textContent = "Error: expected hostname:run_id";
        return;
      }
      btn.disabled = true;
      outputEl.hidden = false;
      outputEl.textContent = "running…";
      if (cmdEl) cmdEl.hidden = true;
      fetch("/api/discovery/trace", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ db: db, key: key }),
      })
        .then(function (r) { return r.json(); })
        .then(function (data) {
          btn.disabled = false;
          if (data.error) {
            outputEl.textContent = "Error: " + data.error;
            return;
          }
          if (cmdEl && data.command) {
            cmdEl.hidden = false;
            cmdEl.textContent = "$ " + data.command;
          }
          if (!data.found) {
            outputEl.textContent = data.output || "no such run in this store";
            return;
          }
          var fields = data.fields || {};
          var links = data.links || {};
          var html = "";
          html += "<div><strong>" + escapeHtml(fields.command || "") + "</strong>"
            + " (" + escapeHtml(fields.hostname || "") + ":" + escapeHtml(fields.run_id || "") + ", "
            + escapeHtml(fields.start_time || "") + ")</div>";
          if (links.report_url) {
            html += "<div><a href=\"" + escapeHtml(links.report_url) + "\" target=\"_blank\">open report page</a></div>";
          }
          ARTIFACT_ROWS.forEach(function (row) { html += artifactRow(row, fields, links); });
          var plotsCount = fields.plots_count || "0";
          html += "<div>Plots: " + escapeHtml(plotsCount) + " PNG(s)"
            + (links.report_url ? " (see report page)" : "") + "</div>";
          outputEl.innerHTML = html;
        })
        .catch(function (err) {
          btn.disabled = false;
          outputEl.textContent = "Error: " + err.message;
        });
    });
  }

  // ---------------------------------------------------------------------
  // Report page: "AI narrative analysis" button (wspy-analyze / Ollama, see
  // render_analyze_card() in server.py). Model discovery is a quick,
  // bounded Ollama call, so it uses a plain fetch+render like the Discovery
  // tab; the analysis run itself can take minutes against a real model, so
  // it streams over SSE the same way a launched workload's live log does
  // (wireRunTab() above), not runSyncEndpoint()'s single blocking fetch.
  // ---------------------------------------------------------------------
  function wireAnalyzeForm() {
    var runButton = byId("analyze-run");
    var logEl = byId("analyze-log");
    var resultEl = byId("analyze-result");
    if (!runButton || !logEl || !resultEl) return;

    var discoverButton = byId("analyze-discover-models");
    var chipsEl = byId("analyze-model-chips");
    if (discoverButton && chipsEl) {
      discoverButton.addEventListener("click", function () {
        discoverButton.disabled = true;
        chipsEl.textContent = "discovering…";
        fetch("/api/discovery/ollama-models", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({}),
        })
          .then(function (r) { return r.json(); })
          .then(function (data) {
            discoverButton.disabled = false;
            if (data.error || !data.models || !data.models.length) {
              chipsEl.textContent = data.error ||
                "no models found -- is Ollama running, and does it have any models pulled?";
              return;
            }
            chipsEl.innerHTML = data.models.map(function (m) {
              return '<button type="button" class="add-model-chip" data-model="'
                + escapeHtml(m) + '">+ ' + escapeHtml(m) + "</button>";
            }).join("");
            chipsEl.querySelectorAll(".add-model-chip").forEach(function (chip) {
              chip.addEventListener("click", function () {
                var input = byId("analyze-models");
                if (!input) return;
                var names = input.value.split(",").map(function (s) { return s.trim(); }).filter(Boolean);
                if (names.indexOf(chip.dataset.model) === -1) names.push(chip.dataset.model);
                input.value = names.join(", ");
              });
            });
          })
          .catch(function (err) {
            discoverButton.disabled = false;
            chipsEl.textContent = "Error: " + err.message;
          });
      });
    }

    runButton.addEventListener("click", function () {
      var allModels = getChecked("analyze-all-models");
      var models = (getValue("analyze-models") || "").split(",")
        .map(function (s) { return s.trim(); }).filter(Boolean);
      var template = getValue("analyze-template");
      // No client-side "at least one model" gate: leaving both blank is a
      // valid choice now (wspy-analyze's own --default-model fallback
      // applies) -- if the default isn't installed either, wspy-analyze's
      // own error shows up in the streamed log below, same as any other
      // wspy-analyze failure.
      runButton.disabled = true;
      resultEl.textContent = "";
      logEl.hidden = false;
      logEl.textContent = "";

      fetch(runButton.dataset.analyzeUrl, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          models: models,
          all_models: allModels,
          critique: getChecked("analyze-critique"),
          template: template,
        }),
      })
        .then(function (resp) {
          return resp.json().then(function (data) {
            if (!resp.ok) throw new Error(data.error || ("HTTP " + resp.status));
            return data;
          });
        })
        .then(function (data) {
          var es = new EventSource(data.events_url);
          es.addEventListener("log", function (ev) {
            logEl.textContent += JSON.parse(ev.data) + "\n";
            logEl.scrollTop = logEl.scrollHeight;
          });
          es.addEventListener("done", function (ev) {
            var payload = JSON.parse(ev.data);
            es.close();
            runButton.disabled = false;
            if (payload.status === "done") {
              resultEl.innerHTML = "Done. Reload this page, or open the "
                + '<a href="' + escapeHtml(data.studio_url) + '">curation studio</a> '
                + "to add the analysis as a block.";
            } else {
              resultEl.textContent = "Finished with errors -- see output above.";
            }
          });
          es.onerror = function () {
            runButton.disabled = false;
          };
        })
        .catch(function (err) {
          runButton.disabled = false;
          resultEl.textContent = "Error: " + err.message;
        });
    });
  }

  function wireDiscoveryTab() {
    runSyncEndpoint({
      buttonId: "capabilities-run",
      outputId: "capabilities-output",
      cmdlineId: "capabilities-cmdline",
      endpoint: "/api/discovery/capabilities",
      buildBody: function () { return {}; },
    });
    runSyncEndpoint({
      buttonId: "preflight-run",
      outputId: "preflight-output",
      cmdlineId: "preflight-cmdline",
      endpoint: "/api/discovery/preflight",
      buildBody: function () { return { groups: selectedGroups("preflight") }; },
    });
  }

  wireTabs();
  wireRunTab();
  wireCheckButton();
  wireValidateTab();
  wireStoreTab();
  wireDiscoveryTab();
  wireAnalyzeForm();
})();
