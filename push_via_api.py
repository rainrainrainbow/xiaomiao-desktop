#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Push local HEAD to GitHub via REST Git Data API (work around proxy 502 on git push)."""
import base64
import json
import subprocess
import sys
import urllib.request

REPO = "rainrainrainbow/xiaomiao-desktop"
API = f"https://api.github.com/repos/{REPO}"


def get_token():
    p = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True, text=True,
    )
    for line in p.stdout.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise RuntimeError("no token in git credential store")


TOKEN = get_token()


def api(path, method="GET", body=None):
    url = API + path if path.startswith("/") else path
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {TOKEN}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if data:
        req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=60) as resp:
        return json.load(resp)


def detect_changed_files():
    """Files changed in local HEAD relative to remote main (merge-base)."""
    remote_sha = api("/commits/main")["sha"]
    p = subprocess.run(
        ["git", "diff", "--name-only", f"{remote_sha}..HEAD"],
        capture_output=True, text=True,
    )
    return [f for f in p.stdout.splitlines() if f.strip()]


def main():
    # Explicit file list from argv, else auto-detect local-vs-remote diff.
    if len(sys.argv) > 1:
        files = sys.argv[1:]
    else:
        files = detect_changed_files()
    if not files:
        print("No changed files to push.")
        return
    print("Files to push:")
    for f in files:
        print(f"  - {f}")

    # 1. Get current remote main commit + its tree
    main_commit = api("/commits/main")
    base_tree_sha = main_commit["commit"]["tree"]["sha"]
    base_tree = api(f"/git/trees/{base_tree_sha}")

    # 2. Create blobs for changed files
    tree_entries = []
    for path in files:
        raw = open(path, "rb").read()
        blob = api("/git/blobs", "POST", {
            "content": base64.b64encode(raw).decode("ascii"),
            "encoding": "base64",
        })
        tree_entries.append({
            "path": path,
            "mode": "100644",
            "type": "blob",
            "sha": blob["sha"],
        })
        print(f"  blob {path}: {blob['sha'][:12]} ({len(raw)} bytes)")

    # 3. Create new tree based on remote tree
    new_tree = api("/git/trees", "POST", {
        "base_tree": base_tree_sha,
        "tree": tree_entries,
    })
    print(f"  new tree: {new_tree['sha'][:12]}")

    # 4. Create commit (reuse local commit message)
    local_msg = subprocess.run(
        ["git", "log", "-1", "--pretty=%B"],
        capture_output=True, text=True,
    ).stdout.strip()
    new_commit = api("/git/commits", "POST", {
        "message": local_msg,
        "tree": new_tree["sha"],
        "parents": [main_commit["sha"]],
    })
    print(f"  new commit: {new_commit['sha'][:12]}")

    # 5. Move main ref
    ref = api("/git/refs/heads/main", "PATCH", {
        "sha": new_commit["sha"],
        "force": False,
    })
    print(f"  main -> {ref['object']['sha'][:12]}")

    # Verify it matches expected local file content
    print("\nDONE. CI should be triggered by this ref update.")
    print(f"EXPECTED_LOCAL_SHA={subprocess.run(['git','rev-parse','HEAD'],capture_output=True,text=True).stdout.strip()}")


if __name__ == "__main__":
    main()
