#!/usr/bin/env python3
"""
wspy-phoronix-segment - Partition wspy telemetry CSVs into per-test-case datasets.

This script parses a wspy unified run directory alongside Phoronix Test Suite (PTS)
results and logs, aligning wall-clock times to slice telemetry CSVs into individual
test cases and trial runs.
"""

import os
import sys
import json
import glob
import argparse
import datetime
import xml.etree.ElementTree as ET
import csv

def parse_iso8601(s):
    # E.g. 2026-07-19T12:21:35.843Z or 2026-07-19T13:32:55.000Z
    s = s.replace("Z", "+00:00")
    try:
        return datetime.datetime.fromisoformat(s)
    except ValueError:
        # Fallback for older python versions or variations
        if "." in s:
            base, ns = s.split(".")
            ns = ns[:6]  # limit to microsecond
            s = f"{base}.{ns}"
        return datetime.datetime.strptime(s, "%Y-%m-%dT%H:%M:%S")

def parse_log_timestamp(s):
    # E.g. 2026-07-19 12:24:42
    # PTS logs write timestamps in UTC/system clock format
    dt = datetime.datetime.strptime(s.strip(), "%Y-%m-%d %H:%M:%S")
    return dt.replace(tzinfo=datetime.timezone.utc)

def parse_xml_timestamp(s):
    # E.g. 2026-07-19 12:21:36
    dt = datetime.datetime.strptime(s.strip(), "%Y-%m-%d %H:%M:%S")
    return dt.replace(tzinfo=datetime.timezone.utc)

def find_phoronix_results(suite, benchmark, pass_start, pass_finish):
    pts_dir = os.path.expanduser("~/.phoronix-test-suite/test-results")
    if not os.path.isdir(pts_dir):
        return None
    
    candidates = []
    for d in os.listdir(pts_dir):
        full_path = os.path.join(pts_dir, d)
        if not os.path.isdir(full_path):
            continue
        comp_xml = os.path.join(full_path, "composite.xml")
        if not os.path.isfile(comp_xml):
            continue
        
        try:
            tree = ET.parse(comp_xml)
            root = tree.getroot()
            
            # Check timestamp in XML
            system_node = root.find("System")
            if system_node is None:
                continue
            ts_node = system_node.find("TimeStamp")
            if ts_node is None or not ts_node.text:
                continue
            
            xml_dt = parse_xml_timestamp(ts_node.text)
            
            # Check if this results folder falls within the pass execution window
            # Adding a small buffer (e.g. 15s) in case of system startup latency
            if (pass_start - datetime.timedelta(seconds=15)) <= xml_dt <= (pass_finish + datetime.timedelta(seconds=15)):
                # Verify that it contains results for our benchmark
                has_bench = False
                for res in root.findall("Result"):
                    ident = res.find("Identifier")
                    if ident is not None and ident.text and (benchmark in ident.text):
                        has_bench = True
                        break
                if has_bench:
                    candidates.append((xml_dt, full_path))
        except Exception as e:
            # Skip invalid XML or other errors
            continue
            
    if not candidates:
        return None
    
    # Return the one closest to the pass start time
    candidates.sort(key=lambda x: abs((x[0] - pass_start).total_seconds()))
    return candidates[0][1]

def get_hash_mappings(install_identifier):
    # E.g. pts/openssl-4.0.0
    install_dir = os.path.expanduser(f"~/.phoronix-test-suite/installed-tests/{install_identifier}")
    pts_install_json = os.path.join(install_dir, "pts-install.json")
    if not os.path.isfile(pts_install_json):
        return {}
        
    try:
        with open(pts_install_json, "r") as f:
            data = json.load(f)
        
        per_run = data.get("test_installation", {}).get("history", {}).get("per_run_times", {})
        mappings = {}
        for h, info in per_run.items():
            if h == "all":
                continue
            desc = info.get("desc")
            if desc:
                mappings[h] = desc
        return mappings
    except Exception:
        return {}

