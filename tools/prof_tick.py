#!/usr/bin/env python3
"""Turn `sample` output for a live plusmc server into tick-cost attribution.

`tools/prof_plusmc.py` drives the real server under a named load and samples it
every millisecond. This reads those samples and answers the question the
optimisation work needs answered: **what is the tick thread spending its time
on, as a share of the 50 ms tick?**

Three things make that a measurement rather than a guess:

* `sample` samples each thread once per millisecond of wall time, so a thread's
  total sample count *is* its window in ms, and one sample is one millisecond of
  that thread's CPU. A tick is 50 ms, so `samples / (window_ms / 50)` is
  ms-of-CPU-per-tick for whatever subtree those samples are in.
* The tick thread's sleep subtree (`nanosleep`, `__semwait_signal`, …) is the
  loop parked between ticks, and is subtracted: only samples actually *doing*
  something count as tick work.
* Self time is inclusive minus the sum of children, so a helper reached from
  three sites is not billed three times. The report gives self (where the CPU
  really goes) and inclusive for the few roots that matter.

The tick thread is identified by which thread's tree contains `tick_world`,
never by name or by sample count — a sampled process has several threads all
showing the full window.

Usage:
    python3 tools/prof_tick.py /tmp/prof_idle.txt /tmp/prof_writes.txt ...
    python3 tools/prof_tick.py --top 25 /tmp/prof_drain.txt
"""

from __future__ import annotations

import argparse
import re
import sys

# Functions the tick loop parks in; samples there are the loop *waiting*, not
# working. Matched on the function name (no argument list) so `read` does not
# swallow every symbol that merely contains it.
IDLE_SYMBOLS = frozenset(("nanosleep", "__semwait_signal", "__psynch_cvwait", "select",
                          "poll", "kevent", "kevent_id", "mach_msg_trap", "recvfrom",
                          "__select", "__select_nocancel", "read$UNIX2003"))

LINE = re.compile(r"^([ +!:|*]*?)(\d+) (.+)$")


def base_name(sym: str) -> str:
    """`mc::server::tick_world(long long)` -> `mc::server::tick_world`."""
    return sym.split("(", 1)[0].strip()


def symbol_of(text: str) -> str:
    """`mc::world::foo(int)  (in plusmc) + 12  [0x…]` -> `mc::world::foo(int)`."""
    sym = text.split("  (in ")[0]
    sym = re.sub(r" \+ \d+(,\d+)*$", "", sym)
    sym = re.sub(r"\s*\[0x[0-9a-fA-F,]+.*$", "", sym)
    return sym.strip()


class Node:
    __slots__ = ("count", "sym", "children", "self_ms", "incl_ms", "parent")

    def __init__(self, count: int, sym: str):
        self.count = count
        self.sym = sym
        self.children: list["Node"] = []
        self.self_ms = 0
        self.incl_ms = 0
        self.parent: "Node | None" = None


class Thread:
    def __init__(self, name: str, count: int):
        self.name = name
        self.count = count
        self.roots: list[Node] = []

    def walk(self):
        stack = list(reversed(self.roots))
        while stack:
            n = stack.pop()
            yield n
            stack.extend(reversed(n.children))


