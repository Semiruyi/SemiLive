"""Join same-host QPC traces; report AU delivery and AU-ready latency."""
import argparse
import csv
import json
import math
from pathlib import Path


def read_trace(path, allow_empty=False):
    path = Path(path)
    status = dict(line.split("=", 1) for line in
                  Path(str(path) + ".status").read_text().splitlines())
    if status.get("write_ok") != "1":
        raise ValueError(f"Incomplete trace: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows and not allow_empty:
        raise ValueError(f"Empty trace: {path}")
    if int(status["records"]) != len(rows):
        raise ValueError(f"Trace count mismatch: {path}")
    for row in rows:
        for field in row.keys() - {"run_id", "machine", "clock"}:
            row[field] = int(row[field])
    return rows, int(status["dropped"])


def identity(row):
    return row["run_id"], row["machine"], row["clock"], row["frequency"]


def frame_key(row):
    return row["ssrc"], row["rtp_timestamp"]


def unique_index(rows):
    result = {}
    for row in rows:
        key = frame_key(row)
        if key in result:
            raise ValueError("Ambiguous RTP timestamp (wrap or restarted session)")
        result[key] = row
    return result


def distribution(values):
    if not values:
        return {"count": 0, "p50": None, "p95": None, "p99": None, "max": None}
    ordered = sorted(values)
    # Nearest-rank percentiles, explicitly shared by tests and reports.
    return {"count": len(values),
            **{f"p{p}": ordered[math.ceil(len(values) * p / 100) - 1]
               for p in (50, 95, 99)}, "max": ordered[-1]}


def analyze(publisher, receiver, warmup_seconds=5, duration_seconds=None, allow_loss=False):
    if not math.isfinite(warmup_seconds) or warmup_seconds < 0 or (
            duration_seconds is not None and (not math.isfinite(duration_seconds) or duration_seconds <= 0)):
        raise ValueError("Invalid measurement interval")
    ids = {identity(r) for r in publisher + receiver}
    if len(ids) != 1:
        raise ValueError("Traces must share run ID, host, clock and frequency")
    run_id, machine, clock, frequency = ids.pop()
    if clock != "qpc" or frequency <= 0:
        raise ValueError("Only same-host QPC measurement is supported")
    tx = unique_index(publisher)
    rx = unique_index(receiver)
    if any(r["capture"] <= 0 for r in publisher):
        raise ValueError("Publisher capture timestamp missing")
    start = min(r["capture"] for r in publisher) + warmup_seconds * frequency
    end = float("inf") if duration_seconds is None else start + duration_seconds * frequency
    selected = {k: r for k, r in tx.items() if start <= r["capture"] < end}
    frames = []
    invalid = []
    for key, p in selected.items():
        r = rx.get(key)
        if r is None:
            continue
        ticks = [p["capture"], p["encode_begin"], p["encode_complete"],
                 p["first_send"], r["first_receive"], r["au_ready"]]
        if (any(t <= 0 for t in ticks) or ticks != sorted(ticks)
                or p["last_send_complete"] < p["first_send"]
                or not r["first_receive"] <= r["last_receive"] <= r["au_ready"]
                or (not allow_loss and p["packet_count"] != r["packet_count"])):
            invalid.append({"ssrc": key[0], "rtp_timestamp": key[1]})
            continue
        names = ("capture_wait_ms", "encode_ms", "output_wait_ms",
                 "first_packet_ms", "receive_to_ready_ms")
        frame = {"frame_id": p["frame_id"], "ssrc": key[0],
                 "rtp_timestamp": key[1], "key_frame": p["key_frame"],
                 "au_bytes": p["au_bytes"], "packet_count": p["packet_count"],
                 "received_packet_count": r["packet_count"],
                 "capture_ticks": ticks[0], "ready_ticks": ticks[-1]}
        frame.update({name: (b - a) * 1000 / frequency
                      for name, a, b in zip(names, ticks, ticks[1:])})
        frame["capture_to_ready_ms"] = (ticks[-1] - ticks[0]) * 1000 / frequency
        frame["send_span_ms"] = (p["last_send_complete"] - p["first_send"]) * 1000 / frequency
        frames.append(frame)
    metrics = ("capture_wait_ms", "encode_ms", "output_wait_ms", "first_packet_ms",
               "receive_to_ready_ms", "capture_to_ready_ms", "send_span_ms")
    ready_times = sorted(f["ready_ticks"] for f in frames)
    summary = {"run_id": run_id, "machine": machine, "clock": clock,
               "frequency": frequency, "warmup_seconds": warmup_seconds,
               "duration_seconds": duration_seconds,
               "publisher_records": len(tx), "receiver_records": len(rx),
               "selected_frames": len(selected), "matched_frames": len(frames),
               "unmatched_publisher": len(selected.keys() - rx.keys()),
               "unmatched_receiver_total": len(rx.keys() - tx.keys()),
               "invalid_frames": invalid,
               "allow_loss": allow_loss,
               "delivery_percent": 100 * len(frames) / len(selected) if selected else None,
               "on_time_delivery_percent": {
                   str(limit): 100 * sum(f["capture_to_ready_ms"] <= limit for f in frames) / len(selected)
                   if selected else None for limit in (50, 100, 150, 200)},
               "au_delivery_gap_ms": distribution([(b-a)*1000/frequency for a,b in zip(ready_times, ready_times[1:])]),
               "metrics_ms": {m: distribution([f[m] for f in frames]) for m in metrics},
               "capture_to_ready_by_frame_type_ms": {
                   kind: distribution([f["capture_to_ready_ms"] for f in frames
                                       if bool(f["key_frame"]) == is_key])
                   for kind, is_key in (("idr", True), ("non_idr", False))}}
    return frames, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("publisher", type=Path)
    parser.add_argument("receiver", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--warmup-seconds", type=float, default=5)
    parser.add_argument("--duration-seconds", type=float)
    parser.add_argument("--allow-loss", action="store_true",
                        help="Allow missing AUs and retransmission packet-count differences")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    pub, pub_dropped = read_trace(args.publisher)
    rec, rec_dropped = read_trace(args.receiver, args.allow_loss)
    frames, summary = analyze(pub, rec, args.warmup_seconds, args.duration_seconds, args.allow_loss)
    summary["trace_dropped"] = {"publisher": pub_dropped, "receiver": rec_dropped}
    summary["valid"] = (bool(summary["selected_frames"]) and not summary["invalid_frames"]
                        and (args.allow_loss or not summary["unmatched_publisher"])
                        and not summary["unmatched_receiver_total"]
                        and not (pub_dropped or rec_dropped))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    with (args.output / "frames.csv").open("w", newline="", encoding="utf-8") as stream:
        if frames:
            writer = csv.DictWriter(stream, fieldnames=list(frames[0]))
            writer.writeheader()
            writer.writerows(frames)
    if not args.quiet:
        print(json.dumps(summary, indent=2))
    if not summary["valid"]:
        raise SystemExit("Measurement invalid; inspect summary.json")


if __name__ == "__main__":
    main()
