#!/usr/bin/env python3
"""Publish a LevelUp-NX release that carries ONLY `LevelUp-NX-vX.Y.Z.nro`.

Why this exists: the release page of this fork once carried the upstream
Moonlight-Switch "All Builds" set (Android / iOS / Linux / macOS / PSVita /
SteamOS / Switch2 / Windows, ~860 MB per release). None of those binaries are
built from this tree, so they were removed. This script keeps it that way:

  * creates (or reuses) the release for a `vX.Y.Z` tag,
  * deletes every asset of that release,
  * uploads the freshly built nro as `LevelUp-NX-vX.Y.Z.nro`,
  * verifies the release ends up with exactly that one asset.

The token comes from the git credential manager (Git Credential Manager), so no
PAT is ever written to disk. Network goes through HTTPS_PROXY when set; the
local Clash proxy below is only a convenience default for this machine.

Usage:
    python scripts/ci/publish_release.py --tag v0.2.0            # dry run
    python scripts/ci/publish_release.py --tag v0.2.0 --apply    # publish
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

from collect_release import asset_name

DEFAULT_REPO = "John-L-CN/levelup-switch"
DEFAULT_PROXY = "http://127.0.0.1:7896"
RETRIES = 4

DEFAULT_NOTES = """## LevelUp-NX {tag}（Nintendo Switch 1 原生客户端）

### 安装
1. 下载 `{asset}`
2. 放到 SD 卡：`sdmc:/switch/LevelUp-NX/{asset}`（文件名带版本号，方便回滚）
3. 按住 `R` 打开任意已安装游戏进入 hbmenu（全内存模式），再启动本程序

> Release 只提供这一个 `{asset}`；大厅里的游戏封面/手柄/震动/音频全部内置。
"""


def github_token() -> str:
    proc = subprocess.Popen(
        ["git", "credential", "fill"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    out, _ = proc.communicate("protocol=https\nhost=github.com\n\n")
    for line in out.splitlines():
        if line.startswith("password="):
            return line.split("=", 1)[1]
    raise RuntimeError("git credential manager has no GitHub credential")


class Api:
    def __init__(self, repo: str, token: str):
        proxy = os.environ.get("HTTPS_PROXY") or os.environ.get("https_proxy") or DEFAULT_PROXY
        handlers = []
        if proxy:
            handlers.append(urllib.request.ProxyHandler({"http": proxy, "https": proxy}))
        self.opener = urllib.request.build_opener(*handlers)
        self.repo = repo
        self.token = token

    def request(self, method: str, path: str, payload=None, raw=None, ctype=None):
        url = path if path.startswith("https://") else "https://api.github.com" + path
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "User-Agent": "levelup-nx-publish",
        }
        data = raw
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if ctype:
            headers["Content-Type"] = ctype
        last = None
        for attempt in range(RETRIES):
            req = urllib.request.Request(url, data=data, headers=headers, method=method)
            try:
                with self.opener.open(req, timeout=180) as resp:
                    body = resp.read().decode("utf-8", "replace")
                    return resp.status, (json.loads(body) if body.strip() else None)
            except urllib.error.HTTPError as exc:
                if exc.code in (401, 403, 404, 422):
                    detail = exc.read().decode("utf-8", "replace")[:400]
                    raise RuntimeError(f"{method} {url} -> HTTP {exc.code}: {detail}") from None
                last = exc
            except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
                last = exc
            time.sleep(1.5 * (attempt + 1))
        raise RuntimeError(f"{method} {url} failed after {RETRIES} attempts: {last}")


def resolve_nro(explicit):
    if explicit is not None:
        return explicit
    candidates = sorted(
        Path("build/switch").glob("LevelUp-NX*.nro"),
        key=lambda path: path.stat().st_mtime,
    )
    return candidates[-1] if candidates else Path("build/switch/LevelUp-NX.nro")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True, help="vX.Y.Z")
    parser.add_argument(
        "--nro",
        type=Path,
        help="Switch build to publish (default: newest build/switch/LevelUp-NX*.nro)",
    )
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--notes", type=Path, help="markdown file for the release notes")
    parser.add_argument(
        "--prerelease",
        action="store_true",
        help="mark the release as a pre-release (experimental build)",
    )
    parser.add_argument("--apply", action="store_true", help="actually touch GitHub")
    args = parser.parse_args()

    nro = resolve_nro(args.nro)
    if not nro.is_file():
        print(f"error: {nro} not found (build it first)", file=sys.stderr)
        return 1

    asset = asset_name(args.tag)
    body = (
        args.notes.read_text(encoding="utf-8")
        if args.notes
        else DEFAULT_NOTES.format(tag=args.tag, asset=asset)
    )

    if not args.apply:
        print(f"[dry-run] release {args.tag} on {args.repo}")
        print(f"[dry-run] release will contain exactly '{asset}'")
        print(f"[dry-run] upload {nro} ({nro.stat().st_size:,} bytes) as '{asset}'")
        return 0

    api = Api(args.repo, github_token())

    release = None
    try:
        _, release = api.request("GET", f"/repos/{args.repo}/releases/tags/{args.tag}")
    except RuntimeError as exc:
        if "404" not in str(exc):
            raise
    if release:
        print(f"reusing release {args.tag} (id={release['id']})")
        api.request(
            "PATCH",
            f"/repos/{args.repo}/releases/{release['id']}",
            payload={
                "name": f"LevelUp-NX {args.tag}",
                "body": body,
                "draft": False,
                "prerelease": bool(args.prerelease),
            },
        )
    else:
        print(f"creating release {args.tag}")
        _, release = api.request(
            "POST",
            f"/repos/{args.repo}/releases",
            payload={
                "tag_name": args.tag,
                "target_commitish": "main",
                "name": f"LevelUp-NX {args.tag}",
                "body": body,
                "draft": False,
                "prerelease": bool(args.prerelease),
            },
        )

    # 1. Drop whatever the release currently carries, then upload the fresh build.
    _, current = api.request("GET", f"/repos/{args.repo}/releases/{release['id']}")
    for old in current.get("assets", []):
        status, _ = api.request("DELETE", f"/repos/{args.repo}/releases/assets/{old['id']}")
        print(f"  removed {old['name']} (HTTP {status})")

    upload_base = release["upload_url"].split("{")[0]
    data = nro.read_bytes()
    status, info = api.request(
        "POST",
        f"{upload_base}?name={asset}",
        raw=data,
        ctype="application/octet-stream",
    )
    print(f"uploaded {asset} ({len(data):,} bytes, HTTP {status}, id={info['id']})")

    # 2. Verify the release really carries exactly that one asset.
    _, final = api.request("GET", f"/repos/{args.repo}/releases/{release['id']}")
    names = [a["name"] for a in final.get("assets", [])]
    print(f"release {args.tag} assets: {names}")
    if names != [asset]:
        print(f"error: expected exactly ['{asset}']", file=sys.stderr)
        return 1
    print(f"done: https://github.com/{args.repo}/releases/tag/{args.tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