def parse(path: str):
    """-> (threads, window_ms). The window is the largest thread's sample count."""
    threads: list[Thread] = []
    cur: Thread | None = None
    cur_node: Node | None = None
    stack: list[tuple[int, Node]] = []
    window = 0
    for raw in open(path, "r", errors="replace"):
        line = raw.rstrip("\n")
        if not line or line.startswith("[") or line.startswith("Binary Images") or \
                line.startswith("Call graph"):
            continue
        m = LINE.match(line)
        if not m:
            continue
        prefix, count, text = m.group(1), int(m.group(2)), m.group(3)
        if not prefix.strip(" "):
            cur = Thread(text.strip(), count)
            threads.append(cur)
            stack = []
            window = max(window, count)
            continue
        if cur is None:
            continue
        depth = max(0, (len(prefix) - len("    + ")) // 2)
        node = Node(count, symbol_of(text))
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if stack:
            stack[-1][1].children.append(node)
            node.parent = stack[-1][1]
        else:
            cur.roots.append(node)
        stack.append((depth, node))
    return threads, window


def roll_up(thread: Thread) -> None:
    """Fill self/inclusive on every node of one thread's tree."""
    order: list[Node] = []

    def visit(n: Node) -> None:
        for c in n.children:
            visit(c)
        n.incl_ms = n.count
        n.self_ms = n.count - sum(c.count for c in n.children)
        order.append(n)

    for root in thread.roots:
        visit(root)


def find_thread(threads: list[Thread], needle: str) -> Thread | None:
    for t in threads:
        if any(needle in n.sym for n in t.walk()):
            return t
    return None


def subtree_inclusive(thread: Thread) -> dict[str, int]:
    out: dict[str, int] = {}
    for n in thread.walk():
        out[n.sym] = out.get(n.sym, 0) + n.count
    return out


def idle_samples(thread: Thread) -> int:
    total = 0
    for n in thread.walk():
        if base_name(n.sym) in IDLE_SYMBOLS:
            total += n.count
            for c in n.children:
                total -= c.count  # do not double-count parked-below frames
    return total


def short(sym: str, width: int = 96) -> str:
    sym = sym.replace("std::__1::", "")
    return sym if len(sym) <= width else sym[: width - 1] + "…"


def callers(thread: Thread, needle: str, ticks: float, top: int, width: int = 5) -> None:
    """Aggregate the ancestor chains of every node whose symbol contains `needle`.

    Self time alone cannot say where the CPU goes: `_platform_memcmp` is a leaf
    with no opinion about who asked it to compare strings. This walks up from
    each matching node and prints the shortest informative chain, which is what
    turns "memcmp is hot" into "memcmp is hot *because* the collision predicate
    is comparing block names".
    """
    agg: dict[tuple[str, ...], int] = {}
    for n in thread.walk():
        if needle not in n.sym:
            continue
        chain: list[str] = []
        p = n.parent
        while p is not None and len(chain) < width:
            chain.append(p.sym)
            p = p.parent
        key = tuple(reversed(chain))
        agg[key] = agg.get(key, 0) + n.count
    print(f"   -- callers of {needle} --")
    for chain, count in sorted(agg.items(), key=lambda kv: -kv[1])[:top]:
        print(f"   {count / ticks:8.3f} ms/tick  {short(' <- '.join(chain), 150)}")


def report(path: str, top: int, label: str, paths: list[str]) -> None:
    threads, window = parse(path)
    if not threads:
        print(f"== {label}: no call graph in {path} ==\n")
        return
    tick = find_thread(threads, "tick_world") or max(threads, key=lambda t: t.count)
    for t in threads:
        roll_up(t)
    idle = idle_samples(tick)
    work = tick.count - idle
    ticks = window / 50.0 if window else 1.0
    per_tick = work / ticks
    self_ms: dict[str, int] = {}
    incl = subtree_inclusive(tick)
    for n in tick.walk():
        self_ms[n.sym] = self_ms.get(n.sym, 0) + n.self_ms

    print(f"== {label} ({path}) ==")
    print(f"   window {window} ms ({ticks:.0f} ticks) | tick thread {tick.name.split()[0]}"
          f" | parked {idle} samples ({idle / ticks:.2f} ms/tick) | work {work} samples"
          f" ({per_tick:.2f} ms/tick = {100.0 * per_tick / 50.0:.1f}% of a 50 ms tick)")

    rank = sorted(((ms, sym) for sym, ms in self_ms.items()
                   if ms > 0 and base_name(sym) not in IDLE_SYMBOLS), reverse=True)
    print(f"   -- self time, top {top} --")
    for ms, sym in rank[:top]:
        share = 100.0 * ms / work if work else 0.0
        print(f"   {ms / ticks:8.3f} ms/tick  {share:5.1f}%  {short(sym)}")

    roots = ("mc::server::tick_world", "mc::world::relight_world_chunk", "operator new",
             "mc::world::compute_chunk_light")
    print("   -- inclusive roots --")
    for sym, ms in sorted(incl.items(), key=lambda kv: -kv[1]):
        if any(sym.startswith(r) for r in roots) and ms > 0:
            print(f"   {ms / ticks:8.3f} ms/tick  {100.0 * ms / work if work else 0:5.1f}%  "
                  f"{short(sym)}")

    for pat in paths:
        callers(tick, pat, ticks, top=6)

    others = [t for t in threads if t is not tick]
    if others:
        print("   -- other threads --")
        for t in sorted(others, key=lambda t: -(t.count - idle_samples(t)))[:5]:
            busy = t.count - idle_samples(t)
            s_ms = {}
            for n in t.walk():
                s_ms[n.sym] = s_ms.get(n.sym, 0) + n.self_ms
            top_sym = max(s_ms.items(), key=lambda kv: kv[1]) if s_ms else ("-", 0)
            print(f"   {t.name.split()[0]:>16} {t.count:7d} samples, {busy:6d} busy "
                  f"({busy * 1.0 / ticks:6.2f} ms/tick), top self "
                  f"{100.0 * top_sym[1] / max(1, busy):5.1f}% {short(top_sym[0], 60)}")
    print()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--phase", default=None, help="label for a single file")
    ap.add_argument("--callers", action="append", default=[],
                    help="aggregate the ancestor chains of frames containing this text")
    args = ap.parse_args()
    for path in args.files:
        label = args.phase or path.rsplit("/", 1)[-1].replace("prof_", "").replace(".txt", "")
        report(path, args.top, label, args.callers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
