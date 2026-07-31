#!/usr/bin/env python3
"""Generate a shields.io-compatible badge JSON from a JUnit test report.

Usage: gen_test_badge.py <junit-xml> <badge-json-out>
"""
import json
import sys
import xml.etree.ElementTree as ET


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <junit-xml> <badge-json-out>", file=sys.stderr)
        return 1

    junit_path, badge_path = sys.argv[1], sys.argv[2]

    root = ET.parse(junit_path).getroot()
    total = int(root.get("tests", 0))
    failures = int(root.get("failures", 0))
    skipped = int(root.get("skipped", 0))
    passed = total - failures - skipped

    parts = [f"{passed} passed"]
    if failures:
        parts.append(f"{failures} failed")
    if skipped:
        parts.append(f"{skipped} skipped")
    message = ", ".join(parts)

    if failures > 0:
        color = "red"
    elif skipped > 0:
        color = "yellow"
    else:
        color = "brightgreen"

    badge = {
        "schemaVersion": 1,
        "label": "tests",
        "message": message,
        "color": color,
    }

    with open(badge_path, "w") as f:
        json.dump(badge, f, indent=2)
        f.write("\n")

    print(f"message={message}")
    print(f"color={color}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
