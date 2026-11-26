#!/usr/bin/env python3
"""Streaming census for P0-02 observation-only audit logs.

Exit codes: 0 clean, 1 attention, 2 malformed, 3 inconclusive.
Fixture rows are validated but excluded from the runtime verdict table.
"""

from __future__ import annotations

import argparse
import json
import re
import shlex
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

OWN_LABEL = re.compile(r"^\[[^\]]+\]\s+\[[^\]]+\]\s+(P0-AUDIT-[A-Z0-9-]+)(?:\s+(.*))?$")
DOMAINS = {"SAMPLER": 0x1, "ALLOCATOR": 0x2, "RESOLVED": 0x4, "PSO-RTV": 0x8}
VERDICTS = ("ok", "attention", "unknown", "not-applicable", "recovered")
DOMAIN_LABELS = {
    "SAMPLER": {
        "ok": {"ok", "bound-ok"},
        "attention": {"attention", "neutral-fallback", "unbound", "owner-mismatch", "out-of-range", "misaligned"},
        "unknown": {"unknown", "intern-failed", "owner-unknown", "geometry-unknown"},
    },
    "ALLOCATOR": {
        "ok": {"ok", "safe"},
        "attention": {"attention", "unsafe", "reset-failed"},
        "unknown": {"unknown", "fence-unknown"},
        "recovered": {"recovered"},
    },
    "RESOLVED": {
        "ok": {"ok", "exact", "not-stale", "exact-depth"},
        "attention": {"attention", "fallback-dim", "stale", "rejected", "format-mismatch",
                      "format-fallback", "non-last-winner", "fallback-srv"},
        "unknown": {"unknown", "metadata-unknown", "frame-underflow", "staging-unknown"},
        "not-applicable": {"not-applicable", "depth-fallback"},
    },
    "PSO-RTV": {
        "ok": {"ok", "pso-rtv-match"},
        "attention": {"attention", "pso-rtv-mismatch"},
        "unknown": {"unknown", "metadata-unknown", "rtv-format-unknown"},
        "not-applicable": {"not-applicable", "depth-only"},
    },
}
LEGACY = ("SAMPLER-HEAP-OWNER-MISMATCH", "ALLOC-LIFETIME-FENCE",
          "RESOLVED-BIND-DIM-MISMATCH", "TEMP-PSO-RTV")


@dataclass
class Result:
    errors: list[str] = field(default_factory=list)
    config: dict = field(default_factory=dict)
    table: dict[str, dict[str, int]] = field(default_factory=lambda: {
        domain: {verdict: 0 for verdict in VERDICTS} for domain in DOMAINS
    })
    fixtures: int = 0
    fixture_done: int = 0
    summaries: int = 0
    legacy: dict[str, int] = field(default_factory=lambda: {marker: 0 for marker in LEGACY})
    coverage: dict[str, str] = field(default_factory=lambda: {domain: "BLIND" for domain in DOMAINS})
    rows: int = 0

    @property
    def attention(self) -> int:
        return sum(values["attention"] for values in self.table.values())

    @property
    def unknown(self) -> int:
        return sum(values["unknown"] for values in self.table.values())

    def inconclusive(self) -> bool:
        if self.config.get("status") != "armed":
            return True
        mask = self.config.get("mask", 0)
        return any((mask & DOMAINS[domain]) and self.coverage.get(domain) in ("BLIND", "PARTIAL")
                   for domain in DOMAINS)

    def exit_code(self) -> int:
        if self.errors:
            return 2
        if not self.config:
            return 1 if sum(self.legacy.values()) else 3
        if self.config.get("status") != "armed":
            return 3
        if self.unknown or self.inconclusive():
            return 3
        return 1 if self.attention else 0


def fields(message: str) -> dict[str, str]:
    try:
        tokens = shlex.split(message or "", posix=True)
    except ValueError:
        return {}
    return dict(token.split("=", 1) for token in tokens if "=" in token)


def integer(values: dict[str, str], name: str, errors: list[str], line: int) -> int:
    raw = values.get(name)
    if raw is None or not re.fullmatch(r"\d+", raw):
        errors.append(f"line {line}: missing/non-negative integer {name}")
        return 0
    return int(raw)


