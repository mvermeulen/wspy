#!/usr/bin/env python3
"""
wspy web launcher + report browser -- thin end-to-end slice (INVESTIGATION_4.0.md
4.1 Tier 2, item 6).

One fixed configuration only: the "amdtopdown" pass wspy-run's deep-cpu/deep-gpu
profiles already use --

    wspy --csv --interval 1 --topdown --no-rusage --no-software --no-ipc

-- followed by workload/phoronix/gnuplot.sh's amdtopdown.csv -> amdtopdown.png
plot block. No preset picker, no configuration/option checklist: that's #8,
built on top of this slice later. This process is a thin client: every run it
launches is exactly the wspy/gnuplot command lines a user could type by hand,
and it shows both before running them. It keeps no state beyond the run
directories it writes -- the report browser reads CSV/manifest/PNG straight
off disk, no database.

Usage:
    web/server.py [--host HOST] [--port PORT] [--wspy PATH]
                   [--gnuplot-script PATH] [--output-root DIR]

Stdlib only, by design (see CLAUDE.md's web/ entry for the reasoning).
"""
import argparse
import html
import json
import os
import queue
import re
import secrets
import shlex
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")

# The one fixed configuration this slice knows about -- matches wspy-run's
# deep-cpu/deep-gpu "amdtopdown" pass exactly (wspy-run, load_builtin_profile()).
WSPY_FIXED_ARGS = ["--csv", "--interval", "1", "--topdown",
                    "--no-rusage", "--no-software", "--no-ipc"]
CSV_NAME = "amdtopdown.csv"
MANIFEST_NAME = "amdtopdown.manifest.json"
PNG_NAME = "amdtopdown.png"
LOG_NAME = "launch.log"
ARTIFACT_FILES = (CSV_NAME, MANIFEST_NAME, PNG_NAME, LOG_NAME)

NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")

# ---------------------------------------------------------------------------
# Run registry: in-memory only, purely to relay a run's live log lines to an
# SSE stream while it's in flight. Nothing here is authoritative -- once a
# run finishes, the report page reads its directory off disk like any other,
# so a server restart mid-run loses only the live tail, not the artifacts.
# ---------------------------------------------------------------------------

class RunState:
    def __init__(self, rundir):
        self.rundir = rundir
        self.lines = []
        self.status = "running"  # running | done | error
        self.report_url = None
        self.cond = threading.Condition()

    def append(self, line):
        with self.cond:
            self.lines.append(line)
            self.cond.notify_all()

    def finish(self, status, report_url):
        with self.cond:
            self.status = status
            self.report_url = report_url
            self.cond.notify_all()


RUNS = {}
RUNS_LOCK = threading.Lock()


def run_key(suite, benchmark, run_id):
    return (suite, benchmark, run_id)


def valid_segment(s):
    return bool(s) and bool(NAME_RE.match(s)) and s not in (".", "..")


def make_run_id():
    # Same shape as wspy-run's own <timestamp>.<ms>-<suffix> run ids (see
    # run_index.c's format_run_id()), but this server is long-running so a
    # per-request pid isn't unique the way wspy-run's own "$$" is -- a short
    # random hex suffix stands in for it instead.
    now = datetime.now(timezone.utc)
    ms = now.microsecond // 1000
    return f"{now.strftime('%Y%m%dT%H%M%S')}.{ms:03d}-{secrets.token_hex(4)}"


def default_benchmark_from_workload(argv):
    base = os.path.basename(argv[0]) if argv else "workload"
    base = re.sub(r"[^A-Za-z0-9_.-]", "_", base)
    return base or "workload"


# ---------------------------------------------------------------------------
# Command construction -- the same strings are used to (a) show the user what
# will run, and (b) actually run it, so the preview is never a lie.
# ---------------------------------------------------------------------------

def build_wspy_argv(wspy_bin, rundir, workload_argv):
    csv_path = os.path.join(rundir, CSV_NAME)
    manifest_path = os.path.join(rundir, MANIFEST_NAME)
    return ([wspy_bin] + WSPY_FIXED_ARGS +
            ["-o", csv_path, "--manifest", manifest_path, "--"] + workload_argv)


def build_gnuplot_argv(gnuplot_script):
    return ["bash", gnuplot_script]


