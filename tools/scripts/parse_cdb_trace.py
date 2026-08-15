"""Parse genuine tagged Armada II CDB events; ignore command echoes."""
from __future__ import annotations
import argparse, json, re
from pathlib import Path

TAG = re.compile(r"\[A2TRACE:([A-Z0-9_]+)\]")
MODULE = re.compile(r"ModLoad:\s+([0-9a-fA-F`]+)\s+([0-9a-fA-F`]+)\s+(.*)")

ap = argparse.ArgumentParser()
ap.add_argument("--log", required=True)
ap.add_argument("--session", required=True)
ap.add_argument("--output-json", required=True)
ap.add_argument("--output-md", required=True)
args = ap.parse_args()

log = Path(args.log).read_text(encoding="utf-8-sig", errors="replace") if Path(args.log).exists() else ""
out = {"session": json.loads(Path(args.session).read_text(encoding="utf-8-sig")), "modules": [], "events": [],
       "result_hits": {"SUCCEED": 0, "FAIL": 0}, "errors": []}
pending_objective = False
for line in log.splitlines():
    match = MODULE.search(line)
    if match:
        out["modules"].append({"base": match.group(1), "end": match.group(2), "path": match.group(3).strip()})
    if "Couldn't resolve" in line or "error" in line.lower():
        out["errors"].append(line)
    if ".printf" in line or re.search(r"\bbp\s+", line):
        continue
    if "[A2TRACE:OBJECTIVE] tid=***" in line:
        pending_objective = True
        continue
    if pending_objective and re.match(r"^[0-9a-fA-F]+\s+ret=[0-9a-fA-F]+\s+index=[0-9a-fA-F]+\s+complete=[0-9a-fA-F]+$", line):
        out["events"].append({"tag": "OBJECTIVE", "raw": "[A2TRACE:OBJECTIVE] tid=" + line})
        pending_objective = False
        continue
    pending_objective = False
    tag = TAG.search(line)
    if tag:
        event = {"tag": tag.group(1), "raw": line}
        out["events"].append(event)
        if tag.group(1) in ("SUCCEED", "FAIL"):
            out["result_hits"][tag.group(1)] += 1

Path(args.output_json).write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
summary = ["# Parsed Armada II CDB trace", "", f"Modules: {len(out['modules'])}",
           f"SucceedMission hits: {out['result_hits']['SUCCEED']}", f"FailMission hits: {out['result_hits']['FAIL']}", "",
           "## Events", ""]
summary.extend(f"- `{event['tag']}` {event['raw']}" for event in out["events"])
Path(args.output_md).write_text("\n".join(summary) + "\n", encoding="utf-8")
