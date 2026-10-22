#!/usr/bin/env python3
"""Release notes for the in-app "What's New" box.

The notes are the CHANGELOG.md bullets ADDED since the previous release
tag (`v-<sha>`), newest first, at most 5. Each PR adds its own bullets
under `## Unreleased` (release-notes-check.yml enforces that), so every
build ships exactly what changed since the last one, and a bullet that
already shipped never ships again.

  python3 .github/scripts/release_notes.py            # print the notes
  python3 .github/scripts/release_notes.py --check BASE_REF
      # PR check: exit 1 unless the PR adds at least one bullet and every
      # added bullet follows the style rules.
"""

import re
import subprocess
import sys

CHANGELOG = "CHANGELOG.md"
MAX_BULLETS = 5
MAX_CHARS = 80
FALLBACK = "- Fixes and small improvements"
# Words a roofing contractor should not have to read. A bullet with one of
# these fails the PR check; say what the operator sees or can now do.
JARGON = re.compile(
    r"\b(ROS|QP|MPC|SSH|S3|ETag|SHA|sha256|md5|rsync|JSON|YAML|CI|OTA|API|"
    r"Lambda|ROI|SLAM|LiDAR|Livox|ODrive|Zenoh|CMU|udisks|fsync|syncfs|"
    r"stdout|uploader\.py|\.py|\.cpp|Stage \d)\b",
    re.IGNORECASE,
)


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True,
                          text=True).stdout


def section_bullets(text: str) -> list:
    """`- ` lines under any `## ` heading (the header's own rules are skipped)."""
    out, inside = [], False
    for line in text.splitlines():
        if line.startswith("## "):
            inside = True
        elif inside and line.startswith("- "):
            out.append(line.rstrip())
    return out


def changelog_at(ref: str) -> str:
    try:
        return git("show", f"{ref}:{CHANGELOG}")
    except subprocess.CalledProcessError:
        return ""


def added_bullets(base: str) -> list:
    """Bullets in HEAD's CHANGELOG.md that `base` did not have, in file order."""
    old = set(section_bullets(changelog_at(base)))
    return [b for b in section_bullets(changelog_at("HEAD")) if b not in old]


def previous_release_tag() -> str:
    try:
        return git("describe", "--tags", "--abbrev=0", "--match", "v-*",
                   "HEAD~1").strip()
    except subprocess.CalledProcessError:
        return ""


def top_section_bullets() -> list:
    lines = changelog_at("HEAD").splitlines()
    out, inside = [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = True
            continue
        if inside and line.startswith("- "):
            out.append(line.rstrip())
    return out


def problems(bullet: str) -> list:
    text = bullet[2:].strip()
    found = []
    if len(text) > MAX_CHARS:
        found.append(f"longer than {MAX_CHARS} characters ({len(text)})")
    jargon = sorted({m.group(0) for m in JARGON.finditer(text)})
    if jargon:
        found.append("technical words: " + ", ".join(jargon))
    return found


def notes() -> str:
    tag = previous_release_tag()
    bullets = added_bullets(tag) if tag else top_section_bullets()
    bullets = bullets[:MAX_BULLETS]
    return "\n".join(bullets) if bullets else FALLBACK


def check(base_ref: str) -> int:
    merge_base = git("merge-base", base_ref, "HEAD").strip()
    bullets = added_bullets(merge_base)
    if not bullets:
        print("::error file=CHANGELOG.md::Add at least one plain-language bullet "
              "under '## Unreleased' describing what the operator will notice "
              "(or label the PR 'no-release-notes').")
        return 1
    bad = 0
    for bullet in bullets:
        for problem in problems(bullet):
            print(f"::error file=CHANGELOG.md::'{bullet}': {problem}")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        sys.exit(check(sys.argv[2]))
    print(notes())
