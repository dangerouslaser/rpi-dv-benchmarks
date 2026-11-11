#!/usr/bin/env python3
"""Write the releases.json that LibreELEC's update menu reads, from the
GitHub releases of the LibreELEC tree (needs an authenticated gh).

    scripts/releases-json.py > releases.json

Releases go to the DV-13.0 channel, pre-releases to DVbeta-13.0. In each
channel a larger key is a newer build; a build updates to the newest build
of the channel it is in.
"""
import json
import subprocess
import sys

REPO = "dangerouslaser/libreelec-rpi-dv"
ARCH = "RPi5.aarch64"
CHANNELS = {False: "DV-13.0", True: "DVbeta-13.0"}
URL = f"https://github.com/{REPO}/releases/download/"
# the version in a build's name: LibreELEC-RPi5.aarch64-13.0-dv-0.5.0-beta.5
PRETTYNAME_REGEX = r"-dv-([0-9]+\.[0-9]+\.[0-9]+(?:-[a-z]+\.[0-9]+)?)"


def gh(*args):
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def main():
    releases = json.loads(gh("release", "list", "-R", REPO, "--limit", "200",
                             "--json", "tagName,isPrerelease,isDraft,publishedAt"))
    out = {}
    for release in sorted(releases, key=lambda r: r["publishedAt"]):
        if release["isDraft"]:
            continue
        tag = release["tagName"]
        assets = json.loads(gh("release", "view", tag, "-R", REPO, "--json", "assets"))["assets"]
        names = {a["name"]: a for a in assets}
        entry = {}
        for kind, suffix in (("file", ".tar"), ("image", ".img.gz")):
            asset = next((a for n, a in names.items() if n.endswith(suffix)), None)
            if not asset:
                continue
            sha256 = None
            if asset["name"] + ".sha256" in names:
                sha256 = gh("release", "download", tag, "-R", REPO, "-p",
                            asset["name"] + ".sha256", "-O", "-").split()[0]
            entry[kind] = {
                "name": asset["name"],
                "sha256": sha256,
                "size": str(asset["size"]),
                "subpath": tag,
                "timestamp": release["publishedAt"].replace("T", " ").rstrip("Z"),
            }
        if "file" not in entry:
            continue
        channel = out.setdefault(CHANNELS[release["isPrerelease"]], {
            "prettyname_regex": PRETTYNAME_REGEX,
            "project": {ARCH: {"releases": {}}},
            "url": URL,
        })
        builds = channel["project"][ARCH]["releases"]
        builds[str(len(builds))] = entry
    json.dump(out, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
