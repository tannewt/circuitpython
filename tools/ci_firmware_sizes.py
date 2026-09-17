#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2014 MicroPython & CircuitPython contributors (https://github.com/adafruit/circuitpython/graphs/contributors)
#
# SPDX-License-Identifier: MIT

"""Merge the per-board firmware size records of a CI run into one report.

build_release_files.py writes sizes/<board>.json for every board it builds, and the board
job uploads it as the artifact zz-sizes-<board>. The build-ci job downloads those into one
directory and runs:

    ci_firmware_sizes.py <records-dir> <out-dir>

which writes

    <out-dir>/0-sizes.json    every board with its per-language builds and derived free flash
    <out-dir>/0-sizes.html    a sortable, filterable table of the same boards, data embedded

The names match the artifacts build-ci uploads them as, one file each, unzipped. The
CircuitPython version, from --version or $CP_VERSION, goes into the reports. The script
also appends a line to $GITHUB_STEP_SUMMARY when that is set, or to --summary FILE,
saying how many boards were built and where the reports are. The run description in the
reports comes from the GITHUB_* environment variables; outside CI the report says so.
"""

import argparse
import collections
import datetime
import html
import json
import os
import pathlib
import sys

# Both reports share this name; build-ci uploads each as an artifact of the same name.
REPORT_NAME = "0-sizes"


def load_records(records_dir):
    """Read every sizes/<board>.json under records_dir, keyed by board."""
    records = {}
    for path in sorted(pathlib.Path(records_dir).rglob("*.json")):
        with path.open("r") as f:
            record = json.load(f)
        if not isinstance(record, dict) or "languages" not in record:
            print(f"Skipping {path}: not a size record", file=sys.stderr)
            continue
        records[record["board"]] = record
    return records


def summarize(record):
    """Add the derived fields the table and the summary sort and colour by."""
    region = record.get("region")
    languages = record["languages"]
    board = {
        "port": record["port"],
        "board": record["board"],
        "region": region,
        "en_US": (languages.get("en_US") or {}).get("used"),
        "largest_language": None,
        "largest_used": None,
        "largest_source": None,
        "min_free": None,
        "pct": None,
        "measured": sum(1 for b in languages.values() if b["source"] == "measured"),
        "predicted": sum(1 for b in languages.values() if b["source"] == "predicted"),
        "failed": sorted(lang for lang, b in languages.items() if b["status"] == "failed"),
        "languages": languages,
    }
    sized = [(b["used"], lang) for lang, b in languages.items() if b["used"] is not None]
    if sized:
        used, lang = max(sized)
        board["largest_language"] = lang
        board["largest_used"] = used
        board["largest_source"] = languages[lang]["source"]
        if region:
            board["min_free"] = region - used
            board["pct"] = round(100.0 * used / region, 2)
    return board


def run_info(version):
    """Where the numbers came from, from the variables GitHub Actions sets."""
    env = os.environ
    info = {
        "version": version,
        "repository": env.get("GITHUB_REPOSITORY"),
        "run_id": env.get("GITHUB_RUN_ID"),
        "event": env.get("GITHUB_EVENT_NAME"),
        "ref": env.get("GITHUB_REF_NAME"),
        "sha": env.get("GITHUB_SHA"),
        "url": None,
    }
    if info["repository"] and info["run_id"]:
        server = env.get("GITHUB_SERVER_URL", "https://github.com")
        info["url"] = f"{server}/{info['repository']}/actions/runs/{info['run_id']}"
    return info


def describe_run(info):
    """One sentence for the report header."""
    if not info["url"]:
        return "Built outside GitHub Actions."
    parts = [f"{info['event']} build" if info["event"] else "build"]
    if info["version"]:
        parts.append(f"of CircuitPython {info['version']}")
    elif info["ref"]:
        parts.append(f"of {info['ref']}")
    if info["sha"]:
        parts.append(f"at {info['sha'][:10]}")
    return " ".join(parts)


def sort_key(board):
    # Fullest first; boards without a measurable region go last, alphabetically.
    return (board["pct"] is None, -(board["pct"] or 0), board["board"])


def write_json(boards, info, out_dir):
    document = {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "run": info,
        "boards": boards,
    }
    with (out_dir / f"{REPORT_NAME}.json").open("w") as f:
        json.dump(document, f, indent=1)
        f.write("\n")


HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Firmware Sizes</title>
<style>
:root { --bg:#fff; --fg:#1b1b1b; --muted:#666; --line:#ddd; --head:#f3f3f3; --hover:#f7f7ff;
  --free256:#f8d0d0; --free1k:#fbe3c8; --free4k:#fdf3c4; --free16k:#eef6dc; --bad:#b00020; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg:#151515; --fg:#e6e6e6;
  --muted:#9a9a9a; --line:#333; --head:#222; --hover:#22263a;
  --free256:#5a2323; --free1k:#5a3d1e; --free4k:#54501e; --free16k:#2b4222; --bad:#ff6b6b; } }
:root[data-theme="dark"] { --bg:#151515; --fg:#e6e6e6; --muted:#9a9a9a; --line:#333; --head:#222;
  --hover:#22263a; --free256:#5a2323; --free1k:#5a3d1e; --free4k:#54501e; --free16k:#2b4222; --bad:#ff6b6b; }
body { margin:0; padding:16px; background:var(--bg); color:var(--fg); font:14px/1.4 system-ui, sans-serif; }
h1 { font-size:20px; margin:0 0 4px; }
p.meta { color:var(--muted); margin:0 0 12px; }
a { color:inherit; }
.controls { display:flex; flex-wrap:wrap; gap:12px; align-items:center; margin-bottom:12px; }
input, select { font:inherit; padding:4px 8px; border:1px solid var(--line); border-radius:4px;
  background:var(--bg); color:var(--fg); }
.summary { display:grid; grid-template-columns: repeat(auto-fill, minmax(180px,1fr)); gap:8px; margin-bottom:16px; }
.summary div { border:1px solid var(--line); border-radius:6px; padding:8px; cursor:pointer; }
.summary div:hover { background:var(--hover); }
.summary div.selected { border-color:var(--fg); }
.summary div.empty { color:var(--muted); }
.summary b { display:block; }
.summary span { color:var(--muted); font-size:12px; }
.wrap { overflow-x:auto; }
table { border-collapse:collapse; width:100%; }
th, td { padding:4px 8px; border-bottom:1px solid var(--line); white-space:nowrap; }
th { background:var(--head); text-align:left; cursor:pointer; position:sticky; top:0; user-select:none; }
th.num, td.num { text-align:right; font-variant-numeric: tabular-nums; }
th .arrow { color:var(--muted); font-size:11px; margin-left:4px; }
tr:hover td { background:var(--hover); }
tr.free256 td.free { background:var(--free256); } tr.free1k td.free { background:var(--free1k); }
tr.free4k td.free { background:var(--free4k); } tr.free16k td.free { background:var(--free16k); }
td .failed { color:var(--bad); font-size:12px; margin-left:6px; }
td .predicted { color:var(--muted); }
/* The colour key doubles as the free-flash filter: radio buttons drawn as coloured chips. */
.steps { margin-bottom:12px; }
.steps label { display:inline-block; position:relative; padding:2px 8px; margin:0 6px 4px 0; border-radius:4px;
  border:1px solid var(--line); cursor:pointer; font-size:13px; }
.steps input { position:absolute; opacity:0; width:0; height:0; margin:0; }
.steps label:has(input:checked) { border-color:var(--fg); font-weight:600; }
.steps label:has(input:focus-visible) { outline:2px solid var(--fg); outline-offset:1px; }
.steps .free256 { background:var(--free256); } .steps .free1k { background:var(--free1k); }
.steps .free4k { background:var(--free4k); } .steps .free16k { background:var(--free16k); }
</style>
</head>
<body>
<h1>Firmware sizes</h1>
<p class="meta">__RUN__ __COUNTS__
"Largest" is the language build that uses the most flash, which is what has to fit.
A size in <span class="predicted">grey</span> was predicted for a language that was not built.
Click a column header to sort.</p>
<div class="controls">
  <input id="filter" type="search" placeholder="Filter board or port" size="32">
  <span id="count" class="meta"></span>
</div>
<div class="steps" id="free">Free flash:
  <label class="free256"><input type="radio" name="free" value="256"> under 256 B</label>
  <label class="free1k"><input type="radio" name="free" value="1024"> under 1 KiB</label>
  <label class="free4k"><input type="radio" name="free" value="4096"> under 4 KiB</label>
  <label class="free16k"><input type="radio" name="free" value="16384"> under 16 KiB</label>
  <label><input type="radio" name="free" value="" checked> all</label>
</div>
<div class="summary" id="summary"></div>
<div class="wrap"><table id="t">
<thead><tr>
<th data-k="port">Port<span class="arrow"></span></th>
<th data-k="board">Board<span class="arrow"></span></th>
<th data-k="region" class="num">Region<span class="arrow"></span></th>
<th data-k="en_US" class="num">en_US used<span class="arrow"></span></th>
<th data-k="largest_language">Largest lang<span class="arrow"></span></th>
<th data-k="largest_used" class="num">Largest used<span class="arrow"></span></th>
<th data-k="min_free" class="num">Min free<span class="arrow"></span></th>
<th data-k="pct" class="num">% full<span class="arrow"></span></th>
<th data-k="measured" class="num" title="Languages built + languages predicted">Langs<span class="arrow"></span></th>
</tr></thead>
<tbody></tbody>
</table></div>
<script>
const DATA = __DATA__;
const cols = ["port","board","region","en_US","largest_language","largest_used","min_free","pct","measured"];
const numeric = new Set(["region","en_US","largest_used","min_free","pct","measured"]);
let sortKey = "pct", sortDir = -1;
let port = "";  // Port tile clicked to filter on, or "" for all ports.
const tbody = document.querySelector("#t tbody");
const esc = s => String(s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
const fmt = n => n == null ? "" : n.toLocaleString("en-US");
const kib = n => (n % 1024 ? (n / 1024).toFixed(2).replace(/\.?0+$/, "") : n / 1024) + " KiB";
const STEPS = [[256, "free256", "under 256 B"], [1024, "free1k", "under 1 KiB"], [4096, "free4k", "under 4 KiB"], [16384, "free16k", "under 16 KiB"]];
function step(free) { return free == null ? null : STEPS.find(s => free < s[0]) || null; }
function visible(anyPort) {
  const q = document.getElementById("filter").value.trim().toLowerCase();
  const free = document.querySelector('input[name="free"]:checked').value;
  return DATA.filter(d => (anyPort || !port || d.port === port) && (!free || (d.min_free != null && d.min_free < free)) &&
    (!q || (d.board + " " + d.port).toLowerCase().includes(q)));
}
function cell(d, k) {
  if (k === "board") return esc(d.board) + (d.failed.length ? `<span class="failed">failed: ${esc(d.failed.join(" "))}</span>` : "");
  if (k === "pct") return d.pct == null ? "" : d.pct.toFixed(2);
  if (k === "region") return d.region ? kib(d.region) : "";
  if (k === "measured") return d.measured + (d.predicted ? ` + ${d.predicted}` : "");
  if (k === "largest_used" && d.largest_source === "predicted") return `<span class="predicted">${fmt(d[k])}</span>`;
  return numeric.has(k) ? fmt(d[k]) : esc(d[k] == null ? "" : d[k]);
}
function render() {
  const rows = visible().slice().sort((a, b) => {
    let x = a[sortKey], y = b[sortKey];
    if (x == null && y == null) return a.board.localeCompare(b.board);
    if (x == null) return 1; if (y == null) return -1;
    if (numeric.has(sortKey)) return (x - y) * sortDir || a.board.localeCompare(b.board);
    return String(x).localeCompare(String(y)) * sortDir || a.board.localeCompare(b.board);
  });
  tbody.innerHTML = rows.map(d => `<tr class="${(step(d.min_free) || [])[1] || ""}">` + cols.map(k =>
    `<td class="${numeric.has(k) ? "num " : ""}${k === "min_free" ? "free" : ""}">${cell(d, k)}</td>`
  ).join("") + "</tr>").join("");
  document.getElementById("count").textContent = rows.length + " of " + DATA.length + " boards";
  document.querySelectorAll("th").forEach(th => th.querySelector(".arrow").textContent =
    th.dataset.k === sortKey ? (sortDir < 0 ? "\u25BC" : "\u25B2") : "");
  // The tiles are the port selector. Every port gets one, and their counts follow the search
  // and free-flash filters but not the port selection, so each tile says what clicking it shows.
  const count = () => ({n:0, steps:STEPS.map(() => 0)});
  const sum = {"": count()};
  DATA.forEach(d => sum[d.port] ||= count());
  visible(true).forEach(d => { for (const s of [sum[""], sum[d.port]]) {
    s.n++; const i = STEPS.indexOf(step(d.min_free)); if (i >= 0) s.steps[i]++; } });
  document.getElementById("summary").innerHTML = Object.keys(sum).sort().map(f => {
    const parts = STEPS.map((s, i) => sum[f].steps[i] ? `${sum[f].steps[i]} ${s[2]}` : "").filter(Boolean);
    const classes = [f === port ? "selected" : "", sum[f].n ? "" : "empty"].filter(Boolean).join(" ");
    return `<div data-port="${esc(f)}" class="${classes}" title="${f ? "Show only this port" : "Show every port"}">` +
      `<b>${f ? esc(f) : "All ports"}</b>${sum[f].n} boards<br><span>${parts.join(", ") || "none under 16 KiB"}</span></div>`;
  }).join("");
}
document.querySelectorAll("th").forEach(th => th.addEventListener("click", () => {
  const k = th.dataset.k;
  if (sortKey === k) sortDir = -sortDir; else { sortKey = k; sortDir = numeric.has(k) ? -1 : 1; }
  render();
}));
["filter","free"].forEach(id => document.getElementById(id).addEventListener("input", render));
document.getElementById("summary").addEventListener("click", e => {
  const tile = e.target.closest("[data-port]");
  if (!tile) return;
  port = port === tile.dataset.port ? "" : tile.dataset.port;
  render();
});
render();
</script>
</body>
</html>
"""


def write_html(boards, info, out_dir):
    if info["url"]:
        run = 'From GitHub Actions <a href="{url}">run {run_id}</a> ({desc}).'.format(
            url=html.escape(info["url"], quote=True),
            run_id=html.escape(info["run_id"]),
            desc=html.escape(describe_run(info)),
        )
    else:
        run = html.escape(describe_run(info))
    with_region = sum(1 for b in boards if b["region"])
    counts = f"{len(boards)} boards, {with_region} with a measurable flash region."
    # The table rows carry only what the page shows; the per-language detail is in
    # the JSON report. "</" must not appear inside a script element.
    table = [{k: v for k, v in b.items() if k != "languages"} for b in boards]
    data = json.dumps(table, separators=(",", ":")).replace("</", "<\\/")
    page = (
        HTML_TEMPLATE.replace("__RUN__", run)
        .replace("__COUNTS__", counts)
        .replace("__DATA__", data)
    )
    with (out_dir / f"{REPORT_NAME}.html").open("w") as f:
        f.write(page)


def markdown_summary(boards):
    """One line for the job's step summary; the detail is in the reports."""
    if not boards:
        return "No boards were built in this run, so there is no firmware size report.\n"
    with_region = sum(1 for b in boards if b["pct"] is not None)
    return (
        f"{len(boards)} boards built, {with_region} with a measurable flash region. "
        f"The complete firmware size reports are in the `{REPORT_NAME}.html` and "
        f"`{REPORT_NAME}.json` artifacts below.\n"
    )


FREE_STEPS = [(256, "<256 B"), (1024, "<1 KiB"), (4096, "<4 KiB"), (16384, "<16 KiB")]


def print_ports(boards):
    """Per-port counts of boards with little flash left, for the job log."""
    ports = collections.defaultdict(collections.Counter)
    for b in boards:
        c = ports[b["port"]]
        c["boards"] += 1
        if b["min_free"] is None:
            continue
        for limit, label in FREE_STEPS:
            if b["min_free"] < limit:
                c[label] += 1
                break
    labels = [label for _, label in FREE_STEPS]
    print(f"{'port':16} {'boards':>6}" + "".join(f" {label:>8}" for label in labels))
    for name in sorted(ports):
        c = ports[name]
        print(f"{name:16} {c['boards']:6}" + "".join(f" {c[label]:8}" for label in labels))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("records_dir", help="directory holding the per-board size records")
    parser.add_argument("out_dir", help="where to write the JSON and HTML reports")
    parser.add_argument(
        "--version",
        default=os.environ.get("CP_VERSION"),
        help="CircuitPython version named in the reports (default: $CP_VERSION)",
    )
    parser.add_argument(
        "--summary",
        default=os.environ.get("GITHUB_STEP_SUMMARY"),
        help="append the markdown summary to this file (default: $GITHUB_STEP_SUMMARY)",
    )
    args = parser.parse_args()

    records = load_records(args.records_dir)
    boards = sorted((summarize(r) for r in records.values()), key=sort_key)
    info = run_info(args.version)

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    write_json(boards, info, out_dir)
    write_html(boards, info, out_dir)

    summary = markdown_summary(boards)
    if args.summary:
        with open(args.summary, "a") as f:
            f.write(summary)
    else:
        print(summary)
    print_ports(boards)


if __name__ == "__main__":
    main()
