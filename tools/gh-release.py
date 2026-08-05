#!/usr/bin/env python3
"""Create a GitHub release and upload the zip asset.

Usage:
    python3 tools/gh-release.py [version]       # e.g. 0.1.1

The version defaults to VERSION in the Makefile, which is the same value that
named the zip, so the asset and the tag cannot drift apart.

Auth, in order: $GITHUB_TOKEN, $GH_TOKEN, then `gh auth token` if the GitHub
CLI is logged in. The token is never printed.

Idempotent: re-running reuses the existing release and replaces the asset.

Release notes come from docs/release-notes/v<version>.md when that file exists,
so release text is versioned with the code rather than living only on
github.com. Without it, a minimal generated note is used.

This script lives in tools/ rather than dist/ on purpose: dist/ is gitignored,
and an earlier copy of it here was never committed at all.
"""
import json, os, re, subprocess, sys, urllib.request, urllib.error

REPO = "zoitrok/hypr-miyoo"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def makefile_version():
    with open(os.path.join(ROOT, "Makefile")) as f:
        m = re.search(r"^VERSION\s*:=\s*(\S+)", f.read(), re.M)
    if not m:
        sys.exit("Could not read VERSION from the Makefile.")
    return m.group(1)


def read_token():
    tok = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if tok:
        return tok
    try:
        out = subprocess.run(["gh", "auth", "token"], capture_output=True,
                             text=True, check=True).stdout.strip()
        if out:
            return out
    except (OSError, subprocess.CalledProcessError):
        pass
    sys.exit("No token. Set GITHUB_TOKEN (contents:write / classic 'repo'), "
             "or log in with `gh auth login`.")


def read_notes(tag):
    path = os.path.join(ROOT, "docs", "release-notes", f"{tag}.md")
    if os.path.exists(path):
        with open(path) as f:
            return f.read()
    return f"## HYPR demoscene radio — {tag}\n"

def api(method, url, token, data=None, headers=None, binary=False):
    h = {"Authorization": f"Bearer {token}",
         "Accept": "application/vnd.github+json",
         "X-GitHub-Api-Version": "2022-11-28"}
    if headers:
        h.update(headers)
    body = data if binary else (json.dumps(data).encode() if data is not None else None)
    req = urllib.request.Request(url, data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")

def main():
    version = sys.argv[1] if len(sys.argv) > 1 else makefile_version()
    version = version.lstrip("v")
    tag = f"v{version}"
    name = f"HYPR demoscene radio {tag}"
    zip_path = os.path.join(ROOT, "dist", f"hypr-demoscene-radio-{tag}.zip")
    notes = read_notes(tag)

    token = read_token()
    if not os.path.exists(zip_path):
        sys.exit(f"Zip not found: {zip_path}\nRun `make release` first.")

    base = f"https://api.github.com/repos/{REPO}"

    # Find or create the release for the tag.
    st, rel = api("GET", f"{base}/releases/tags/{tag}", token)
    if st == 404:
        st, rel = api("POST", f"{base}/releases", token,
                      {"tag_name": tag, "name": name, "body": notes,
                       "draft": False, "prerelease": False})
        if st >= 300:
            sys.exit(f"Create release failed ({st}): {rel}")
        print(f"Created release {tag}")
    else:
        # Refresh name/notes on an existing release.
        api("PATCH", f"{base}/releases/{rel['id']}", token,
            {"name": name, "body": notes})
        print(f"Reusing existing release {tag}")

    asset_name = os.path.basename(zip_path)
    for a in rel.get("assets", []):
        if a["name"] == asset_name:
            api("DELETE", f"{base}/releases/assets/{a['id']}", token)
            print("Removed previous asset")

    with open(zip_path, "rb") as f:
        blob = f.read()
    up = f"https://uploads.github.com/repos/{REPO}/releases/{rel['id']}/assets?name={asset_name}"
    st, asset = api("POST", up, token, data=blob, binary=True,
                    headers={"Content-Type": "application/zip"})
    if st >= 300:
        sys.exit(f"Asset upload failed ({st}): {asset}")

    print("Uploaded:", asset["browser_download_url"])
    print("Release page:", rel["html_url"])

if __name__ == "__main__":
    main()
