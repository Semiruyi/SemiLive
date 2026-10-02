"""Compare two capacity experiments; desktop content is not replayed identically."""
import argparse
import csv
import json
from pathlib import Path


def load(root):
    groups = {g["loss_percent"]: g for g in json.loads((root / "groups.json").read_text())}
    runs = []
    for path in root.glob("loss-*/relay-*/receiver.json"):
        directory = path.parent
        receiver = json.loads(path.read_text())
        publisher = json.loads((directory / "publisher.json").read_text())
        manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))
        with (directory / "publisher.csv").open(newline="") as stream:
            max_packets = max((int(r["packet_count"]) for r in csv.DictReader(stream)), default=0)
        runs.append({"loss": manifest["loss_percent"], "seed": manifest["seed"],
                     "capacity": receiver["config"]["rtp"]["reorder_maximum_buffered_packets"],
                     "conditions": (publisher["config"]["video"], manifest["warmup_seconds"],
                                    manifest["duration_seconds"], manifest["rtcp_enabled"],
                                    receiver["config"]["rtp"]["reorder_maximum_hold_ms"]),
                     "capacity_gaps": receiver["rtp"]["reorder"]["capacity_gaps"],
                     "timeout_gaps": receiver["rtp"]["reorder"]["timeout_gaps"],
                     "max_au_packets": max_packets})
    return groups, runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    old, old_runs = load(args.baseline)
    new, new_runs = load(args.candidate)
    old_index = {(r["loss"], r["seed"]): r for r in old_runs}
    new_index = {(r["loss"], r["seed"]): r for r in new_runs}
    if old_index.keys() != new_index.keys() or not old_index:
        raise SystemExit("Loss levels or seeds differ")
    if any(old_index[k]["conditions"] != new_index[k]["conditions"] for k in old_index):
        raise SystemExit("Media settings, duration, RTCP or hold time differ")
    rows = []
    for loss in sorted(old):
        left = [r for r in old_runs if r["loss"] == loss]
        right = [r for r in new_runs if r["loss"] == loss]
        a, b = old[loss], new[loss]
        rows.append({"loss_percent": loss,
                     "baseline_packets": left[0]["capacity"], "candidate_packets": right[0]["capacity"],
                     "delivery_baseline_percent": a["delivery_percent_median"],
                     "delivery_candidate_percent": b["delivery_percent_median"],
                     "delivery_improvement_points": b["delivery_percent_median"] - a["delivery_percent_median"],
                     "p95_baseline_ms": a["p95_ms_median"], "p95_candidate_ms": b["p95_ms_median"],
                     "candidate_runs_with_delivery": b["runs_with_delivery"],
                     "capacity_gaps_baseline_total": sum(r["capacity_gaps"] for r in left),
                     "capacity_gaps_candidate_total": sum(r["capacity_gaps"] for r in right),
                     "timeout_gaps_baseline_total": sum(r["timeout_gaps"] for r in left),
                     "timeout_gaps_candidate_total": sum(r["timeout_gaps"] for r in right),
                     "max_au_packets_baseline": max(r["max_au_packets"] for r in left),
                     "max_au_packets_candidate": max(r["max_au_packets"] for r in right)})
    with (args.candidate / "comparison.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (args.candidate / "comparison.json").write_text(json.dumps(rows, indent=2) + "\n")
    print(json.dumps(rows, indent=2))


if __name__ == "__main__":
    main()