def parse_trial_runs_from_log(log_file):
    # Parses the trial runs and their start timestamps from a PTS log file
    runs = []
    if not os.path.isfile(log_file):
        return runs
        
    try:
        with open(log_file, "r") as f:
            lines = f.readlines()
        
        current_run = None
        for i, line in enumerate(lines):
            # E.g. 2026-07-19 12:21 - Run 1
            if " - Run " in line and line.startswith("202"):
                # The next line or line after usually has the precise start timestamp
                # Let's inspect the next few lines
                for offset in range(1, 4):
                    if i + offset < len(lines):
                        candidate = lines[i + offset].strip()
                        if candidate.startswith("202") and len(candidate) == 19:
                            try:
                                dt = parse_log_timestamp(candidate)
                                runs.append(dt)
                                break
                            except ValueError:
                                pass
    except Exception:
        pass
    return runs

def slice_csv(csv_path, start_dt, elapsed_start, elapsed_finish, out_path):
    if not os.path.isfile(csv_path):
        return False
        
    try:
        with open(csv_path, "r") as f:
            reader = csv.reader(f)
            header = next(reader)
            
            # Find the time column
            if "time" not in header:
                return False
            time_idx = header.index("time")
            
            sliced_rows = []
            for row in reader:
                if not row or len(row) <= time_idx:
                    continue
                try:
                    t_val = float(row[time_idx])
                    if elapsed_start <= t_val <= elapsed_finish:
                        sliced_rows.append(row)
                except ValueError:
                    continue
            
            if not sliced_rows:
                return False
                
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, "w", newline="") as out_f:
                writer = csv.writer(out_f)
                writer.writerow(header)
                writer.writerows(sliced_rows)
            return True
    except Exception as e:
        print(f"Error slicing CSV {csv_path}: {e}", file=sys.stderr)
        return False

def make_safe_filename(s):
    # E.g. "Algorithm: SHA256" -> "algorithm_sha256"
    return "".join(c if c.isalnum() else "_" for c in s.lower()).strip("_")

