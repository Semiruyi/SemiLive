import unittest
from analyze import analyze, distribution


def sample(timestamp=10):
    pub = dict(run_id="test", machine="host", clock="qpc", frequency=1000,
               ssrc=1, rtp_timestamp=timestamp, frame_id=1, capture=100,
               encode_begin=102, encode_complete=110, first_send=112,
               last_send_complete=114, au_bytes=100, packet_count=2, key_frame=1)
    rec = dict(pub, first_receive=113, last_receive=115, au_ready=117)
    return pub, rec


class AnalysisTest(unittest.TestCase):
    def test_stages_sum_and_percentiles(self):
        p, r = sample()
        frames, summary = analyze([p], [r], 0)
        f = frames[0]
        self.assertEqual(f["capture_to_ready_ms"], 17)
        self.assertEqual(sum(f[k] for k in ("capture_wait_ms", "encode_ms",
                         "output_wait_ms", "first_packet_ms", "receive_to_ready_ms")), 17)
        self.assertEqual(distribution(range(1, 101))["p95"], 95)
        self.assertEqual(summary["matched_frames"], 1)

    def test_cross_host_rejected(self):
        p, r = sample()
        r["machine"] = "other"
        with self.assertRaises(ValueError):
            analyze([p], [r], 0)

    def test_invalid_clock_and_interval(self):
        p, r = sample()
        for warmup, duration in ((float('nan'), None), (-1, None), (0, 0)):
            with self.assertRaises(ValueError):
                analyze([p], [r], warmup, duration)
        p["clock"] = r["clock"] = "steady"
        with self.assertRaises(ValueError):
            analyze([p], [r], 0)

    def test_duplicate_key_rejected(self):
        p, r = sample()
        with self.assertRaises(ValueError):
            analyze([p, p], [r], 0)

    def test_negative_stage_and_packet_mismatch(self):
        p, r = sample()
        r["first_receive"] = 111
        self.assertEqual(len(analyze([p], [r], 0)[1]["invalid_frames"]), 1)
        r["first_receive"] = 113
        r["packet_count"] = 3
        self.assertEqual(len(analyze([p], [r], 0)[1]["invalid_frames"]), 1)

    def test_warmup_unmatched_and_wrapped_timestamp(self):
        p, r = sample(0xffffffff)
        p2, r2 = sample(0)
        for key in ("capture", "encode_begin", "encode_complete", "first_send", "last_send_complete"):
            p2[key] += 2000
        for key in ("first_receive", "last_receive", "au_ready"):
            r2[key] += 2000
        _, s = analyze([p, p2], [r, r2], 1)
        self.assertEqual(s["selected_frames"], 1)
        self.assertEqual(s["matched_frames"], 1)
        self.assertEqual(analyze([p, p2], [r], 0)[1]["unmatched_publisher"], 1)

    def test_loss_denominator_and_retransmissions(self):
        p, r = sample()
        p2, _ = sample(11)
        r["packet_count"] = 3
        _, s = analyze([p, p2], [r], 0, allow_loss=True)
        self.assertEqual(s["delivery_percent"], 50)
        self.assertEqual(s["on_time_delivery_percent"]["100"], 50)
        self.assertEqual(s["invalid_frames"], [])

    def test_complete_loss_has_no_latency_distribution(self):
        p, _ = sample()
        _, s = analyze([p], [], 0, allow_loss=True)
        self.assertEqual(s["delivery_percent"], 0)
        self.assertEqual(s["on_time_delivery_percent"]["200"], 0)
        self.assertIsNone(s["metrics_ms"]["capture_to_ready_ms"]["p95"])


if __name__ == "__main__":
    unittest.main()
