"""Run AI source regressions using pip's lupa LuaJIT 2.1 runtime."""
import argparse
from pathlib import Path
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--lupa-dir", type=Path)
args = parser.parse_args()
if args.lupa_dir:
    sys.path.insert(0, str(args.lupa_dir))
from lupa.luajit21 import LuaRuntime

root = Path(__file__).resolve().parents[1]
lua = LuaRuntime(unpack_returned_tuples=True)
lua.globals().ROOT = root.as_posix()
lua.execute((root / "Tests/GoldMiningTests.lua").read_text(encoding="utf-8"))
