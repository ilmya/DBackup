#!/usr/bin/env python3
"""Check that the acceptance matrix and its user steps stay traceable."""

import json
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "tests" / "acceptance-scenarios.json"
CASE_PATTERN = re.compile(r"\b(?:(?:S[1-5]-[A-Z0-9-]+)|ROBUST|REL)-\d{3}\b")
sys.path.insert(0, str(ROOT / "tests"))
import run_acceptance_case  # noqa: E402


def main() -> int:
    try:
        catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
        docs = (ROOT / "docs" / "acceptance-test-cases.md").read_text(encoding="utf-8")
        generators = "\n".join((ROOT / path).read_text(encoding="utf-8") for path in (
            "tools/acceptance/dbackup_acceptance.py",
            "tests/prepare_acceptance_data.py",
        ))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"Catalog validation error: {exc}", file=sys.stderr)
        return 2

    automated = [case.get("id") for case in catalog.get("automatedCases", [])]
    manual = catalog.get("manualCases", [])
    registered = automated + manual
    duplicates = sorted({case for case in registered if registered.count(case) > 1})
    documented = set(CASE_PATTERN.findall(docs))
    registered_set = set(registered)
    missing = sorted(documented - registered_set)
    undocumented = sorted(registered_set - documented)
    missing_fixtures = []
    unmapped_automated = []
    for case in catalog.get("automatedCases", []):
        if not case.get("expected") or not run_acceptance_case.ctest_pattern(case.get("id", "")):
            unmapped_automated.append(case.get("id", "<missing id>"))
        for relative in case.get("fixture", []):
            if Path(relative).name not in generators:
                missing_fixtures.append(f"{case.get('id')}: {relative}")
    errors = []
    if duplicates:
        errors.append("duplicate catalog IDs: " + ", ".join(duplicates))
    if missing:
        errors.append("documented IDs absent from catalog: " + ", ".join(missing))
    if undocumented:
        errors.append("catalog IDs absent from user case sheet: " + ", ".join(undocumented))
    if missing_fixtures:
        errors.append("missing fixture declarations: " + ", ".join(missing_fixtures))
    if unmapped_automated:
        errors.append("automated cases missing expected results or CTest mappings: " + ", ".join(unmapped_automated))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Acceptance catalog valid: {len(registered_set)} cases, {len(automated)} automated scenario definitions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