def census(path: Path, legacy: bool = False) -> Result:
    result = Result()
    seen_seq: set[int] = set()
    last_all_seq = 0
    summary_counts = {}
    row_counts = {}
    rows_seen = set()
    last_domain_seq = {domain: 0 for domain in DOMAINS}
    mask = 0
    with path.open("r", encoding="utf-8", errors="replace") as source:
        for line_number, line in enumerate(source, 1):
            match = OWN_LABEL.match(line.rstrip("\r\n"))
            if not match:
                if legacy:
                    legacy_match = re.match(r"^\[[^\]]+\]\s+\[[^\]]+\]\s+(\S+)", line)
                    message = legacy_match.group(1) if legacy_match else ""
                    for marker in LEGACY:
                        if message == marker:
                            result.legacy[marker] += 1
                continue
            marker, message = match.groups()
            values = fields(message)
            if marker == "P0-AUDIT-CONFIG":
                if result.config:
                    result.errors.append(f"line {line_number}: duplicate P0-AUDIT-CONFIG")
                status = values.get("status")
                try:
                    parsed_mask = int(values.get("mask", ""), 16)
                except ValueError:
                    parsed_mask = -1
                result.config = {"status": status, "mask": parsed_mask, "source": values.get("source", "")}
                if status not in ("armed", "invalid"):
                    result.errors.append(f"line {line_number}: unknown config status")
                if parsed_mask < 0 or parsed_mask & ~0x8000000F:
                    result.errors.append(f"line {line_number}: invalid config mask")
                if status == "armed":
                    mask = parsed_mask
                continue
            if marker == "P0-AUDIT-SELFTEST-DONE":
                if values.get("pass") != "8" or values.get("total") != "8" or values.get("fixture") != "1":
                    result.errors.append(f"line {line_number}: bad fixture done marker")
                result.fixture_done += 1
                continue
            if marker == "P0-AUDIT-SUMMARY":
                result.summaries += 1
                summary_seq = integer(values, "seq", result.errors, line_number)
                if summary_seq in seen_seq:
                    result.errors.append(f"line {line_number}: duplicate seq {summary_seq}")
                seen_seq.add(summary_seq)
                if summary_seq <= last_all_seq:
                    result.errors.append(f"line {line_number}: seq is not monotonic")
                last_all_seq = max(last_all_seq, summary_seq)
                domain = values.get("domain")
                if domain not in DOMAINS:
                    result.errors.append(f"line {line_number}: summary domain missing/unknown")
                    continue
                if values.get("v") != "1" or values.get("verdict") != "summary" or \
                        values.get("category", "summary") != "summary":
                    result.errors.append(f"line {line_number}: bad summary schema")
                events = integer(values, "events", result.errors, line_number)
                categories = tuple(integer(values, name, result.errors, line_number)
                                   for name in ("attention", "unknown", "notApplicable", "recovered"))
                ok_count = events - sum(categories)
                counts = (ok_count,) + categories
                domain_seq = integer(values, "domainSeq", result.errors, line_number)
                valid = True
                if ok_count < 0:
                    result.errors.append(f"line {line_number}: {domain} summary internal sum mismatch")
                    valid = False
                if domain_seq != events:
                    result.errors.append(f"line {line_number}: summary domainSeq/events mismatch")
                    valid = False
                previous = summary_counts.get(domain)
                if previous is not None and any(current < prior for current, prior in zip(counts, previous)):
                    result.errors.append(f"line {line_number}: {domain} summary counts regressed")
                    valid = False
                if valid:
                    summary_counts[domain] = counts
                continue
            domain = marker.removeprefix("P0-AUDIT-")
            if domain not in DOMAINS:
                result.errors.append(f"line {line_number}: unknown P0 audit marker {domain}")
                continue
            if not result.config:
                result.errors.append(f"line {line_number}: audit row before ARMED config")
            if values.get("v") != "1":
                result.errors.append(f"line {line_number}: row v must be 1")
            sequence = integer(values, "seq", result.errors, line_number)
            if sequence in seen_seq:
                result.errors.append(f"line {line_number}: duplicate seq {sequence}")
            seen_seq.add(sequence)
            if sequence <= last_all_seq:
                result.errors.append(f"line {line_number}: seq is not monotonic")
            last_all_seq = max(last_all_seq, sequence)
            fixture = integer(values, "fixture", result.errors, line_number)
            if fixture not in (0, 1):
                result.errors.append(f"line {line_number}: fixture must be 0/1")
            events = integer(values, "events", result.errors, line_number)
            categories = tuple(integer(values, name, result.errors, line_number)
                               for name in ("attention", "unknown", "notApplicable", "recovered"))
            verdict = values.get("verdict")
            category = values.get("category")
            if category is None and verdict in VERDICTS:
                category = verdict  # backward-compatible v1 rows written before domain labels
            if category not in VERDICTS:
                result.errors.append(f"line {line_number}: unknown category {category}")
            elif verdict not in DOMAIN_LABELS[domain].get(category, set()):
                result.errors.append(f"line {line_number}: unknown {domain} verdict {verdict} for category {category}")
            if "reason" not in values:
                result.errors.append(f"line {line_number}: reason missing")
            category_sum = sum(categories)
            if category_sum > events:
                result.errors.append(f"line {line_number}: category sum exceeds events")
            if fixture:
                result.fixtures += 1
                continue
            result.rows += 1
            domain_seq = integer(values, "domainSeq", result.errors, line_number)
            ok_count = events - sum(categories)
            counts = (ok_count,) + categories
            valid = True
            if ok_count < 0:
                result.errors.append(f"line {line_number}: {domain} row internal sum mismatch")
                valid = False
            if domain_seq != events:
                result.errors.append(f"line {line_number}: {domain} domainSeq/events mismatch")
                valid = False
            if domain_seq <= last_domain_seq[domain]:
                result.errors.append(f"line {line_number}: {domain} domainSeq is not monotonic")
                valid = False
            previous = row_counts.get(domain, (0, 0, 0, 0, 0))
            if any(current < prior for current, prior in zip(counts, previous)):
                result.errors.append(f"line {line_number}: {domain} row counters regressed")
                valid = False
            event_delta = events - previous[0]
            category_delta = tuple(current - prior for current, prior in zip(categories, previous[1:]))
            if any(value < 0 or value > event_delta for value in category_delta) or sum(category_delta) > event_delta:
                result.errors.append(f"line {line_number}: {domain} category sums exceed the event advance")
                valid = False
            if valid:
                row_counts[domain] = counts
                rows_seen.add(domain)
                last_domain_seq[domain] = domain_seq
            if result.config.get("status") != "armed":
                result.errors.append(f"line {line_number}: runtime row while audit is not armed")
            elif not (mask & DOMAINS[domain]):
                result.errors.append(f"line {line_number}: {domain} row while mask bit is disabled")
    for domain in DOMAINS:
        if result.config.get("status") != "armed" or not (mask & DOMAINS[domain]):
            result.coverage[domain] = "DISABLED"
            continue
        if domain in summary_counts:
            counts = summary_counts[domain]
            if sum(counts) > 0:
                result.coverage[domain] = "COMPLETE" if domain in rows_seen else "SUMMARY-ONLY"
            else:
                result.coverage[domain] = "PARTIAL" if domain in rows_seen else "BLIND"
        elif domain in row_counts:
            counts = row_counts[domain]
            result.coverage[domain] = "PARTIAL"
        else:
            counts = (0, 0, 0, 0, 0)
            result.coverage[domain] = "BLIND"
        result.table[domain] = dict(zip(VERDICTS, counts))
    if result.config.get("status") == "armed" and result.config.get("mask") == 0 and result.rows:
        result.errors.append("mask=0 emitted runtime rows")
    return result


