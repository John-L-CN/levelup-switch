#!/usr/bin/env python3
"""One-shot helper: list releases, delete a release, verify the final state."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from publish_release import DEFAULT_REPO, Api, github_token  # noqa: E402


def main() -> int:
    api = Api(DEFAULT_REPO, github_token())
    action = sys.argv[1] if len(sys.argv) > 1 else "list"

    if action == "delete":
        tag = sys.argv[2]
        _, releases = api.request("GET", f"/repos/{DEFAULT_REPO}/releases?per_page=100")
        hit = [r for r in releases if r["tag_name"] == tag]
        if not hit:
            print(f"no release for {tag}")
            return 0
        for rel in hit:
            api.request("DELETE", f"/repos/{DEFAULT_REPO}/releases/{rel['id']}")
            print(f"deleted release {tag} (id={rel['id']})")

    _, releases = api.request("GET", f"/repos/{DEFAULT_REPO}/releases?per_page=100")
    print(f"releases = {len(releases)}")
    for rel in releases:
        names = [a["name"] for a in rel.get("assets", [])]
        print(f"  {rel['tag_name']!r}  name={rel['name']!r}  assets={names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
