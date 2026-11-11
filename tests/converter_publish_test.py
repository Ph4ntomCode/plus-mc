#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC Converter's published catalog (Phase 7, slice 9).

`catalog --publish <dir>` turns a scan into the directory a static host
serves: `index.json`, `verified.md`, `history.tsv`, `index.html` and a
`SHA256SUMS` over all four. `--history` keeps the append-only ledger, and
`--sign-key` / `verify-site` add the review signature. This test reuses the
analyzer's own class-file/JAR writer and pins:

  * the published artifact set, the sums over it and the static page;
  * run 1 of the ledger, and that re-publishing an unchanged scan adds no run
    and rewrites nothing (byte-identical artifacts, reproduced delta);
  * run 2's added/removed/unchanged counts and run 3's changed entry, in both
    the text report and the index JSON;
  * the Ed25519 signature: a signed site verifies with its own public key, a
    tampered artifact and a tampered sums file are refused, and an unsigned
    site is checked for integrity only;
  * every refusal: missing/repeated options, `--sign-key` without
    `--publish`, a malformed ledger, an unreadable key and a missing site.

Run: converter_publish_test.py /path/to/plusmc-convert /path/to/plusmc-mod
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import converter_test  # noqa: E402

TIMEOUT = 60
ARTIFACTS = ["SHA256SUMS", "history.tsv", "index.html", "index.json", "verified.md"]


