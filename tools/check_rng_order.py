#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail on C++ expressions whose evaluation order the standard leaves open.

The bug this exists to prevent (docs/LINUX_PORT.md, "The worldgen divergence"):
three `next_int()` calls on one shared generator sat inside a single
`pack_block_pos(...)` argument list. Which draw landed in which parameter was
then the compiler's choice -- clang evaluates arguments left to right, gcc right
to left -- so the aquifer picked a different centre, and the same seed generated
stone on macOS and water on Linux. Nothing in the code looked wrong, both
compilers were correct, and only a platform comparison could see it.

The rule the checker enforces: **two draws on a stateful generator must not share
a full-expression unless something between them sequences the two.**

  * ordered -- one draw nested inside the other's argument list (the inner call
               completes first);
  * ordered -- separated by a sequencing operator: `&&`, `||`, `?:` (or being the
               two branches of one ternary, of which only one runs), a comma
               *operator*, or a braced initialiser list (left to right by
               definition); a `;` starts a new full-expression anyway;
  * hazard  -- two draws as arguments of one call, or as the operands of one
               binary operator, or in sibling calls of one argument list.

A `,` inside a call's argument list is a *separator*, not the comma operator, so
it does not sequence those arguments; the same character inside a parenthesised
sub-expression is the comma operator and does. Keywords (`if (...)`, `while (...)`)
parenthesise a full expression too, so their commas are operators. A brace group
is only treated as a sequencing initialiser list when it *is* one -- a function
body is a brace group as well, and its statements are separated by the `;`
counter instead.

