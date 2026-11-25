#!/usr/bin/env python3
"""Verify that C/header files embedded in the VivaLaVent patch match canonical sources."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "integrations/vivalalvent/rtc-s3-selftest-hook.diff"
EMBEDDED_TO_SOURCE = {
    "shellcode_hv/rtc_s3_sleep.c": "rtc_s3_sleep.c",
    "shellcode_hv/rtc_s3_sleep.h": "rtc_s3_sleep.h",
    "shellcode_hv/rtc_s3_hook.c": "shellcode_hv/rtc_s3_hook.c",
    "shellcode_hv/rtc_s3_hook.h": "shellcode_hv/rtc_s3_hook.h",
}


def extract_new_file(lines: list[str], path: str) -> bytes:
    matches = [
        i for i, line in enumerate(lines)
        if line == f"diff --git a/{path} b/{path}"
    ]
    if len(matches) != 1:
        raise ValueError(f"expected one diff section for {path}, found {len(matches)}")

    start = matches[0] + 1
    end = start
    while end < len(lines) and not lines[end].startswith("diff --git "):
        end += 1
    section = lines[start:end]

    hunks = [i for i, line in enumerate(section) if line.startswith("@@ ")]
    if len(hunks) != 1:
        raise ValueError(f"{path}: expected one addition hunk, found {len(hunks)}")

    header = section[hunks[0]]
    if not re.search(r"@@ -0,0 \+1,\d+ @@", header):
        raise ValueError(f"{path}: expected a new-file addition hunk, got {header!r}")

    added = []
    for line in section[hunks[0] + 1:]:
        if line.startswith("diff --git "):
            break
        if line.startswith("+") and not line.startswith("+++"):
            added.append(line[1:])
        elif line.startswith("\\ No newline at end of file"):
            continue
        else:
            raise ValueError(f"{path}: unexpected non-addition line in new-file hunk")

    declared = int(re.search(r"\+1,(\d+)", header).group(1))
    if len(added) != declared:
        raise ValueError(
            f"{path}: hunk declares {declared} lines but contains {len(added)}"
        )
    return ("\n".join(added) + "\n").encode("utf-8")


def main() -> int:
    lines = PATCH.read_text(encoding="utf-8").splitlines()
    failures = []

    for embedded_path, source_path in EMBEDDED_TO_SOURCE.items():
        try:
            embedded = extract_new_file(lines, embedded_path)
            canonical = (ROOT / source_path).read_bytes()
            if embedded != canonical:
                failures.append(f"{embedded_path} does not match {source_path}")
            else:
                print(f"OK: {embedded_path} matches {source_path}")
        except (OSError, ValueError) as exc:
            failures.append(str(exc))

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1

    print("All embedded integration files match their canonical sources.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