def run(cli, args, expect=0):
    proc = subprocess.run(
        [str(cli), *[str(arg) for arg in args]],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"{Path(cli).name} exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-6000:]}\nstderr:\n{proc.stderr[-6000:]}"
        )
    return proc


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ledger_rows(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        rows.append(line.split("\t"))
    return rows


def sums_entries(path):
    entries = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        digest, name = line.split("  ", 1)
        entries[name] = digest
    return entries


def portable_class(name="com/example/Portable"):
    return converter_test.class_bytes(
        name,
        super_name="org/bukkit/plugin/java/JavaPlugin",
        interfaces=["org/bukkit/event/Listener", "org/bukkit/command/CommandExecutor"],
    )


def partial_class():
    return converter_test.class_bytes(
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


def history_of(index):
    assert "history" in index, index
    return index["history"]


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: converter_publish_test.py <plusmc-convert> <plusmc-mod>")
    cli = Path(sys.argv[1]).resolve()
    mod_tool = Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-converter-publish-") as temporary:
        root = Path(temporary)
        plugins = root / "plugins"
        (plugins / "nested").mkdir(parents=True)
        converter_test.make_jar(
            plugins / "alpha.jar",
            {"com/example/Portable.class": portable_class()},
            converter_test.MANIFEST,
        )
        converter_test.make_jar(
            plugins / "nested" / "beta.jar",
            {"com/example/Partial.class": partial_class()},
            converter_test.MANIFEST,
        )
        converter_test.make_jar(
            plugins / "plain.jar",
            {"com/example/Plain.class": converter_test.class_bytes("com/example/Plain")},
        )
        (plugins / "broken.jar").write_bytes(b"this is not a zip archive")

        site = root / "site"
        first = run(cli, ["catalog", plugins, "--publish", site])
        assert "history: run 1 appended" in first.stdout, first.stdout
        assert "4 added, 0 removed, 0 changed, 0 unchanged" in first.stdout, first.stdout
        for name in ARTIFACTS:
            assert (site / name).is_file(), f"missing {name}"
        assert "signed: no" in first.stdout, first.stdout
        print("PASS publish writes the site")

        index = json.loads((site / "index.json").read_text(encoding="utf-8"))
        first_history = history_of(index)
        assert first_history["run"] == 1, first_history
        assert first_history["file"].endswith("site/history.tsv"), first_history
        assert [entry["file"] for entry in first_history["added"]] == [
            "alpha.jar",
            "broken.jar",
            "nested/beta.jar",
            "plain.jar",
        ], first_history
        assert first_history["removed"] == [] and first_history["changed"] == [], first_history
        assert first_history["unchanged"] == 0, first_history

        rows = ledger_rows(site / "history.tsv")
        assert len(rows) == 4, rows
        assert {row[0] for row in rows} == {"1"}, rows
        assert {row[4] for row in rows} == {"portable", "partial", "refused", "unreadable"}, rows
        assert all(len(row) == 9 for row in rows), rows
        assert (site / "history.tsv").read_text(encoding="utf-8").startswith(
            "# +MC Verified catalog history (schema 1)"
        )
        scores = {row[3]: (row[4], row[5], row[6]) for row in rows}
        assert scores["alpha.jar"] == ("portable", "green", "1.000"), scores
        assert scores["plain.jar"][0] == "refused", scores
        assert scores["broken.jar"][:2] == ("unreadable", "-"), scores
        print("PASS history run 1")

        html = (site / "index.html").read_text(encoding="utf-8")
        assert html.startswith("<!doctype html>"), html[:80]
        assert "<td>4 (1 portable, 1 partial, 1 refused, 1 unreadable)</td>" in html, html
        assert "<td><code>alpha.jar</code></td>" in html, html
        assert "<td>Example 1.2.3</td>" in html, html
        assert "Not a portability proof" in html, html
        assert "<script" not in html.lower(), html
        print("PASS static page")

        sums = sums_entries(site / "SHA256SUMS")
        assert sorted(sums) == ["history.tsv", "index.html", "index.json", "verified.md"], sums
        for name, digest in sums.items():
            assert sha256(site / name) == digest, f"{name} does not match SHA256SUMS"
        print("PASS SHA256SUMS")

        # Re-publishing an unchanged scan adds no run and changes no byte. The
        # index reproduces run 1's delta rather than saying "nothing changed".
        before = {name: (site / name).read_bytes() for name in ARTIFACTS}
        again = run(cli, ["catalog", plugins, "--publish", site])
        assert "already describes this scan" in again.stdout, again.stdout
        after = {name: (site / name).read_bytes() for name in ARTIFACTS}
        for name in ARTIFACTS:
            assert before[name] == after[name], f"{name} changed on an unchanged re-publish"
        assert len(ledger_rows(site / "history.tsv")) == 4
        assert history_of(json.loads((site / "index.json").read_text()))["run"] == 1
        print("PASS re-publish is a no-op")

        # Run 2: one jar appears and one is gone.
        converter_test.make_jar(
            plugins / "gamma.jar",
            {"com/example/Gamma.class": portable_class("com/example/Gamma")},
            converter_test.MANIFEST,
        )
        (plugins / "broken.jar").unlink()
        second = run(cli, ["catalog", plugins, "--publish", site])
        assert "history: run 2 appended" in second.stdout, second.stdout
        assert "1 added, 1 removed, 0 changed, 3 unchanged" in second.stdout, second.stdout
        assert "+ gamma.jar  portable  1.000" in second.stdout, second.stdout
        assert "- broken.jar" in second.stdout, second.stdout
        run2 = history_of(json.loads((site / "index.json").read_text()))
        assert run2["run"] == 2, run2
        assert [entry["file"] for entry in run2["added"]] == ["gamma.jar"], run2
        assert [entry["file"] for entry in run2["removed"]] == ["broken.jar"], run2
        assert run2["changed"] == [] and run2["unchanged"] == 3, run2
        rows = ledger_rows(site / "history.tsv")
        assert len(rows) == 8, rows
        assert {row[0] for row in rows} == {"1", "2"}, rows
        print("PASS history run 2 (added, removed)")

        # Run 3: beta moves from partial to portable in place.
        converter_test.make_jar(
            plugins / "nested" / "beta.jar",
            {"com/example/Partial.class": portable_class("com/example/Partial")},
            converter_test.MANIFEST,
        )
        third = run(cli, ["catalog", plugins, "--publish", site])
        assert "history: run 3 appended" in third.stdout, third.stdout
        assert "0 added, 0 removed, 1 changed, 3 unchanged" in third.stdout, third.stdout
        assert "~ nested/beta.jar  partial -> portable" in third.stdout, third.stdout
        run3 = history_of(json.loads((site / "index.json").read_text()))
        assert run3["run"] == 3, run3
        assert len(run3["changed"]) == 1, run3
        change = run3["changed"][0]
        assert change["file"] == "nested/beta.jar", change
        assert (change["from"], change["to"]) == ("partial", "portable"), change
        assert change["from_coverage"] != change["to_coverage"], change
        assert change["to_coverage"] == "1.000", change
        assert len(ledger_rows(site / "history.tsv")) == 12
        print("PASS history run 3 (changed)")

        # An explicit ledger elsewhere is created with its parents and starts
        # its own run numbering; the text report names the file it wrote.
        elsewhere = root / "logs" / "history.tsv"
        explicit = run(cli, ["catalog", plugins, "--history", elsewhere])
        assert "history: run 1 appended" in explicit.stdout, explicit.stdout
        assert elsewhere.as_posix() in explicit.stdout.replace("\\", "/"), explicit.stdout
        assert len(ledger_rows(elsewhere)) == 4, ledger_rows(elsewhere)
        assert {row[0] for row in ledger_rows(elsewhere)} == {"1"}, ledger_rows(elsewhere)
        print("PASS explicit --history file")

        # The review signature. `plusmc-mod keygen` owns the Ed25519 key format
        # the module packer and the site share.
        private = root / "catalog.pem"
        public = root / "catalog.pub.pem"
        run(mod_tool, ["keygen", "--private", private, "--public", public])
        assert private.is_file() and public.is_file()

        signed = root / "signed"
        result = run(cli, ["catalog", plugins, "--publish", signed, "--sign-key", private])
        assert "signed: yes" in result.stdout, result.stdout
        assert (signed / "verified.sig").is_file(), "no signature"
        assert (signed / "verified.pub.pem").is_file(), "no public key"
        assert (signed / "verified.pub.pem").read_bytes() == public.read_bytes(), (
            "the published public key differs from the keygen output"
        )
        checked = run(cli, ["verify-site", signed, "--key", public])
        assert "files: 4 checked" in checked.stdout, checked.stdout
        assert "signature: verified" in checked.stdout, checked.stdout
        # The site's own copy of the key is enough to check it.
        run(cli, ["verify-site", signed, "--key", signed / "verified.pub.pem"])
        unsigned = run(cli, ["verify-site", site])
        assert "files: 4 checked" in unsigned.stdout, unsigned.stdout
        assert "signature: not checked" in unsigned.stdout, unsigned.stdout
        print("PASS signed publish and verify-site")

        machine = json.loads(run(cli, ["verify-site", signed, "--json"]).stdout)
        assert machine["mode"] == "verify-site", machine
        assert machine["ok"] is True and machine["checked"] == 4, machine
        assert machine["signature"] is False, machine
        print("PASS verify-site JSON")

        # A tampered artifact fails on its checksum...
        original = (signed / "index.json").read_bytes()
        (signed / "index.json").write_bytes(original + b" ")
        proc = run(cli, ["verify-site", signed], expect=1)
        assert "index.json does not match its SHA256SUMS entry" in proc.stderr, proc.stderr
        (signed / "index.json").write_bytes(original)

        # ...and a tampered artifact whose sums were also rewritten fails on
        # the signature, because the sums are what the signature covers.
        markdown = (signed / "verified.md").read_bytes()
        (signed / "verified.md").write_bytes(markdown + b"\nforged\n")
        sums = sums_entries(signed / "SHA256SUMS")
        sums["verified.md"] = sha256(signed / "verified.md")
        (signed / "SHA256SUMS").write_text(
            "".join(f"{digest}  {name}\n" for name, digest in sorted(sums.items())),
            encoding="utf-8",
        )
        proc = run(cli, ["verify-site", signed, "--key", public], expect=1)
        assert "does not sign SHA256SUMS" in proc.stderr, proc.stderr

        # Restoring the site restores the check.
        run(cli, ["catalog", plugins, "--publish", signed, "--sign-key", private])
        run(cli, ["verify-site", signed, "--key", public])
        print("PASS tamper detection")
        print("PASS restoring a republished site")

        proc = run(cli, ["catalog", plugins, "--publish"], expect=2)
        assert "--publish needs a directory" in proc.stderr, proc.stderr
        proc = run(
            cli,
            ["catalog", plugins, "--publish", root / "a", "--publish", root / "b"],
            expect=2,
        )
        assert "--publish given more than once" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins, "--sign-key", private], expect=2)
        assert "--sign-key needs --publish" in proc.stderr, proc.stderr
        proc = run(cli, ["catalog", plugins, "--history"], expect=2)
        assert "--history needs a file" in proc.stderr, proc.stderr
        proc = run(
            cli,
            ["catalog", plugins, "--history", root / "a.tsv", "--history", root / "b.tsv"],
            expect=2,
        )
        assert "--history given more than once" in proc.stderr, proc.stderr
        # --top now caps the published page as well as the Markdown report.
        proc = run(cli, ["catalog", plugins, "--publish", root / "capped", "--top", "2"])
        assert "Showing the first 2 of 4 entries" in (root / "capped" / "index.html").read_text(
            encoding="utf-8"
        )
        assert proc.returncode == 0
        print("PASS publish option refusals")

        broken_ledger = root / "broken.tsv"
        broken_ledger.write_text(
            "1\tnot-a-digest\t/srv\talpha.jar\tportable\tgreen\t1.000\tExample\t1.0\n",
            encoding="utf-8",
        )
        proc = run(cli, ["catalog", plugins, "--history", broken_ledger], expect=1)
        assert "broken.tsv:1" in proc.stderr, proc.stderr
        assert "digest column" in proc.stderr, proc.stderr
        assert broken_ledger.read_text(encoding="utf-8").startswith("1\t"), "the ledger was rewritten"
        bad_key = root / "bad.pem"
        bad_key.write_text("not a key\n", encoding="utf-8")
        proc = run(
            cli,
            ["catalog", plugins, "--publish", root / "nokey", "--sign-key", bad_key],
            expect=1,
        )
        assert "cannot parse signing key" in proc.stderr, proc.stderr
        assert not (root / "nokey" / "index.html").exists(), (
            "an unreadable key published a site"
        )
        print("PASS ledger and key refusals")

        missing = run(cli, ["verify-site", root / "absent"], expect=1)
        assert "not a directory" in missing.stderr, missing.stderr
        partial_site = root / "partial-site"
        partial_site.mkdir()
        proc = run(cli, ["verify-site", partial_site], expect=1)
        assert "SHA256SUMS" in proc.stderr, proc.stderr
        proc = run(cli, ["verify-site"], expect=2)
        assert "usage:" in proc.stdout, proc.stdout
        proc = run(cli, ["verify-site", signed, "--key"], expect=2)
        assert "--key needs a public key file" in proc.stderr, proc.stderr
        proc = run(cli, ["verify-site", signed, "--key", public, "--key", public], expect=2)
        assert "--key given more than once" in proc.stderr, proc.stderr
        print("PASS verify-site refusals")

        # A failed scan publishes nothing, so a broken directory cannot replace
        # a good site with an error page.
        failed = root / "failed"
        proc = run(cli, ["catalog", root / "absent", "--publish", failed], expect=1)
        assert "not a directory" in proc.stderr, proc.stderr
        assert not failed.exists(), "a failed scan created the site directory"
        print("PASS failed scan publishes nothing")

    print("converter-publish: all checks passed")


if __name__ == "__main__":
    main()
