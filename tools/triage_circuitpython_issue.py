#!/usr/bin/env python3
"""Fetch and triage a CircuitPython GitHub issue/PR via pi RPC mode."""

from __future__ import annotations

import argparse
import json
import queue
import subprocess
import sys
import threading
import uuid
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_REPO = "adafruit/circuitpython"


class PiRpcClient:
    def __init__(
        self, repo_root: Path, provider: str | None, model: str | None, thinking: str | None
    ):
        cmd = ["pi", "--mode", "rpc", "--no-extensions", "--no-skills", "--no-prompt-templates"]
        if provider:
            cmd.extend(["--provider", provider])
        if model:
            cmd.extend(["--model", model])
        if thinking:
            cmd.extend(["--thinking", thinking])

        self.proc = subprocess.Popen(
            cmd,
            cwd=repo_root,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        if not self.proc.stdin or not self.proc.stdout:
            raise RuntimeError("Failed to start pi RPC process")

        self._queue: queue.Queue[dict[str, Any]] = queue.Queue()
        self._reader = threading.Thread(target=self._read_stdout, daemon=True)
        self._reader.start()

    def _read_stdout(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                self._queue.put(json.loads(line))
            except json.JSONDecodeError:
                # Ignore non-JSON lines.
                continue

    def send(self, payload: dict[str, Any]) -> str:
        req_id = str(uuid.uuid4())
        payload = dict(payload)
        payload["id"] = req_id
        assert self.proc.stdin is not None
        self.proc.stdin.write(json.dumps(payload) + "\n")
        self.proc.stdin.flush()
        return req_id

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def run_gh_api(path: str) -> Any:
    result = subprocess.run(
        ["gh", "api", path],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"gh api failed ({path}): {result.stderr.strip()}")
    return json.loads(result.stdout)


def shorten(text: str | None, limit: int = 4000) -> str:
    if not text:
        return ""
    text = text.strip()
    if len(text) <= limit:
        return text
    return text[:limit] + "\n...[truncated]..."


def fetch_issue_context(repo: str, number: int, max_comments: int) -> str:
    issue = run_gh_api(f"repos/{repo}/issues/{number}")
    comments = run_gh_api(f"repos/{repo}/issues/{number}/comments")
    comments = comments[-max_comments:] if max_comments > 0 else []

    pr_data = None
    pr_files = None
    if issue.get("pull_request"):
        pr_data = run_gh_api(f"repos/{repo}/pulls/{number}")
        pr_files = run_gh_api(f"repos/{repo}/pulls/{number}/files")

    lines: list[str] = []
    labels = [l["name"] for l in issue.get("labels", [])]
    lines.extend(
        [
            f"Repository: {repo}",
            f"Number: #{issue['number']}",
            f"Type: {'PR' if pr_data else 'Issue'}",
            f"Title: {issue.get('title', '')}",
            f"State: {issue.get('state', '')}",
            f"URL: {issue.get('html_url', '')}",
            f"Author: {issue.get('user', {}).get('login', '')}",
            f"Created: {issue.get('created_at', '')}",
            f"Updated: {issue.get('updated_at', '')}",
            f"Labels: {', '.join(labels) if labels else '(none)'}",
            "",
            "Issue Body:",
            shorten(issue.get("body"), 12000) or "(empty)",
            "",
            f"Comments (latest {len(comments)}):",
        ]
    )

    for idx, comment in enumerate(comments, 1):
        lines.extend(
            [
                f"--- Comment {idx} by {comment.get('user', {}).get('login', '')} at {comment.get('created_at', '')} ---",
                shorten(comment.get("body"), 3000) or "(empty)",
            ]
        )

    if pr_data:
        lines.extend(
            [
                "",
                "PR Details:",
                f"Draft: {pr_data.get('draft')}",
                f"Merged: {pr_data.get('merged')}",
                f"Mergeable state: {pr_data.get('mergeable_state')}",
                f"Base: {pr_data.get('base', {}).get('ref')}",
                f"Head: {pr_data.get('head', {}).get('ref')} ({pr_data.get('head', {}).get('repo', {}).get('full_name')})",
                f"Changed files: {pr_data.get('changed_files')}",
                f"Additions/Deletions: {pr_data.get('additions')}/{pr_data.get('deletions')}",
                "",
                "PR File List:",
            ]
        )
        for f in pr_files or []:
            lines.append(
                f"- {f.get('filename')} (+{f.get('additions')}/-{f.get('deletions')}, {f.get('status')})"
            )

    return "\n".join(lines)


def build_prompt(issue_context: str, issue_number: int, apply_fix: bool) -> str:
    maybe_fix = (
        "If the required code change is minimal and low-risk, create a new branch from main, implement it, and commit. "
        "If not minimal, explain why no code changes were made."
        if apply_fix
        else "Do not modify files or branches; analysis only."
    )

    return f"""
You are triaging CircuitPython GitHub issue/PR #{issue_number} in a local CircuitPython checkout.

Use repository files and git history as needed. Please provide:
1) One-line summary.
2) Exact hardware needed to test.
3) A candidate code.py test script.
4) What native_sim (Zephyr) tests would be needed.
5) Whether this appears already fixed in git history (show evidence: commits/files).
6) Additional labels to add.
7) If not fixed, a concrete plan to fix.
8) {maybe_fix}

When checking if fixed already, focus git history on likely relevant paths/files and cite commit hashes.

Return results in sections numbered 1-8.

GitHub context:
{issue_context}
""".strip()


def wait_for_response(client: PiRpcClient, req_id: str) -> dict[str, Any]:
    while True:
        msg = client._queue.get()
        if msg.get("type") == "response" and msg.get("id") == req_id:
            return msg


def rpc_command(client: PiRpcClient, payload: dict[str, Any]) -> dict[str, Any]:
    req_id = client.send(payload)
    resp = wait_for_response(client, req_id)
    if not resp.get("success"):
        raise RuntimeError(f"pi {payload.get('type')} failed: {resp.get('error')}")
    return resp.get("data") or {}


def run_agent_prompt(client: PiRpcClient, prompt: str) -> str:
    rpc_command(client, {"type": "prompt", "message": prompt})

    # Stream text deltas live while waiting for completion.
    while True:
        msg = client._queue.get()
        if msg.get("type") == "message_update":
            evt = msg.get("assistantMessageEvent", {})
            if evt.get("type") == "text_delta":
                sys.stdout.write(evt.get("delta", ""))
                sys.stdout.flush()
        elif msg.get("type") == "agent_end":
            break

    sys.stdout.write("\n")

    return rpc_command(client, {"type": "get_last_assistant_text"}).get("text") or ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("issue", type=int, help="CircuitPython issue/PR number")
    parser.add_argument(
        "--repo", default=DEFAULT_REPO, help="GitHub repo (default: adafruit/circuitpython)"
    )
    parser.add_argument(
        "--max-comments", type=int, default=15, help="Include up to this many latest comments"
    )
    parser.add_argument("--provider", help="pi provider")
    parser.add_argument("--model", help="pi model")
    parser.add_argument("--thinking", help="pi thinking level")
    parser.add_argument(
        "--apply-fix",
        action="store_true",
        help="Allow pi to create branch and implement minimal fix",
    )
    parser.add_argument(
        "--log-dir",
        default=".triage-reports",
        help="Directory (relative to repo root) for exported pi HTML logs",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    if not (repo_root / ".git").exists():
        raise RuntimeError(f"Could not find repository root at {repo_root}")

    context = fetch_issue_context(args.repo, args.issue, args.max_comments)
    prompt = build_prompt(context, args.issue, args.apply_fix)

    print(f"Fetched #{args.issue} from {args.repo}. Starting pi triage...", file=sys.stderr)
    if args.apply_fix:
        print(
            "Note: --apply-fix enabled. Agent will decide whether to create a branch from main and commit minimal changes.",
            file=sys.stderr,
        )

    export_dir = repo_root / args.log_dir
    export_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    export_path = export_dir / f"pi-triage-{args.issue}-{ts}.html"

    client = PiRpcClient(
        repo_root=repo_root, provider=args.provider, model=args.model, thinking=args.thinking
    )
    try:
        final_text = run_agent_prompt(client, prompt)
        stats = rpc_command(client, {"type": "get_session_stats"})
        rpc_command(client, {"type": "export_html", "outputPath": str(export_path)})
    finally:
        client.close()

    cost = stats.get("cost")
    print("\n===== Final triage report =====\n")
    print(final_text)
    print("\n===== Metadata =====")
    if isinstance(cost, (int, float)):
        print(f"Analysis cost: ${cost:.6f}")
    else:
        print(f"Analysis cost: {cost}")
    print(f"pi log export: {export_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