def main():
    parser = argparse.ArgumentParser(description="Segment wspy telemetry CSVs by Phoronix test cases.")
    parser.add_argument("--rundir", required=True, help="Path to the wspy unified run directory")
    args = parser.parse_args()
    
    run_dir = os.path.abspath(args.rundir)
    manifest_path = os.path.join(run_dir, "manifest.json")
    if not os.path.isfile(manifest_path):
        print(f"Error: manifest.json not found in {run_dir}", file=sys.stderr)
        sys.exit(1)
        
    try:
        with open(manifest_path, "r") as f:
            manifest = json.load(f)
    except Exception as e:
        print(f"Error reading manifest: {e}", file=sys.stderr)
        sys.exit(1)
        
    suite = manifest.get("suite")
    benchmark = manifest.get("benchmark")
    if not suite or not benchmark:
        print("Error: manifest.json missing suite or benchmark fields", file=sys.stderr)
        sys.exit(1)
        
    print(f"Analyzing run directory: {run_dir}")
    print(f"Suite: {suite}, Benchmark: {benchmark}")
    
    passes = manifest.get("passes", [])
    for p in passes:
        pass_name = p.get("name")
        pass_manifest_name = p.get("manifest")
        pass_output_name = p.get("output")
        
        if not pass_manifest_name:
            continue
            
        pass_manifest_path = os.path.join(run_dir, pass_manifest_name)
        if not os.path.isfile(pass_manifest_path):
            print(f"Skipping pass {pass_name}: manifest {pass_manifest_name} not found")
            continue
            
        try:
            with open(pass_manifest_path, "r") as f:
                pm = json.load(f)
        except Exception:
            continue
            
        timing = pm.get("timing", {})
        start_time_str = timing.get("start_time")
        finish_time_str = timing.get("finish_time")
        if not start_time_str or not finish_time_str:
            continue
            
        pass_start = parse_iso8601(start_time_str)
        pass_finish = parse_iso8601(finish_time_str)
        
        print(f"\n--- Pass: {pass_name} ---")
        print(f"  Time Window: {pass_start.isoformat()} to {pass_finish.isoformat()}")
        
        # Locate corresponding Phoronix Results Folder
        pts_res_dir = find_phoronix_results(suite, benchmark, pass_start, pass_finish)
        if not pts_res_dir:
            print("  Could not find matching Phoronix test results directory for this pass.")
            continue
            
        print(f"  Matched Phoronix results directory: {pts_res_dir}")
        comp_xml = os.path.join(pts_res_dir, "composite.xml")
        
        try:
            tree = ET.parse(comp_xml)
            root = tree.getroot()
        except Exception as e:
            print(f"  Error parsing composite.xml: {e}")
            continue
            
        # Parse result metadata
        results = []
        for res in root.findall("Result"):
            ident = res.find("Identifier")
            if ident is None or not ident.text or benchmark not in ident.text:
                continue
            desc_node = res.find("Description")
            desc = desc_node.text if desc_node is not None else ""
            
            # Find times in Entry JSON
            durations = []
            entry = res.find("Data/Entry")
            if entry is not None:
                json_node = entry.find("JSON")
                if json_node is not None and json_node.text:
                    try:
                        entry_json = json.loads(json_node.text)
                        times_str = entry_json.get("test-run-times", "")
                        if times_str:
                            durations = [float(d) for d in times_str.split(":")]
                    except Exception:
                        pass
            
            results.append({
                "identifier": ident.text,
                "description": desc,
                "durations": durations
            })
            
        if not results:
            print("  No matching benchmark results found in composite.xml")
            continue
            
        # Load installed-tests hash mappings from pts-install.json
        install_ident = results[0]["identifier"]
        hash_mappings = get_hash_mappings(install_ident)
        if not hash_mappings:
            print(f"  Warning: Could not load pts-install.json for {install_ident}")
            
        # Correlate logs and trial run times
        scheduled_runs = []
        for res in results:
            desc = res["description"]
            # Find corresponding hash
            matched_hash = None
            for h, h_desc in hash_mappings.items():
                if h_desc == desc:
                    matched_hash = h
                    break
            
            # If no direct match in pts-install.json, we can fallback to matching
            # description in generated.json or using computed hash.
            # But pts-install.json covers most cases.
            if not matched_hash:
                continue
                
            log_pattern = os.path.join(pts_res_dir, "test-logs", matched_hash, "*.log")
            log_files = glob.glob(log_pattern)
            if not log_files:
                continue
                
            log_file = log_files[0]
            trial_starts = parse_trial_runs_from_log(log_file)
            durations = res["durations"]
            
            for idx, start_dt in enumerate(trial_starts):
                if idx < len(durations):
                    d = durations[idx]
                    elapsed_start = (start_dt - pass_start).total_seconds()
                    elapsed_finish = elapsed_start + d
                    
                    scheduled_runs.append({
                        "description": desc,
                        "trial": idx + 1,
                        "start_dt": start_dt,
                        "elapsed_start": elapsed_start,
                        "elapsed_finish": elapsed_finish
                    })
                    
        if not scheduled_runs:
            print("  Could not correlate any trial runs from logs.")
            continue
            
        # Sort scheduled runs by start time
        scheduled_runs.sort(key=lambda x: x["start_dt"])
        
        # Scan for CSV files in the pass directory
        # Pass output path might be named like "<name>.csv"
        # We can find all CSV files next to manifest or output folder
        csv_files = glob.glob(os.path.join(run_dir, "*.csv"))
        if not csv_files:
            print("  No telemetry CSV files found in run directory to slice.")
            continue
            
        for csv_file in csv_files:
            csv_name = os.path.basename(csv_file)
            print(f"  Processing telemetry CSV: {csv_name}")
            
            sliced_count = 0
            for run in scheduled_runs:
                desc_slug = make_safe_filename(run["description"])
                trial = run["trial"]
                el_start = run["elapsed_start"]
                el_finish = run["elapsed_finish"]
                
                out_name = f"{desc_slug}_run{trial}_{csv_name}"
                out_path = os.path.join(run_dir, "segmented", pass_name, out_name)
                
                success = slice_csv(csv_file, run["start_dt"], el_start, el_finish, out_path)
                if success:
                    print(f"    Sliced: {out_name} [{el_start:.1f}s to {el_finish:.1f}s]")
                    sliced_count += 1
            
            if sliced_count > 0:
                print(f"  Successfully segmented {csv_name} into {sliced_count} test-case files.")
                
    print("\nSegmentation complete!")

if __name__ == "__main__":
    main()
