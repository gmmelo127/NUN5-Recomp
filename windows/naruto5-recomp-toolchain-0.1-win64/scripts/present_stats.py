import csv
import statistics
import sys
from pathlib import Path

path = Path(sys.argv[1] if len(sys.argv) > 1 else "games/naruto/present.csv")
rows = list(csv.DictReader(open(path)))
ps = [int(r["us"]) for r in rows if r.get("kind") == "P"]
if len(ps) < 2:
    print("sem presents suficientes")
    raise SystemExit(1)
d = sorted((ps[i] - ps[i - 1]) / 1000.0 for i in range(1, len(ps)))
span = (ps[-1] - ps[0]) / 1e6
print("frames=%d span=%.2fs fps=%.1f" % (len(ps), span, (len(ps) - 1) / span if span else 0))
print("avg_ms=%.2f median=%.2f p95=%.2f p99=%.2f max=%.2f" % (
    statistics.mean(d), statistics.median(d), d[int(len(d) * .95)], d[int(len(d) * .99)], d[-1]))
print("acima_20ms=%.1f%% acima_33ms=%.1f%%" % (
    sum(1 for x in d if x > 20) * 100.0 / len(d), sum(1 for x in d if x > 33.34) * 100.0 / len(d)))
