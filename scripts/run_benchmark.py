import argparse
import os
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--benchmark", choices=["micro", "bfs", "pagerank"], required=True)
parser.add_argument("--threads", type=int, default=2)
parser.add_argument("--reps", type=int)
parser.add_argument("--memory-mib", type=int, default=128)
parser.add_argument("--graph-scale", type=int, default=18)
parser.add_argument("--density", type=int, default=100)
parser.add_argument("--hot-density", type=int)
parser.add_argument("--pattern", choices=["sequential", "random"], default="sequential")
parser.add_argument("--allocator", choices=["glibc", "tcmalloc"], default="glibc")
args = parser.parse_args()

root = Path(__file__).resolve().parents[1]
env = os.environ.copy()
env["OMP_NUM_THREADS"] = str(args.threads)

if args.allocator == "tcmalloc":
    env["LD_PRELOAD"] = "libtcmalloc_minimal.so.4"
else:
    env.pop("LD_PRELOAD", None)

if args.benchmark == "micro":
    reps = args.reps if args.reps is not None else 100
    hot_density = args.hot_density if args.hot_density is not None else args.density

    cmd = ["/tmp/sequential", str(args.memory_mib), str(reps), str(args.density), str(args.threads), args.pattern, str(hot_density)]

else:
    reps = args.reps if args.reps is not None else 1
    executable = "pr" if args.benchmark == "pagerank" else "bfs"

    cmd = [str(root / "third_party/gapbs" / executable), "-g", str(args.graph_scale), "-n", str(reps)]

subprocess.run(cmd, env=env, check=True)
