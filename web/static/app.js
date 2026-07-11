(function () {
  "use strict";

  var form = document.getElementById("run-form");
  if (!form) return; // report page has no form

  var workloadEl = document.getElementById("workload");
  var suiteEl = document.getElementById("suite");
  var benchmarkEl = document.getElementById("benchmark");
  var runIdEl = document.getElementById("run_id");
  var previewWspy = document.getElementById("preview-wspy");
  var previewGnuplot = document.getElementById("preview-gnuplot");
  var liveOutput = document.getElementById("live-output");
  var runButton = document.getElementById("run-button");
  var runResult = document.getElementById("run-result");

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

  function updatePreview() {
    var workload = workloadEl.value.trim();
    var suite = (suiteEl.value.trim() || "manual");
    var benchmark = benchmarkEl.value.trim() || "<benchmark>";
    var runId = runIdEl.value.trim() || "<auto>";
    var outputRoot = form.dataset.outputRoot;
    var wspyBin = form.dataset.wspy;
    var gnuplotScript = form.dataset.gnuplot;
    var rundir = outputRoot + "/" + suite + "/" + benchmark + "/" + runId;

    var workloadArgv = workload ? splitWorkload(workload) : ["<workload command>"];
    var wspyArgv = [wspyBin].concat(WSPY_FIXED_ARGS,
      ["-o", rundir + "/amdtopdown.csv", "--manifest", rundir + "/amdtopdown.manifest.json", "--"],
      workloadArgv);
    previewWspy.textContent = "$ " + shJoin(wspyArgv);

    var gnuplotArgv = ["bash", gnuplotScript];
    previewGnuplot.textContent = "$ (cd " + shQuote(rundir) + " && " + shJoin(gnuplotArgv) + ")";
  }

  [workloadEl, suiteEl, benchmarkEl, runIdEl].forEach(function (el) {
    el.addEventListener("input", updatePreview);
  });
  updatePreview();

  form.addEventListener("submit", function (ev) {
    ev.preventDefault();
    runButton.disabled = true;
    runResult.textContent = "";
    liveOutput.hidden = false;
    liveOutput.textContent = "";

    fetch("/api/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        workload: workloadEl.value,
        suite: suiteEl.value,
        benchmark: benchmarkEl.value,
        run_id: runIdEl.value,
      }),
    })
      .then(function (resp) {
        return resp.json().then(function (data) {
          if (!resp.ok) throw new Error(data.error || ("HTTP " + resp.status));
          return data;
        });
      })
      .then(function (data) {
        runIdEl.value = data.run_id;
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
})();
