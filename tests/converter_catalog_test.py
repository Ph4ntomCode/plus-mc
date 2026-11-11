#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC Converter catalog (Phase 7, slice 4).

`plusmc-convert catalog <dir>` scans every JAR under a directory and writes a
versioned index of what the analyzer and mapper found: score, declared
api-version, reference coverage and port status. This test reuses the
analyzer's own class-file/JAR writer, so the fixtures cannot drift, and pins:

  * one entry per jar for all four statuses: portable, partial, refused and
    unreadable;
  * sorted names relative to the scanned directory, nested jars included;
  * the summary counts, the JSON shape and the text report;
  * deterministic output and the `--out` index file (including parent
    directory creation);
  * the Verified report: status/coverage ranking, the `--top` cap, deterministic
    Markdown bytes, a report written before the coverage gate fails, and its
    own option refusals;
  * refusals for a missing directory, an empty directory, a file path, and a
    missing or repeated `--out`.

Run: converter_catalog_test.py /path/to/plusmc-convert
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import converter_test  # noqa: E402

TIMEOUT = 60


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"plusmc-convert exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-6000:]}\nstderr:\n{proc.stderr[-6000:]}"
        )
    return proc


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: converter_catalog_test.py <plusmc-convert>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-converter-catalog-") as temporary:
        root = Path(temporary)
        plugins = root / "plugins"
        (plugins / "nested").mkdir(parents=True)

        # Only the mapped plugin/event/command surface: every class reference
        # has a native symbol, so this is the portable case.
        portable = converter_test.class_bytes(
            "com/example/Portable",
            super_name="org/bukkit/plugin/java/JavaPlugin",
            interfaces=["org/bukkit/event/Listener", "org/bukkit/command/CommandExecutor"],
        )
        converter_test.make_jar(
            plugins / "alpha.jar",
            {"com/example/Portable.class": portable},
            converter_test.MANIFEST,
        )

        # Wider API plus a world call with no ABI equivalent: scaffoldable, but
        # the work diff is not empty.
        partial = converter_test.class_bytes(
            "com/example/Partial",
            super_name="org/bukkit/plugin/java/JavaPlugin",
            classes=["org/bukkit/inventory/Inventory"],
            methods=[
                (
                    "org/bukkit/World",
                    "spawn",
                    "(Lorg/bukkit/Location;Ljava/lang/Class;)Lorg/bukkit/entity/Entity;",
                )
            ],
        )
        converter_test.make_jar(
            plugins / "nested" / "beta.jar",
            {"com/example/Partial.class": partial},
            converter_test.MANIFEST,
        )

        # A class archive with no manifest: `analyze` reports it, `port` would
        # refuse it. The catalog records that refusal instead of dropping it.
        plain = converter_test.class_bytes("com/example/Plain")
        converter_test.make_jar(plugins / "plain.jar", {"com/example/Plain.class": plain})

        (plugins / "broken.jar").write_bytes(b"this is not a zip archive")
        (plugins / "notes.txt").write_text("not a plugin")

        text = run(cli, ["catalog", plugins]).stdout
        assert "+MC converter catalog" in text, text
        assert "jars: 4 (1 portable, 1 partial, 1 refused, 1 unreadable)" in text, text
        assert "scores: 1 green, 1 yellow, 1 red (3 analyzed)" in text, text
        assert "portable  alpha.jar  Example 1.2.3" in text, text
        assert "api-version 1.20" in text, text
        assert "partial  nested/beta.jar  Example 1.2.3" in text, text
        assert "refused  plain.jar  (no plugin.yml or paper-plugin.yml)" in text, text
        assert "unreadable  broken.jar" in text, text
        assert "archive(s) could not be read" in text, text
        assert "would be refused by port" in text, text
        print("PASS catalog text report")

        index = json.loads(run(cli, ["catalog", plugins, "--json"]).stdout)
        assert index["tool"] == "plusmc-convert", index
        assert index["schema"] == 1, index
        assert index["mode"] == "catalog", index
        assert index["abi"] == 1, index
        assert index["ok"] is True, index
        assert Path(index["directory"]).resolve() == plugins.resolve(), index
        summary = index["summary"]
        assert summary == {
            "jars": 4,
            "analyzed": 3,
            "portable": 1,
            "partial": 1,
            "refused": 1,
            "unreadable": 1,
            "green": 1,
            "yellow": 1,
            "red": 1,
        }, summary
        entries = index["entries"]
        assert [entry["file"] for entry in entries] == [
            "alpha.jar",
            "broken.jar",
            "nested/beta.jar",
            "plain.jar",
        ], entries

        alpha = entries[0]
        assert alpha["status"] == "portable", alpha
        assert alpha["score"] == "green", alpha
        assert alpha["name"] == "Example" and alpha["version"] == "1.2.3", alpha
        assert alpha["api_version"] == "1.20", alpha
        assert alpha["gap"] == 0 and alpha["partial_refs"] == 0, alpha
        assert alpha["coverage"] == 1.0, alpha

        broken = entries[1]
        assert broken["status"] == "unreadable", broken
        assert broken["score"] is None, broken
        assert broken["reason"], broken
        assert broken["coverage"] == 0.0, broken

        beta = entries[2]
        assert beta["status"] == "partial", beta
        assert beta["score"] == "yellow", beta
        assert beta["gap"] >= 1, beta
        assert 0.0 <= beta["coverage"] < 1.0, beta
        assert "gap" in beta["reason"], beta

        plain_entry = entries[3]
        assert plain_entry["status"] == "refused", plain_entry
        assert plain_entry["score"] == "red", plain_entry
        assert "no plugin.yml" in plain_entry["reason"], plain_entry
        print("PASS catalog JSON index")

        # The index file is the JSON report, and two scans of the same tree are
        # byte-identical (no timestamp, no absolute-order dependence).
        destination = root / "generated" / "index.json"
        run(cli, ["catalog", plugins, "--out", destination])
        written = destination.read_bytes()
        assert json.loads(written)["mode"] == "catalog", written[:400]
        assert written == run(cli, ["catalog", plugins, "--json"]).stdout.encode(), (
            "the --out file and the stdout JSON differ"
        )
        second = root / "second.json"
        run(cli, ["catalog", plugins, "--out=" + str(second)])
        assert second.read_bytes() == written, "catalog output is not deterministic"
        print("PASS index file and deterministic output")

        # The Verified report: the same scan as the document a catalog page
        # publishes. Ranking is the review order (portable, partial, refused,
        # unreadable; coverage desc inside a status), and the bytes are stable.
        report = root / "site" / "verified.md"
        run(cli, ["catalog", plugins, "--report", report])
        markdown = report.read_text(encoding="utf-8")
        assert markdown.startswith("# +MC Verified catalog"), markdown
        assert "| jars | 4 (1 portable, 1 partial, 1 refused, 1 unreadable) |" in markdown, markdown
        assert "| scores | 1 green, 1 yellow, 1 red (3 analyzed) |" in markdown, markdown
        assert "| 1 | `alpha.jar` | Example 1.2.3 | portable | 100% | green |" in markdown, markdown
        assert "| 2 | `nested/beta.jar` | Example 1.2.3 | partial |" in markdown, markdown
        assert "| 3 | `plain.jar` | - | refused | - | red |" in markdown, markdown
        assert "| 4 | `broken.jar` | - | unreadable | - | - |" in markdown, markdown
        assert "Not a portability proof" in markdown, markdown
        again = root / "site" / "again.md"
        run(cli, ["catalog", plugins, "--report", again])
        assert again.read_bytes() == report.read_bytes(), "the report is not deterministic"
        capped = root / "site" / "top.md"
        run(cli, ["catalog", plugins, "--report", capped, "--top", "2"])
        top_text = capped.read_text(encoding="utf-8")
        assert "| 1 | `alpha.jar`" in top_text and "| 2 | `nested/beta.jar`" in top_text, top_text
        assert "| 3 |" not in top_text, top_text
        assert "Showing the first 2 of 4 entries" in top_text, top_text
        print("PASS Verified report")

        # The gate checks every portable/partial entry, writes the index and
        # the report first, and refuses only after the artifacts are on disk.
        gate = root / "gate" / "index.json"
        gate_report = root / "gate" / "verified.md"
        gated = run(
            cli,
            ["catalog", plugins, "--out", gate, "--report", gate_report,
             "--fail-under", "100"],
            expect=1,
        )
        assert "nested/beta.jar" in gated.stderr, gated.stderr
        assert "below --fail-under 100%" in gated.stderr, gated.stderr
        assert json.loads(gate.read_bytes())["mode"] == "catalog", gate
        assert gate_report.exists(), "the report was not written before the gate failed"
        assert run(cli, ["catalog", plugins, "--fail-under", "0"]).returncode == 0
        proc = run(cli, ["catalog", plugins, "--fail-under", "101"], expect=2)
        assert "0 to 100" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins, "--fail-under"], expect=2)
        assert "--fail-under needs a percentage" in proc.stderr, proc.stderr
        proc = run(
            cli,
            ["catalog", plugins, "--fail-under", "10", "--fail-under", "20"],
            expect=2,
        )
        assert "given more than once" in proc.stderr, proc.stderr
        print("PASS --fail-under gate")

        missing = run(cli, ["catalog", root / "absent"], expect=1)
        assert "not a directory" in missing.stderr, missing.stderr
        empty = root / "empty"
        empty.mkdir()
        proc = run(cli, ["catalog", empty], expect=1)
        assert "no .jar files" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins / "notes.txt"], expect=1)
        assert "not a directory" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins, "--out"], expect=2)
        assert "--out needs a file" in proc.stderr, proc.stderr
        proc = run(
            cli,
            ["catalog", plugins, "--out", root / "a.json", "--out", root / "b.json"],
            expect=2,
        )
        assert "--out given more than once" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog"], expect=2)
        assert "usage:" in proc.stdout, proc.stdout
        proc = run(cli, ["catalog", plugins, "--report"], expect=2)
        assert "--report needs a file" in proc.stderr, proc.stderr
        proc = run(
            cli,
            ["catalog", plugins, "--report", root / "a.md", "--report", root / "b.md"],
            expect=2,
        )
        assert "--report given more than once" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins, "--top", "2"], expect=2)
        assert "--top needs --report" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins, "--report", root / "c.md", "--top", "0"], expect=2)
        assert "1 to 10000" in proc.stderr, proc.stderr
        proc = run(
            cli,
            ["catalog", plugins, "--report", root / "c.md", "--top", "2", "--top", "3"],
            expect=2,
        )
        assert "--top given more than once" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", root / "absent", "--report", root / "nope.md"], expect=1)
        assert not (root / "nope.md").exists(), "a failed scan wrote a report"
        print("PASS catalog refusals")

    print("converter-catalog: all checks passed")


if __name__ == "__main__":
    main()
