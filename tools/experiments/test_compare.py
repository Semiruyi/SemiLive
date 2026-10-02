"""Check that PLI and reorder comparisons keep other controls fixed."""
import json
from pathlib import Path
import tempfile
import unittest

from compare import load


class ComparisonTests(unittest.TestCase):
    def make_run(self, root, capacity=1024, enabled=True, retry=500, legacy=False):
        root.mkdir(parents=True, exist_ok=True)
        (root / "groups.json").write_text('[{"loss_percent": 3}]', encoding="utf-8")
        directory = root / "loss-3" / "relay-1"
        directory.mkdir(parents=True)
        receiver = {
            "config": {"rtp": {"reorder_maximum_buffered_packets": capacity,
                               "reorder_maximum_hold_ms": 50}},
            "rtp": {"reorder": {"capacity_gaps": 0, "timeout_gaps": 1}}}
        if not legacy:
            receiver["config"]["pli"] = {
                "enabled": enabled, "initial_wait_ms": 100,
                "retry_interval_ms": retry, "media_inactivity_timeout_ms": 1000}
        for name, data in (
            ("receiver", receiver),
            ("publisher", {"config": {"video": {"gop_size": 60}}}),
            ("manifest", {"loss_percent": 3, "seed": 1001, "warmup_seconds": 2,
                          "duration_seconds": 10, "rtcp_enabled": True})):
            (directory / f"{name}.json").write_text(json.dumps(data), encoding="utf-8")
        (directory / "publisher.csv").write_text("packet_count\n100\n", encoding="utf-8")
        return root

    def test_pli_comparison_only_relaxes_enable_flag(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            off = self.make_run(root / "off", enabled=False)
            on = self.make_run(root / "on")
            changed = self.make_run(root / "changed", capacity=512)
            retry = self.make_run(root / "retry", retry=100)
            conditions = lambda path: load(path, "pli")[1][0]["conditions"]
            self.assertEqual(conditions(off), conditions(on))
            self.assertNotEqual(conditions(off), conditions(changed))
            self.assertNotEqual(conditions(off), conditions(retry))

    def test_reorder_comparison_does_not_relax_pli(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            a = self.make_run(root / "a", capacity=512)
            b = self.make_run(root / "b")
            off = self.make_run(root / "off", enabled=False)
            self.assertEqual(load(a)[1][0]["conditions"], load(b)[1][0]["conditions"])
            self.assertNotEqual(load(a)[1][0]["conditions"], load(off)[1][0]["conditions"])

    def test_historical_reports_have_no_automatic_pli(self):
        with tempfile.TemporaryDirectory() as temp:
            path = self.make_run(Path(temp), legacy=True)
            self.assertFalse(load(path)[1][0]["pli_enabled"])


if __name__ == "__main__":
    unittest.main()