**Scope**: `src/` is the hard-fail scope, and that is the game. `tests/`,
`tools/` and `bench/` are scanned and reported as advisory, because a test
double's `next_int()`/`unit_random()` can *return the draw* rather than draw
(`mob_spawn_test.cpp`'s `Script`/`Seeded` do) and no syntax-only check can tell
the two apart; `--strict` promotes them to failures for a manual sweep.

Draw calls are the generator's own: `next_int` / `next_long` / `next_float` /
`next_double` / `next_boolean` (worldgen_random.h and the other RNGs), plus
`unit_random()`, the mob environment's draw. Only calls count.

Run over the tree (0 hits, exit 0) or with `--explain` for the scan counts;
`--path` narrows the scan, which is also how the negative and positive cases are
exercised.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Calls that advance a shared generator.
DRAW_NAMES = ("next_int", "next_long", "next_float", "next_double", "next_boolean", "unit_random")

# A `(` preceded by one of these parenthesises a full expression (or a specifier)
# rather than passing arguments, so a comma inside it is the comma operator.
KEYWORDS = {
    "if", "while", "for", "switch", "catch", "return", "sizeof", "decltype",
    "static_assert", "assert", "throw", "typeid", "alignof", "noexcept",
    "and", "or", "not", "co_await", "co_yield", "co_return", "case", "do", "else",
}

# The keywords whose parenthesised clause is followed by a sequenced statement.
CONTROL = {"if", "while", "for", "switch", "catch", "do", "else"}

# The words a `{` can follow as a *block*, not an initialiser list.
BLOCK_WORDS = {"else", "do", "try", "namespace", "struct", "class", "union", "enum", "catch"}

# Everything the scan needs to look at: the draws, and the punctuation that
# builds the group tree and the statement boundaries. Match-driven, so the cost
# is the number of these, not the size of the file.
TOKENS = re.compile(
    r"next_int|next_long|next_float|next_double|next_boolean|unit_random|[()\[\]{};]"
)

# A string, a char literal, a line comment or a block comment: one pass, each
# replaced by spaces (newlines kept, so offsets and line numbers survive).
NOISE = re.compile(
    r'"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*'"
    r"|//[^\n]*"
    r"|/\*.*?\*/",
    re.DOTALL,
)

SKIP_DIRS = {"build", "build-linux", ".git", "node_modules", "third_party", "extern"}
SUFFIXES = (".cpp", ".h", ".hpp", ".cc", ".cxx")
STRICT_ROOTS = ("src",)
ADVISORY_ROOTS = ("tests", "tools", "bench")


def blank_noise(text: str) -> str:
    """Replace strings and comments with spaces, keeping offsets and newlines."""
    return NOISE.sub(
        lambda m: "".join("\n" if ch == "\n" else " " for ch in m.group(0)), text
    )


def prev_nonspace(code: str, i: int) -> int:
    i -= 1
    while i >= 0 and code[i] in " \t\n":
        i -= 1
    return i


def word_before(code: str, i: int) -> str:
    j = i
    while j >= 0 and (code[j].isalnum() or code[j] == "_"):
        j -= 1
    return code[j + 1 : i + 1]


def draw_at(code: str, i: int) -> int | None:
    """The offset of the `(` when a draw call starts at `i`, else None."""
    prev = code[i - 1] if i else " "
    if prev.isalnum() or prev == "_":
        return None  # the tail of a longer identifier
    for name in DRAW_NAMES:
        if code.startswith(name, i):
            j = i + len(name)
            while j < len(code) and code[j] in " \t\n":
                j += 1
            return j if j < len(code) and code[j] == "(" else None
    return None


def group_end(code: str, open_at: int) -> int:
    """Offset just past the bracket matching the opener at `open_at`."""
    depth = 0
    for i in range(open_at, len(code)):
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(code)


class Group:
    __slots__ = ("kind", "open", "close", "call_args", "init_list", "control")

    def __init__(self, kind: str, open_at: int, call_args: bool, init_list: bool,
                 control: bool = False) -> None:
        self.kind = kind
        self.open = open_at
        self.close = -1
        self.call_args = call_args
        self.init_list = init_list
        # The parentheses of `if`/`while`/`for`/`switch`/`catch`: the body that
        # follows the `)` is sequenced after the condition, so a draw in the
        # condition and a draw in the body cannot be unordered.
        self.control = control


def is_call_args(code: str, open_at: int) -> bool:
    """Whether the `(` at `open_at` opens an argument list rather than a group."""
    prev = prev_nonspace(code, open_at)
    if prev < 0 or not (code[prev].isalnum() or code[prev] in "_)]"):
        return False
    if code[prev] in ")]":
        return True
    return word_before(code, prev) not in KEYWORDS


def is_init_list(code: str, open_at: int) -> bool:
    """Whether the `{` at `open_at` is a braced initialiser list (sequenced)."""
    prev = prev_nonspace(code, open_at)
    if prev < 0:
        return True
    c = code[prev]
    if c in "=(,{>]":
        return True
    if c in ")};:":
        return False  # a function body, a statement block, a label's block
    if c.isalnum() or c == "_":
        return word_before(code, prev) not in BLOCK_WORDS
    return False


def scan_code(code: str, path: str):
    """Return (draw-call count, [hazard messages]) for one pre-cleaned file."""
    groups: list[Group] = []
    stack: list[int] = []
    statement = 0
    draws: list[tuple[int, int, int, tuple[int, ...]]] = []  # start, end, stmt, groups
    skip_until = 0

    for m in TOKENS.finditer(code):
        i = m.start()
        if i < skip_until:
            continue
        text = m.group(0)
        if text == ";":
            statement += 1
            continue
        if text in "([{":
            init = text == "{" and is_init_list(code, i)
            call_args = text == "(" and is_call_args(code, i)
            control = (text == "(" and not call_args and
                       word_before(code, prev_nonspace(code, i)) in CONTROL)
            g = Group(text, i, call_args, init, control)
            groups.append(g)
            stack.append(len(groups) - 1)
            if text == "{":
                statement += 1
            continue
        if text in ")]}":
            if stack:
                closed = groups[stack.pop()]
                closed.close = i
                if text == ")" and closed.control:
                    statement += 1
            if text == "}":
                statement += 1
            continue
        open_at = draw_at(code, i)
        if open_at is None:
            continue
        end = group_end(code, open_at)
        draws.append((i, end, statement, tuple(stack)))
        groups.append(Group("(", open_at, True, False))
        groups[-1].close = end - 1
        skip_until = end

    if len(draws) < 2:
        return len(draws), []

    by_statement: dict[int, list[tuple[int, int, int, tuple[int, ...]]]] = {}
    for d in draws:
        by_statement.setdefault(d[2], []).append(d)

    hazards: list[str] = []
    for stmt, items in sorted(by_statement.items()):
        if len(items) < 2:
            continue
        # The outermost draws only: a draw inside another draw's argument list is
        # ordered against it, and cannot be unordered with anything else either.
        top = [d for d in items if not any(o[0] < d[0] < o[1] for o in items)]
        first_bad = last_bad = None
        for a, b in zip(top, top[1:]):
            if _ordered(groups, code, a, b):
                continue
            if first_bad is None:
                first_bad = a
            last_bad = b
        if first_bad is None:
            continue
        snippet = " ".join(
            code[_statement_start(code, first_bad[0]) : _statement_end(code, last_bad[1])].split()
        )
        if len(snippet) > 150:
            snippet = snippet[:147] + "..."
        line = code.count("\n", 0, first_bad[0]) + 1
        hazards.append(f"{path}:{line}: unordered RNG draws: {snippet}")
    return len(draws), hazards


def _ordered(groups: list[Group], code: str, a, b) -> bool:
    """Whether something between draws `a` and `b` sequences them."""
    lca = _lca(groups, a[3], b[3])
    if lca is not None and groups[lca].init_list:
        return True  # a braced initialiser list is sequenced left to right
    span_start = groups[lca].open + 1 if lca is not None else _statement_start(code, a[0])
    prefix = _mask(code[span_start : a[0]])
    between = _mask(code[a[1] : b[0]])
    if "?" in prefix and ":" in between:
        return True  # the two branches of one ternary: only one of them runs
    if "?" in between or "&&" in between or "||" in between:
        return True
    if "," in between:
        # A comma operator sequences; an argument-list separator does not.
        return lca is None or not (groups[lca].kind == "(" and groups[lca].call_args)
    return False


def _lca(groups: list[Group], path_a: tuple[int, ...], path_b: tuple[int, ...]):
    common = 0
    for x, y in zip(path_a, path_b):
        if x != y:
            break
        common += 1
    return path_a[common - 1] if common else None


def _mask(span: str) -> str:
    """`span` with every nested bracket group blanked, keeping depth 0 only."""
    out = []
    depth = 0
    for ch in span:
        if ch in "([{":
            depth += 1
            out.append(" ")
        elif ch in ")]}":
            if depth:
                depth -= 1
            out.append(" ")
        elif depth:
            out.append(" ")
        else:
            out.append(ch)
    return "".join(out)


def _statement_start(code: str, pos: int) -> int:
    i = pos
    while i > 0 and code[i - 1] not in ";{}":
        i -= 1
    return i


def _statement_end(code: str, pos: int) -> int:
    i = pos
    while i < len(code) and code[i] not in ";{}":
        i += 1
    return i


def collect(roots: list[Path]) -> list[Path]:
    files: list[Path] = []
    for root in roots:
        if root.is_file():
            files.append(root)
        elif root.is_dir():
            files.extend(
                p
                for p in root.rglob("*")
                if p.suffix in SUFFIXES and not (SKIP_DIRS & set(p.parts))
            )
    return sorted(files)


def main() -> int:
    ap = argparse.ArgumentParser(description="Fail on unordered shared-generator draws.")
    ap.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent))
    ap.add_argument("--path", action="append", default=None,
                    help="scan this file or directory instead of the repo's sources")
    ap.add_argument("--strict", action="store_true",
                    help="treat hits in tests/, tools/ and bench/ as failures too")
    ap.add_argument("--explain", action="store_true")
    args = ap.parse_args()

    repo = Path(args.repo)
    if args.path:
        roots = [Path(p) for p in args.path]
        strict_for = roots if args.strict else []
        advisory_for = [] if args.strict else roots
    else:
        strict_for = [repo / d for d in STRICT_ROOTS]
        advisory_for = [repo / d for d in ADVISORY_ROOTS]

    calls = 0
    scanned = 0
    fails: list[str] = []
    notes: list[str] = []
    for roots, bucket in ((strict_for, fails), (advisory_for, notes)):
        files = collect(roots)
        for path in files:
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "next_" not in text and "unit_random" not in text:
                continue
            scanned += 1
            count, found = scan_code(blank_noise(text), str(path))
            calls += count
            bucket.extend(found)

    if args.explain:
        print(f"scanned {scanned} file(s), {calls} draw call(s)", file=sys.stderr)
    if notes:
        print(
            f"rng-order: {len(notes)} advisory hit(s) in tests/tools/bench -- a test "
            f"double's `next_int()`/`unit_random()` can return the draw instead of "
            f"drawing, which no syntax check can tell apart (`--strict` fails on "
            f"them):",
            file=sys.stderr,
        )
        for n in notes:
            print(f"  {n}", file=sys.stderr)
    for h in fails:
        print(h)
    if fails:
        print(
            f"\n{len(fails)} risky expression(s): a shared generator advances in an "
            f"order the compiler chooses (clang left to right, gcc right to left). "
            f"Pull each draw into its own statement -- see docs/LINUX_PORT.md."
        )
        return 1
    print(f"rng-order: clean ({calls} draw calls, none sharing an unsequenced expression)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
