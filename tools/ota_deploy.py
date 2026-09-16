#!/usr/bin/env python3
"""Deploy helper for the Tikk OTA demo (Golioth REST API).

Stdlib only; reads the API key from ~/.golioth/api-key (chmod 600).

Usage:
    python3 ota_deploy.py list
    python3 ota_deploy.py upload 0.1.0 ~/golioth/microros/build-44-mcu/app/zephyr/zephyr.signed.bin
    python3 ota_deploy.py deploy 0.1.0 --name "demo-downgrade"

- upload: POSTs the signed binary as an artifact of package "main"
  (JSON body, content = base64 — the format Golioth's REST expects).
- deploy: creates a NEW deployment in cohort "tikk-demo" with the
  artifact for that version. Deployments are immutable — each toggle
  (0.1.0 <-> 0.2.0) is a fresh deployment; the cohort's active
  deployment is the desired state devices converge to on check-in.

Device-side reminder: the manifest arrives in one pouch session, the
image data in the NEXT one — so press the sync button (P0.11) twice,
~1 min apart, and watch the LED bar during the second session.
"""
import argparse
import base64
import json
import os
import sys
import urllib.request
import urllib.error
import ssl

BASE = "https://api.golioth.io"
ORG = "chris-gammell"
PROJECT = "connect-demo"
PACKAGE = "main"
COHORT = "tikk-demo"
KEYFILE = os.path.expanduser("~/.golioth/api-key")

CTX = ssl.create_default_context()


def api(path, method="GET", payload=None):
    key = open(KEYFILE).read().strip()
    headers = {"x-api-key": key}
    data = None
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{BASE}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=120, context=CTX) as r:
            return r.status, json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        try:
            return e.code, json.loads(body)
        except Exception:
            return e.code, body[:400]


def cmd_list(_):
    # NOTE: always pass pageSize — the API returns an empty list without it.
    st, arts = api(f"/v1/projects/{PROJECT}/artifacts?pageSize=100")
    print(f"artifacts ({st}):")
    for a in (arts.get("list") or []):
        size = (a.get("binaryInfo") or {}).get("size")
        print(f"  {a.get('version'):>10}  id={a.get('id')}  size={size}")
    st, deps = api(f"/v1/organizations/{ORG}/projects/{PROJECT}/cohorts/{COHORT}/deployments?pageSize=100")
    print(f"deployments in cohort {COHORT} ({st}):")
    for d in (deps.get("list") or []):
        print(f"  {d.get('createdAt')}  {d.get('name')}  artifacts={d.get('artifactIds')}")


def cmd_upload(args):
    content = base64.b64encode(open(args.bin, "rb").read()).decode()
    st, resp = api("/v1/artifacts", "POST", {
        "projectId": PROJECT,
        "package": PACKAGE,
        "version": args.version,
        "content": content,
    })
    if st != 200:
        print(f"upload FAILED ({st}): {json.dumps(resp)[:300]}")
        sys.exit(1)
    art = resp.get("data") or {}
    print(f"uploaded {args.bin} -> artifact id={art.get('id')} "
          f"size={(art.get('binaryInfo') or {}).get('size')}")


def cmd_deploy(args):
    # find the artifact id for the requested version
    st, arts = api(f"/v1/projects/{PROJECT}/artifacts?pageSize=100")
    match = [a for a in (arts.get("list") or []) if a.get("version") == args.version]
    if not match:
        print(f"no artifact with version {args.version!r} — run upload first (see list)")
        sys.exit(1)
    aid = match[0]["id"]
    st, dep = api(
        f"/v1/organizations/{ORG}/projects/{PROJECT}/cohorts/{COHORT}/deployments",
        "POST",
        {"name": args.name or f"{PACKAGE}-{args.version}", "artifactIds": [aid]},
    )
    if st not in (200, 201):
        print(f"deploy FAILED ({st}): {json.dumps(dep)[:300]}")
        sys.exit(1)
    d = dep.get("data") or {}
    print(f"deployment created: {d.get('name')} id={d.get('deploymentId')} artifacts={d.get('artifactIds')}")
    print("now: press the Tikk sync button (P0.11) — twice, ~1 min apart (manifest session, then data session)")


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    up = sub.add_parser("upload")
    up.add_argument("version")
    up.add_argument("bin")
    dp = sub.add_parser("deploy")
    dp.add_argument("version")
    dp.add_argument("--name", default=None)
    args = p.parse_args()
    {"list": cmd_list, "upload": cmd_upload, "deploy": cmd_deploy}[args.cmd](args)


if __name__ == "__main__":
    main()