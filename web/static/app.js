(function () {
  "use strict";

  var WSPY_FIXED_ARGS = ["--csv", "--interval", "1", "--topdown",
    "--no-rusage", "--no-software", "--no-ipc"];

  function shQuote(s) {
    if (/^[A-Za-z0-9_.\/=:-]+$/.test(s) && s.length > 0) return s;
    return "'" + s.replace(/'/g, "'\\''") + "'";
  }

  function shJoin(argv) {
    return argv.map(shQuote).join(" ");
  }

  function splitWorkload(s) {
    // Rough client-side approximation of shlex.split for the preview only;
    // the server does the real, authoritative parsing on submit.
    var out = [];
    var re = /'[^']*'|"[^"]*"|\S+/g;
    var m;
    while ((m = re.exec(s.trim())) !== null) {
      var tok = m[0];
      if (tok[0] === "'" || tok[0] === '"') tok = tok.slice(1, -1);
      out.push(tok);
    }
    return out;
  }

  function rundirFor(form, suite, benchmark, runId) {
    return form.dataset.outputRoot + "/" + (suite || "manual") + "/" +
      (benchmark || "<benchmark>") + "/" + (runId || "<auto>");
  }

  // Wires one launcher form: builds a live command preview from its inputs,
  // and on submit POSTs endpoint, then streams the run's live output over
  // SSE into liveOutput/resultEl. `buildPreview(form)` returns nothing but
  // writes into the form's own preview element(s) -- kept per-launcher since
  // item 6's fixed config previews two literal commands and item 7's profile
  // launcher previews one (gnuplot.sh there is conditional, not literal).
  function wireLauncher(opts) {
    var form = document.getElementById(opts.formId);
    if (!form) return;
    var fields = {};
    opts.fields.forEach(function (id) {
      fields[id] = document.getElementById(id);
    });
    var liveOutput = document.getElementById(opts.liveOutputId);
    var runButton = document.getElementById(opts.runButtonId);
    var runResult = document.getElementById(opts.runResultId);

    Object.keys(fields).forEach(function (id) {
      fields[id].addEventListener("input", function () {
        opts.buildPreview(form, fields);
      });
    });
    opts.buildPreview(form, fields);

    form.addEventListener("submit", function (ev) {
      ev.preventDefault();
      runButton.disabled = true;
      runResult.textContent = "";
      liveOutput.hidden = false;
      liveOutput.textContent = "";

      fetch(opts.endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(opts.buildBody(fields)),
      })
        .then(function (resp) {
          return resp.json().then(function (data) {
            if (!resp.ok) throw new Error(data.error || ("HTTP " + resp.status));
            return data;
          });
        })
        .then(function (data) {
          if (fields.run_id) fields.run_id.value = data.run_id;
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

  wireLauncher({
    formId: "run-form",
    fields: ["workload", "suite", "benchmark", "run_id"],
    liveOutputId: "live-output",
    runButtonId: "run-button",
    runResultId: "run-result",
    endpoint: "/api/run",
    buildPreview: function (form, f) {
      var rundir = rundirFor(form, f.suite.value.trim(), f.benchmark.value.trim(), f.run_id.value.trim());
      var workloadArgv = f.workload.value.trim() ? splitWorkload(f.workload.value) : ["<workload command>"];
      var wspyArgv = [form.dataset.wspy].concat(WSPY_FIXED_ARGS,
        ["-o", rundir + "/amdtopdown.csv", "--manifest", rundir + "/amdtopdown.manifest.json", "--"],
        workloadArgv);
      document.getElementById("preview-wspy").textContent = "$ " + shJoin(wspyArgv);
      var gnuplotArgv = ["bash", form.dataset.gnuplot];
      document.getElementById("preview-gnuplot").textContent =
        "$ (cd " + shQuote(rundir) + " && " + shJoin(gnuplotArgv) + ")";
    },
    buildBody: function (f) {
      return {
        workload: f.workload.value,
        suite: f.suite.value,
        benchmark: f.benchmark.value,
        run_id: f.run_id.value,
      };
    },
  });

  wireLauncher({
    formId: "profile-run-form",
    fields: ["p_profile", "p_workload", "p_suite", "p_benchmark", "p_run_id"],
    liveOutputId: "p-live-output",
    runButtonId: "p-run-button",
    runResultId: "p-run-result",
    endpoint: "/api/run-profile",
    buildPreview: function (form, f) {
      var outputRoot = form.dataset.outputRoot;
      var profile = f.p_profile.value.trim() || "<profile>";
      var workloadArgv = f.p_workload.value.trim() ? splitWorkload(f.p_workload.value) : ["<workload command>"];
      var suite = f.p_suite.value.trim() || "manual";
      var benchmark = f.p_benchmark.value.trim() || "<benchmark>";
      var runId = f.p_run_id.value.trim() || "<auto>";
      var argv = [form.dataset.wspyRun, "--wspy", form.dataset.wspy, "-o", outputRoot,
        "--suite", suite, "--benchmark", benchmark, "--run-id", runId, profile, "--"]
        .concat(workloadArgv);
      document.getElementById("p-preview-wspy").textContent = "$ " + shJoin(argv);
    },
    buildBody: function (f) {
      return {
        profile: f.p_profile.value,
        workload: f.p_workload.value,
        suite: f.p_suite.value,
        benchmark: f.p_benchmark.value,
        run_id: f.p_run_id.value,
      };
    },
  });
})();
