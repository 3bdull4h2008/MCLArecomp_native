#!/usr/bin/env python3
"""Unit tests for tools/p0_census.py and its checked-in log fixtures."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("p0_census", ROOT / "tools/p0_census.py")
p0_census = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = p0_census
SPEC.loader.exec_module(p0_census)


class P0CensusTest(unittest.TestCase):
    def parse_text(self, text: str, legacy: bool = False):
        with tempfile.TemporaryDirectory(dir=ROOT / "build") as temporary:
            path = Path(temporary) / "fixture.log"
            path.write_text(text, encoding="utf-8")
            return p0_census.census(path, legacy)

    def test_positive_fixture(self):
        result = p0_census.census(ROOT / "tools/fixtures/p0_audit_positive.log")
        self.assertEqual(result.exit_code(), 1)
        self.assertEqual(result.attention, 1)
        self.assertEqual(result.table["SAMPLER"]["attention"], 1)
        self.assertEqual(result.fixtures, 8)
        self.assertEqual(result.fixture_done, 1)
        self.assertEqual(result.errors, [])

    def test_negative_fixture(self):
        result = p0_census.census(ROOT / "tools/fixtures/p0_audit_negative.log")
        self.assertEqual(result.exit_code(), 0)
        self.assertEqual(result.attention, 0)
        self.assertEqual(result.rows, 4)
        self.assertEqual(result.fixtures, 8)
        self.assertEqual(result.errors, [])

    def test_domain_verdict_labels(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x6 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-ALLOCATOR v=1 seq=1 domainSeq=1 events=1 attention=1 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=attention verdict=unsafe "
            "reason=allocator-reset-before-fence\n"
            "[00:00:00.200] [info] P0-AUDIT-RESOLVED v=1 seq=2 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=ok verdict=not-stale "
            "reason=disabled-age-not-stale\n"
            "[00:00:00.300] [info] P0-AUDIT-SUMMARY v=1 seq=3 domain=ALLOCATOR domainSeq=1 events=1 "
            "attention=1 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n"
            "[00:00:00.400] [info] P0-AUDIT-SUMMARY v=1 seq=4 domain=RESOLVED domainSeq=1 events=1 "
            "attention=0 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n")
        self.assertEqual(result.exit_code(), 1)
        self.assertEqual(result.table["ALLOCATOR"]["attention"], 1)
        self.assertEqual(result.table["RESOLVED"]["ok"], 1)
        self.assertEqual(result.errors, [])

    def test_verdict_category_mismatch(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0xF source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-ALLOCATOR v=1 seq=1 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=ok verdict=unsafe reason=x\n")
        self.assertEqual(result.exit_code(), 2)
        self.assertTrue(any("verdict unsafe for category ok" in error for error in result.errors))

    def test_interleaved_domains_and_cadence_gaps(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0xF source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-SAMPLER v=1 seq=1 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=ok verdict=bound-ok reason=x\n"
            "[00:00:00.200] [info] P0-AUDIT-ALLOCATOR v=1 seq=2 domainSeq=1 events=1 attention=1 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=attention verdict=unsafe reason=x\n"
            "[00:00:00.300] [info] P0-AUDIT-RESOLVED v=1 seq=3 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=ok verdict=exact reason=x\n"
            "[00:00:00.400] [info] P0-AUDIT-SAMPLER v=1 seq=4 domainSeq=2 events=2 attention=1 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=attention verdict=neutral-fallback reason=x\n"
            "[00:00:00.500] [info] P0-AUDIT-PSO-RTV v=1 seq=5 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=1 recovered=0 fixture=0 category=not-applicable verdict=depth-only reason=x\n"
            "[00:00:00.600] [info] P0-AUDIT-ALLOCATOR v=1 seq=6 domainSeq=3 events=3 attention=1 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=ok verdict=safe reason=x\n"
            "[00:00:00.700] [info] P0-AUDIT-RESOLVED v=1 seq=7 domainSeq=3 events=3 attention=1 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=attention verdict=stale reason=x\n"
            "[00:00:00.800] [info] P0-AUDIT-SUMMARY v=1 seq=8 domain=SAMPLER domainSeq=2 events=2 "
            "attention=1 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n"
            "[00:00:00.900] [info] P0-AUDIT-SUMMARY v=1 seq=9 domain=ALLOCATOR domainSeq=3 events=3 "
            "attention=1 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n"
            "[00:01:00.000] [info] P0-AUDIT-SUMMARY v=1 seq=10 domain=RESOLVED domainSeq=3 events=3 "
            "attention=1 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n"
            "[00:01:00.100] [info] P0-AUDIT-SUMMARY v=1 seq=11 domain=PSO-RTV domainSeq=1 events=1 "
            "attention=0 unknown=0 notApplicable=1 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n")
        self.assertEqual(result.exit_code(), 1)
        self.assertEqual(result.table["SAMPLER"]["attention"], 1)
        self.assertEqual(result.table["ALLOCATOR"]["attention"], 1)
        self.assertEqual(result.table["ALLOCATOR"]["ok"], 2)
        self.assertEqual(result.table["RESOLVED"]["attention"], 1)
        self.assertEqual(result.table["PSO-RTV"]["not-applicable"], 1)
        self.assertEqual(result.errors, [])

    def test_throttled_rows_with_larger_summary(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x2 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-SUMMARY v=1 seq=1 domain=ALLOCATOR domainSeq=10 events=10 "
            "attention=0 unknown=10 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=1\n"
            "[00:00:00.200] [info] P0-AUDIT-ALLOCATOR v=1 seq=2 domainSeq=200 events=200 attention=0 unknown=200 "
            "notApplicable=0 recovered=0 fixture=0 category=unknown verdict=fence-unknown reason=x\n")
        self.assertEqual(result.errors, [])
        self.assertEqual(result.coverage["ALLOCATOR"], "COMPLETE")
        self.assertEqual(result.table["ALLOCATOR"]["unknown"], 10)
        self.assertEqual(result.table["ALLOCATOR"]["ok"], 0)
        self.assertEqual(result.exit_code(), 3)

    def test_multiple_summaries_and_regression(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x2 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-SUMMARY v=1 seq=1 domain=ALLOCATOR domainSeq=5 events=5 "
            "attention=0 unknown=5 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=1\n"
            "[00:00:00.200] [info] P0-AUDIT-SUMMARY v=1 seq=2 domain=ALLOCATOR domainSeq=8 events=8 "
            "attention=2 unknown=5 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=2\n"
            "[00:00:00.300] [info] P0-AUDIT-SUMMARY v=1 seq=3 domain=ALLOCATOR domainSeq=7 events=7 "
            "attention=2 unknown=5 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=3\n")
        self.assertEqual(result.exit_code(), 2)
        self.assertTrue(any("summary counts regressed" in error for error in result.errors))
        self.assertEqual(result.table["ALLOCATOR"]["unknown"], 5)
        self.assertEqual(result.table["ALLOCATOR"]["attention"], 2)
        self.assertEqual(result.table["ALLOCATOR"]["ok"], 1)

    def test_rows_without_summary_are_partial(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x2 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-ALLOCATOR v=1 seq=1 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=ok verdict=safe reason=x\n")
        self.assertEqual(result.coverage["ALLOCATOR"], "PARTIAL")
        self.assertEqual(result.table["ALLOCATOR"]["ok"], 1)
        self.assertEqual(result.exit_code(), 3)

    def test_summary_only_is_not_blind(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x2 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-SUMMARY v=1 seq=1 domain=ALLOCATOR domainSeq=3 events=3 "
            "attention=1 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=1\n")
        self.assertEqual(result.coverage["ALLOCATOR"], "SUMMARY-ONLY")
        self.assertNotEqual(result.coverage["ALLOCATOR"], "BLIND")
        self.assertEqual(result.table["ALLOCATOR"]["attention"], 1)
        self.assertEqual(result.table["ALLOCATOR"]["ok"], 2)
        self.assertEqual(result.exit_code(), 1)

    def test_rtv_unknown_true_mismatch_and_depth_only(self):
        unknown = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x8 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-PSO-RTV v=1 seq=1 domainSeq=1 events=1 attention=0 unknown=1 "
            "notApplicable=0 recovered=0 fixture=0 category=unknown verdict=rtv-format-unknown "
            "reason=effective-rtv-slot-format-unknown rtvFmt=0\n"
            "[00:00:00.200] [info] P0-AUDIT-SUMMARY v=1 seq=2 domain=PSO-RTV domainSeq=1 events=1 "
            "attention=0 unknown=1 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=1\n")
        self.assertEqual(unknown.exit_code(), 3)
        self.assertEqual(unknown.table["PSO-RTV"]["unknown"], 1)
        self.assertEqual(unknown.coverage["PSO-RTV"], "COMPLETE")
        mismatch = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x8 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-PSO-RTV v=1 seq=1 domainSeq=1 events=1 attention=1 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 category=attention verdict=pso-rtv-mismatch "
            "reason=pso-rtv-mismatch psoFmt=10 rtvFmt=28\n"
            "[00:00:00.200] [info] P0-AUDIT-SUMMARY v=1 seq=2 domain=PSO-RTV domainSeq=1 events=1 "
            "attention=1 unknown=0 notApplicable=0 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=1\n")
        self.assertEqual(mismatch.exit_code(), 1)
        depth = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x8 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-PSO-RTV v=1 seq=1 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=1 recovered=0 fixture=0 category=not-applicable verdict=depth-only "
            "reason=depth-only-zero-rtv\n"
            "[00:00:00.200] [info] P0-AUDIT-SUMMARY v=1 seq=2 domain=PSO-RTV domainSeq=1 events=1 "
            "attention=0 unknown=0 notApplicable=1 recovered=0 fixture=0 category=summary verdict=summary reason=frame-begun frame=1\n")
        self.assertEqual(depth.exit_code(), 0)

    def test_unknown_marker_schema(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0xF source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-UNKNOWN v=1 seq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 verdict=ok reason=x\n")
        self.assertEqual(result.exit_code(), 2)
        self.assertTrue(any("unknown P0 audit marker" in error for error in result.errors))

    def test_duplicate_sequence_and_bad_version(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x1 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-SAMPLER v=2 seq=1 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 verdict=ok reason=x\n"
            "[00:00:00.200] [info] P0-AUDIT-SAMPLER v=1 seq=1 domainSeq=2 events=2 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 verdict=ok reason=x\n")
        self.assertEqual(result.exit_code(), 2)
        self.assertTrue(any("v must be 1" in error for error in result.errors))
        self.assertTrue(any("duplicate seq" in error for error in result.errors))

    def test_mask_consistency_and_summary_internal_mismatch(self):
        result = self.parse_text(
            "[00:00:00.000] [info] P0-AUDIT-CONFIG status=armed mask=0x1 source=explicit\n"
            "[00:00:00.100] [info] P0-AUDIT-ALLOCATOR v=1 seq=1 domainSeq=1 events=1 attention=0 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 verdict=ok reason=x\n"
            "[00:00:00.200] [info] P0-AUDIT-SUMMARY v=1 seq=2 domain=ALLOCATOR domainSeq=1 events=1 attention=2 unknown=0 "
            "notApplicable=0 recovered=0 fixture=0 verdict=summary reason=frame-begun frame=2\n")
        self.assertEqual(result.exit_code(), 2)
        self.assertTrue(any("mask bit is disabled" in error for error in result.errors))
        self.assertTrue(any("summary internal sum mismatch" in error for error in result.errors))

    def test_invalid_config_is_inconclusive(self):
        result = self.parse_text("[00:00:00.000] [warning] P0-AUDIT-CONFIG status=invalid mask=0x0 source=explicit\n")
        self.assertEqual(result.exit_code(), 3)

    def test_substring_collision_is_not_a_row(self):
        result = self.parse_text(
            "[00:00:00.000] [info] note: P0-AUDIT-SAMPLER verdict=attention reason=prose\n")
        self.assertEqual(result.rows, 0)
        self.assertEqual(result.attention, 0)

    def test_legacy_dirty_rows(self):
        result = self.parse_text(
            "[00:00:00.000] [warning] SAMPLER-HEAP-OWNER-MISMATCH #1 owner=1 active=2\n", legacy=True)
        self.assertEqual(result.exit_code(), 1)
        self.assertEqual(result.legacy["SAMPLER-HEAP-OWNER-MISMATCH"], 1)


if __name__ == "__main__":
    unittest.main()