def shell_preview(argv, cwd=None):
    s = shlex.join(argv)
    if cwd:
        return f"(cd {shlex.quote(cwd)} && {s})"
    return s


# ---------------------------------------------------------------------------
# Run execution
# ---------------------------------------------------------------------------

def execute_run(state, wspy_bin, gnuplot_script, rundir, workload_argv):
    log_path = os.path.join(rundir, LOG_NAME)
    logf = open(log_path, "w")

    def emit(line):
        logf.write(line + "\n")
        logf.flush()
        state.append(line)

    wspy_argv = build_wspy_argv(wspy_bin, rundir, workload_argv)
    emit("$ " + shell_preview(wspy_argv))
    try:
        proc = subprocess.Popen(wspy_argv, cwd=REPO_ROOT,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
    except OSError as e:
        emit(f"[error] failed to launch wspy ({wspy_bin}): {e}")
        logf.close()
        state.finish("error", None)
        return

    for line in proc.stdout:
        emit(line.rstrip("\n"))
    wspy_rc = proc.wait()
    emit(f"[wspy exited {wspy_rc}]")

    csv_path = os.path.join(rundir, CSV_NAME)
    ok = wspy_rc == 0 and os.path.exists(csv_path) and os.path.getsize(csv_path) > 0

    if not ok:
        emit("[skipping gnuplot: no usable CSV output]")
        logf.close()
        state.finish("error", None)
        return

    gnuplot_argv = build_gnuplot_argv(gnuplot_script)
    emit("$ " + shell_preview(gnuplot_argv, cwd=rundir))
    try:
        proc = subprocess.Popen(gnuplot_argv, cwd=rundir,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT,
                                 text=True, bufsize=1)
        for line in proc.stdout:
            emit(line.rstrip("\n"))
        gnuplot_rc = proc.wait()
        emit(f"[gnuplot exited {gnuplot_rc}]")
    except OSError as e:
        emit(f"[error] failed to launch gnuplot script ({gnuplot_script}): {e}")
        gnuplot_rc = 1

    logf.close()
    status = "done" if gnuplot_rc == 0 else "error"
    state.finish(status, None)


# ---------------------------------------------------------------------------
# Report discovery -- scans OUTPUT_ROOT for suite/benchmark/run_id
# directories holding at least one known artifact, newest first. This is
# what lets a report be found without knowing its path by heart.
# ---------------------------------------------------------------------------

def discover_reports(output_root, limit=50):
    reports = []
    if not os.path.isdir(output_root):
        return reports
    for suite in sorted(os.listdir(output_root)):
        suite_dir = os.path.join(output_root, suite)
        if not os.path.isdir(suite_dir):
            continue
        for benchmark in sorted(os.listdir(suite_dir)):
            bench_dir = os.path.join(suite_dir, benchmark)
            if not os.path.isdir(bench_dir):
                continue
            for run_id in sorted(os.listdir(bench_dir)):
                run_dir = os.path.join(bench_dir, run_id)
                if not os.path.isdir(run_dir):
                    continue
                if not any(os.path.exists(os.path.join(run_dir, f)) for f in ARTIFACT_FILES):
                    continue
                mtime = os.path.getmtime(run_dir)
                reports.append({
                    "suite": suite, "benchmark": benchmark, "run_id": run_id,
                    "mtime": mtime,
                })
    reports.sort(key=lambda r: r["mtime"], reverse=True)
    return reports[:limit]


def read_manifest_workload(manifest_path):
    """Best-effort: pull the workload command back out of a manifest.json's
    command.argv (manifest.c writes just the workload's own argv there, not
    wspy's own flags -- see wspy.c main()'s command_line_argv), so 'customize
    & run again' can prefill the launcher without re-parsing anything else."""
    try:
        with open(manifest_path) as f:
            data = json.load(f)
        argv = data.get("command", {}).get("argv", [])
        return argv or None
    except (OSError, json.JSONDecodeError, ValueError):
        pass
    return None


# ---------------------------------------------------------------------------
# HTML rendering
# ---------------------------------------------------------------------------

def page(title, body):
    return f"""<!doctype html>
<html><head><meta charset="utf-8">
<title>{html.escape(title)}</title>
<link rel="stylesheet" href="/static/style.css">
</head><body>
<header><a href="/" class="brand">wspy web</a></header>
<main>{body}</main>
</body></html>"""


def render_index(output_root, prefill, wspy_bin, gnuplot_script):
    reports = discover_reports(output_root)
    rows = []
    for r in reports:
        ts = datetime.fromtimestamp(r["mtime"]).strftime("%Y-%m-%d %H:%M:%S")
        url = f"/report/{r['suite']}/{r['benchmark']}/{r['run_id']}"
        rows.append(
            f"<tr><td>{html.escape(ts)}</td>"
            f"<td>{html.escape(r['suite'])}</td>"
            f"<td>{html.escape(r['benchmark'])}</td>"
            f"<td><a href=\"{html.escape(url)}\">{html.escape(r['run_id'])}</a></td></tr>"
        )
    reports_html = (
        "<table class=\"reports\"><thead><tr><th>when</th><th>suite</th>"
        "<th>benchmark</th><th>run</th></tr></thead><tbody>" + "".join(rows) +
        "</tbody></table>" if rows else "<p class=\"muted\">No runs yet.</p>"
    )

    w_workload = html.escape(prefill.get("workload", ""))
    w_suite = html.escape(prefill.get("suite", "manual"))
    w_benchmark = html.escape(prefill.get("benchmark", ""))

    body = f"""
<section class="panel">
  <h1>Launcher</h1>
  <p class="config-label">Configuration: <code>amdtopdown</code>
     (fixed for this slice &mdash; the preset/configuration picker lands here later)</p>
  <form id="run-form" data-output-root="{html.escape(output_root)}"
        data-wspy="{html.escape(wspy_bin)}" data-gnuplot="{html.escape(gnuplot_script)}">
    <label>Workload command
      <input type="text" id="workload" name="workload" value="{w_workload}"
             placeholder="e.g. sleep 5" required>
    </label>
    <div class="row">
      <label>Suite
        <input type="text" id="suite" name="suite" value="{w_suite}">
      </label>
      <label>Benchmark
        <input type="text" id="benchmark" name="benchmark" value="{w_benchmark}"
               placeholder="(defaults to workload's program name)">
      </label>
      <label>Run id
        <input type="text" id="run_id" name="run_id" placeholder="(auto)">
      </label>
    </div>
    <fieldset class="preview">
      <legend>Commands about to run (copy/paste-able)</legend>
      <pre id="preview-wspy"></pre>
      <pre id="preview-gnuplot"></pre>
    </fieldset>
    <button type="submit" id="run-button">Run</button>
  </form>
  <pre id="live-output" class="live-output" hidden></pre>
  <p id="run-result"></p>
</section>
<section class="panel">
  <h2>Recent reports</h2>
  {reports_html}
</section>
<script src="/static/app.js"></script>
"""
    return page("wspy web launcher", body)


def render_report(output_root, suite, benchmark, run_id):
    rundir = os.path.join(output_root, suite, benchmark, run_id)
    if not os.path.isdir(rundir):
        return None

    csv_path = os.path.join(rundir, CSV_NAME)
    manifest_path = os.path.join(rundir, MANIFEST_NAME)
    png_path = os.path.join(rundir, PNG_NAME)
    log_path = os.path.join(rundir, LOG_NAME)
    base = f"/files/{suite}/{benchmark}/{run_id}"

    workload = read_manifest_workload(manifest_path)
    workload_str = shlex.join(workload) if workload else None

    parts = [f"<h1>Report: {html.escape(suite)} / {html.escape(benchmark)} / {html.escape(run_id)}</h1>"]

    if workload_str:
        parts.append(f"<p>Workload: <code>{html.escape(workload_str)}</code></p>")
        rerun_url = ("/?" +
                     "workload=" + _urlescape(workload_str) +
                     "&suite=" + _urlescape(suite) +
                     "&benchmark=" + _urlescape(benchmark))
        parts.append(f'<p><a href="{rerun_url}">Customize &amp; run again</a></p>')
    else:
        parts.append('<p class="muted">No manifest found; can\'t restore workload command.</p>')

    parts.append("<h2>Artifacts</h2><ul class=\"artifacts\">")
    if os.path.exists(png_path):
        parts.append(f'<li>Topdown plot:<br><img class="plot" src="{base}/{PNG_NAME}" alt="amdtopdown plot"></li>')
    else:
        parts.append('<li class="muted">amdtopdown.png not generated</li>')
    if os.path.exists(csv_path):
        parts.append(f'<li><a href="{base}/{CSV_NAME}">{CSV_NAME}</a> (raw CSV)</li>')
    else:
        parts.append('<li class="muted">amdtopdown.csv missing</li>')
    if os.path.exists(manifest_path):
        parts.append(f'<li><a href="{base}/{MANIFEST_NAME}">{MANIFEST_NAME}</a> (raw manifest)</li>')
    else:
        parts.append('<li class="muted">amdtopdown.manifest.json missing</li>')
    if os.path.exists(log_path):
        parts.append(f'<li><a href="{base}/{LOG_NAME}">{LOG_NAME}</a> (launch log)</li>')
    parts.append("</ul>")

    body = "<section class=\"panel\">" + "".join(parts) + "</section>"
    return page(f"wspy report: {benchmark}/{run_id}", body)


def _urlescape(s):
    from urllib.parse import quote
    return quote(s, safe="")


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "wspy-web/0.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, body, content_type="text/html; charset=utf-8", headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_json(self, code, obj):
        self._send(code, json.dumps(obj), content_type="application/json")

    def do_GET(self):
        parsed = urlsplit(self.path)
        path = parsed.path
        qs = parse_qs(parsed.query)
        cfg = self.server.wspy_cfg

        if path == "/":
            prefill = {k: v[0] for k, v in qs.items() if k in ("workload", "suite", "benchmark")}
            self._send(200, render_index(cfg["output_root"], prefill,
                                          cfg["wspy_bin"], cfg["gnuplot_script"]))
            return

        if path.startswith("/static/"):
            self._serve_static(path[len("/static/"):])
            return

        m = re.match(r"^/report/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id = m.groups()
            if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
                self._send(400, "invalid report path")
                return
            out = render_report(cfg["output_root"], suite, benchmark, run_id)
            if out is None:
                self._send(404, "no such report")
            else:
                self._send(200, out)
            return

        m = re.match(r"^/files/([^/]+)/([^/]+)/([^/]+)/([^/]+)$", path)
        if m:
            suite, benchmark, run_id, filename = m.groups()
            self._serve_artifact(cfg["output_root"], suite, benchmark, run_id, filename)
            return

        m = re.match(r"^/api/run/([^/]+)/([^/]+)/([^/]+)/events$", path)
        if m:
            self._stream_events(*m.groups())
            return

        self._send(404, "not found")

    def do_POST(self):
        parsed = urlsplit(self.path)
        if parsed.path != "/api/run":
            self._send(404, "not found")
            return
        cfg = self.server.wspy_cfg
        length = int(self.headers.get("Content-Length", "0"))
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._send_json(400, {"error": "invalid JSON body"})
            return
        self._start_run(cfg, body)

    def _serve_static(self, rel):
        if not valid_segment(rel):
            self._send(404, "not found")
            return
        path = os.path.join(STATIC_DIR, rel)
        if not os.path.isfile(path):
            self._send(404, "not found")
            return
        ctype = "text/css" if rel.endswith(".css") else \
                "application/javascript" if rel.endswith(".js") else \
                "application/octet-stream"
        with open(path, "rb") as f:
            self._send(200, f.read(), content_type=ctype)

    def _serve_artifact(self, output_root, suite, benchmark, run_id, filename):
        if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
            self._send(400, "invalid path")
            return
        if filename not in ARTIFACT_FILES:
            self._send(404, "not found")
            return
        path = os.path.join(output_root, suite, benchmark, run_id, filename)
        if not os.path.isfile(path):
            self._send(404, "not found")
            return
        ctype = {
            CSV_NAME: "text/csv",
            MANIFEST_NAME: "application/json",
            PNG_NAME: "image/png",
            LOG_NAME: "text/plain",
        }[filename]
        with open(path, "rb") as f:
            self._send(200, f.read(), content_type=ctype)

    def _start_run(self, cfg, body):
        workload_str = (body.get("workload") or "").strip()
        if not workload_str:
            self._send_json(400, {"error": "workload command is required"})
            return
        try:
            workload_argv = shlex.split(workload_str)
        except ValueError as e:
            self._send_json(400, {"error": f"could not parse workload command: {e}"})
            return
        if not workload_argv:
            self._send_json(400, {"error": "workload command is required"})
            return

        suite = (body.get("suite") or "manual").strip()
        benchmark = (body.get("benchmark") or "").strip() or default_benchmark_from_workload(workload_argv)
        run_id = (body.get("run_id") or "").strip() or make_run_id()

        if not all(valid_segment(x) for x in (suite, benchmark, run_id)):
            self._send_json(400, {"error": "suite/benchmark/run_id must be non-empty and "
                                            "contain only letters, digits, '.', '_', '-'"})
            return

        rundir = os.path.join(cfg["output_root"], suite, benchmark, run_id)
        if os.path.exists(rundir):
            self._send_json(409, {"error": f"run directory already exists: {rundir}"})
            return
        os.makedirs(rundir)

        key = run_key(suite, benchmark, run_id)
        state = RunState(rundir)
        with RUNS_LOCK:
            RUNS[key] = state

        wspy_argv = build_wspy_argv(cfg["wspy_bin"], rundir, workload_argv)
        gnuplot_argv = build_gnuplot_argv(cfg["gnuplot_script"])

        t = threading.Thread(target=execute_run, args=(
            state, cfg["wspy_bin"], cfg["gnuplot_script"], rundir, workload_argv,
        ), daemon=True)
        t.start()

        self._send_json(202, {
            "suite": suite, "benchmark": benchmark, "run_id": run_id,
            "events_url": f"/api/run/{suite}/{benchmark}/{run_id}/events",
            "report_url": f"/report/{suite}/{benchmark}/{run_id}",
            "wspy_command": shell_preview(wspy_argv),
            "gnuplot_command": shell_preview(gnuplot_argv, cwd=rundir),
        })

    def _stream_events(self, suite, benchmark, run_id):
        key = run_key(suite, benchmark, run_id)
        with RUNS_LOCK:
            state = RUNS.get(key)
        if state is None:
            self._send(404, "no such run")
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        sent = 0
        try:
            while True:
                with state.cond:
                    state.cond.wait_for(
                        lambda: len(state.lines) > sent or state.status != "running",
                        timeout=15,
                    )
                    pending = state.lines[sent:]
                    sent = len(state.lines)
                    status = state.status
                    report_url = state.report_url
                for line in pending:
                    self.wfile.write(f"event: log\ndata: {json.dumps(line)}\n\n".encode())
                if not pending:
                    self.wfile.write(b": keep-alive\n\n")
                self.wfile.flush()
                if status != "running" and sent >= len(state.lines):
                    report_url = f"/report/{suite}/{benchmark}/{run_id}"
                    self.wfile.write(
                        f"event: done\ndata: {json.dumps({'status': status, 'report_url': report_url})}\n\n".encode()
                    )
                    self.wfile.flush()
                    break
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--wspy", default=os.path.join(REPO_ROOT, "wspy"),
                     help="path to the wspy binary (default: repo root's ./wspy)")
    ap.add_argument("--gnuplot-script",
                     default=os.path.join(REPO_ROOT, "workload", "phoronix", "gnuplot.sh"),
                     help="path to the amdtopdown.csv -> amdtopdown.png plot script")
    ap.add_argument("--output-root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "runs"),
                     help="directory root for <suite>/<benchmark>/<run-id>/ run output "
                          "(default: web/runs)")
    args = ap.parse_args()

    output_root = os.path.abspath(args.output_root)
    os.makedirs(output_root, exist_ok=True)

    if not os.path.isfile(args.wspy):
        print(f"warning: wspy binary not found at {args.wspy} (build it with `make` first; "
              f"runs will fail until it exists)", file=sys.stderr)
    if not os.path.isfile(args.gnuplot_script):
        print(f"warning: gnuplot script not found at {args.gnuplot_script}", file=sys.stderr)

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.wspy_cfg = {
        "wspy_bin": os.path.abspath(args.wspy),
        "gnuplot_script": os.path.abspath(args.gnuplot_script),
        "output_root": output_root,
    }
    print(f"wspy web launcher listening on http://{args.host}:{args.port}  "
          f"(output root: {output_root})")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
