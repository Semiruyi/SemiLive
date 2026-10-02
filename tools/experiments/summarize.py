"""Summarize delivery and latency without pooling unrelated percentiles."""
import argparse
import csv
import json
import statistics
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    rows = []
    for path in sorted(args.root.glob("loss-*/relay-*/summary.json")):
        s = json.loads(path.read_text())
        directory = path.parent
        m = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))
        rec = json.loads((directory / "receiver.json").read_text())
        pub = json.loads((directory / "publisher.json").read_text())
        relay = json.loads((directory / "relay.json").read_text())
        with (directory / "publisher.csv").open(newline="") as stream:
            sent_frames = list(csv.DictReader(stream))
        max_idr_packets = max((int(f["packet_count"]) for f in sent_frames
                               if f["key_frame"] == "1"), default=0)
        rtcp = pub["rtcp"] or {}
        total = s["metrics_ms"]["capture_to_ready_ms"]
        traffic_ok = (relay["traffic"]["received_datagrams"] == pub["rtp"]["media_datagrams"]
                      + rtcp.get("retransmitted_packets", 0))
        received_ok = relay["traffic"]["forwarded_datagrams"] == rec["input"]["received_datagrams"]
        rows.append({"loss_percent": m["loss_percent"], "seed": m["seed"], "valid": s["valid"],
                     "pli_enabled": m.get("pli_enabled", False),
                     "pli_packets_sent_total": (rec["rtcp"] or {}).get("pli_packets_sent", 0),
                     "pli_packets_received_total": rtcp.get("pli_packets_received", 0),
                     "key_frame_requests_applied_total": pub["encoder"].get("key_frame_requests_applied", 0),
                     "reorder_packets": rec["config"]["rtp"]["reorder_maximum_buffered_packets"],
                     "reorder_hold_ms": rec["config"]["rtp"]["reorder_maximum_hold_ms"],
                     "sent_aus": s["selected_frames"], "delivered_aus": s["matched_frames"],
                     "delivery_percent": s["delivery_percent"],
                     "on_time_100ms_percent": s["on_time_delivery_percent"]["100"],
                     "on_time_200ms_percent": s["on_time_delivery_percent"]["200"],
                     "p50_ms": total["p50"], "p95_ms": total["p95"], "p99_ms": total["p99"], "max_ms": total["max"],
                     "receive_to_ready_p95_ms": s["metrics_ms"]["receive_to_ready_ms"]["p95"],
                     "delivery_gap_max_ms": s["au_delivery_gap_ms"]["max"],
                     "timeout_gaps_total": rec["rtp"]["reorder"]["timeout_gaps"],
                     "capacity_gaps_total": rec["rtp"]["reorder"]["capacity_gaps"],
                     "retransmitted_packets_total": rtcp.get("retransmitted_packets", 0),
                     "cache_misses_total": rtcp.get("retransmission_cache_misses", 0),
                     "actual_loss_percent": relay["traffic"]["actual_loss_percent"],
                     "max_idr_packets_total": max_idr_packets,
                     "traffic_counts_match": traffic_ok and received_ok,
                     "recovery_wait_max_ms_total": rec["h264"]["recovery"]["wait_maximum_ms"],
                     "recovered_after_nack_total": (rec["rtcp"] or {}).get("missing_tracker", {}).get("recovered_after_nack", 0),
                     "directory": str(directory), "relay_session": relay["session"]["run_succeeded"]})
    if not rows:
        raise SystemExit("No completed experiments")
    groups = []
    for loss in sorted({r["loss_percent"] for r in rows}):
        selected = [r for r in rows if r["loss_percent"] == loss]
        if len({r["pli_enabled"] for r in selected}) != 1:
            raise SystemExit("Mixed PLI settings within a loss group")
        result = {"loss_percent": loss, "runs": len(selected),
                  "pli_enabled": selected[0]["pli_enabled"],
                  "runs_with_delivery": sum(r["delivered_aus"] > 0 for r in selected),
                  "delivery_percent_min": min(r["delivery_percent"] for r in selected),
                  "delivery_percent_max": max(r["delivery_percent"] for r in selected),
                  "valid": all(r["valid"] and r["relay_session"] for r in selected),
                  "traffic_counts_match": all(r["traffic_counts_match"] for r in selected)}
        for key in ("delivery_percent", "on_time_100ms_percent", "on_time_200ms_percent",
                    "p50_ms", "p95_ms", "p99_ms", "max_ms", "receive_to_ready_p95_ms", "delivery_gap_max_ms"):
            values = [r[key] for r in selected if r[key] is not None]
            result[key + "_median"] = statistics.median(values) if values else None
        groups.append(result)
    for name, data in (("runs", rows), ("groups", groups)):
        with (args.root / f"{name}.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(data[0]))
            writer.writeheader()
            writer.writerows(data)
    (args.root / "groups.json").write_text(json.dumps(groups, indent=2) + "\n")
    print(json.dumps(groups, indent=2))


if __name__ == "__main__":
    main()