def print_result(result: Result, baseline: Result | None, as_json: bool) -> None:
    payload = {
        "exit_code": result.exit_code(), "config": result.config,
        "table": result.table, "attention": result.attention, "unknown": result.unknown,
        "fixtures": result.fixtures, "fixture_done": result.fixture_done,
        "summaries": result.summaries, "coverage": result.coverage,
        "legacy": result.legacy, "errors": result.errors,
    }
    if baseline is not None:
        payload["baseline"] = baseline.table
        payload["delta"] = {domain: {verdict: result.table[domain][verdict] - baseline.table[domain][verdict]
                                     for verdict in VERDICTS} for domain in DOMAINS}
    if as_json:
        print(json.dumps(payload, indent=2, sort_keys=True))
        return
    print(f"P0-CENSUS exit={result.exit_code()} config={result.config.get('status', 'missing')} "
          f"mask={result.config.get('mask', -1):#x} attention={result.attention} unknown={result.unknown} "
          f"fixtures={result.fixtures} errors={len(result.errors)}")
    print("domain\tcoverage\tok\tattention\tunknown\tnot-applicable\trecovered" +
          ("\tdelta-attention" if baseline is not None else ""))
    for domain in DOMAINS:
        row = result.table[domain]
        suffix = ""
        if baseline is not None:
            delta = row["attention"] - baseline.table[domain]["attention"]
            suffix = f"\t{delta:+d}"
        print(f"{domain}\t{result.coverage[domain]}\t{row['ok']}\t{row['attention']}\t{row['unknown']}\t"
              f"{row['not-applicable']}\t{row['recovered']}{suffix}")
    for error in result.errors:
        print(f"ERROR {error}")


def run_self_test() -> int:
    root = Path(__file__).resolve().parents[1]
    positive = census(root / "tools/fixtures/p0_audit_positive.log")
    negative = census(root / "tools/fixtures/p0_audit_negative.log")
    if positive.exit_code() != 1 or positive.attention != 1 or positive.errors:
        return 1
    if negative.exit_code() != 0 or negative.attention != 0 or negative.errors:
        return 1
    with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / "build") as temporary:
        malformed = Path(temporary) / "malformed.log"
        malformed.write_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0xF source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-UNKNOWN v=1 seq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 verdict=ok reason=x\n", encoding="utf-8")
        if census(malformed).exit_code() != 2:
            return 1
    print("P0-CENSUS-SELFTEST OK positive=1 negative=0 malformed=2")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, nargs="?")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--legacy", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return run_self_test()
    if args.log is None:
        parser.error("log is required unless --self-test is used")
    try:
        result = census(args.log, args.legacy)
        baseline = census(args.baseline, args.legacy) if args.baseline else None
    except OSError as exc:
        print(f"ERROR {exc}", file=sys.stderr)
        return 2
    print_result(result, baseline, args.json)
    return result.exit_code()


if __name__ == "__main__":
    raise SystemExit(main())
