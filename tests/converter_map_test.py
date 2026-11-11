#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The +MC Converter mapper (Phase 7, slice 2).

The mapper takes the analyzer's references and gives each one a native path or
an explicit gap. This test builds the same kind of real class files as
`converter_test.py` (imported, so the two cannot drift) and pins:

  * mapped, partial and gap entries in one report, with the table's own note;
  * the class and member counts and the coverage number;
  * a field reference and a non-API (bundled/JDK) reference;
  * an unknown Bukkit class as a gap, an NMS class as a red gap;
  * the JSON summary and per-entry statuses;
  * an unreadable archive refused with exit 1.

Run: converter_map_test.py /path/to/plusmc-convert
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
            f"stdout:\n{proc.stdout[-4000:]}\nstderr:\n{proc.stderr[-4000:]}"
        )
    return proc


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: converter_map_test.py <plusmc-convert>")
    cli = Path(sys.argv[1]).resolve()

    with tempfile.TemporaryDirectory(prefix="plusmc-converter-map-") as temporary:
        root = Path(temporary)

        plugin = converter_test.class_bytes(
            "com/example/Plugin",
            super_name="org/bukkit/plugin/java/JavaPlugin",
            interfaces=["org/bukkit/event/Listener", "org/bukkit/command/CommandExecutor"],
            classes=[
                "org/bukkit/configuration/ConfigurationSection",
                "org/bukkit/inventory/Inventory",
            ],
            methods=[
                ("org/bukkit/plugin/java/JavaPlugin", "getDataFolder", "()Ljava/io/File;"),
                ("org/bukkit/plugin/java/JavaPlugin", "saveDefaultConfig", "()V"),
                (
                    "org/bukkit/command/CommandExecutor",
                    "onCommand",
                    "(Lorg/bukkit/command/CommandSender;Lorg/bukkit/command/Command;"
                    "Ljava/lang/String;[Ljava/lang/String;)Z",
                ),
                (
                    "org/bukkit/configuration/ConfigurationSection",
                    "getString",
                    "(Ljava/lang/String;)Ljava/lang/String;",
                ),
                ("org/bukkit/scheduler/BukkitRunnable", "run", "()V"),
                (
                    "org/bukkit/command/CommandSender",
                    "sendMessage",
                    "(Ljava/lang/String;)V",
                ),
            ],
            fields=[("org/bukkit/entity/Player", "someField", "I")],
        )
        jar = converter_test.make_jar(
            root / "green.jar", {"com/example/Plugin.class": plugin}, converter_test.MANIFEST
        )
        text = run(cli, ["map", jar]).stdout
        assert "classes: 8 (3 mapped, 5 partial, 0 gap)" in text, text
        assert "members: 7 (4 mapped, 1 partial, 2 gap)" in text, text
        assert "coverage: 86% (13 of 15 API references" in text, text
        assert "mapped  org/bukkit/plugin/java/JavaPlugin -> pmc_plugin lifecycle" in text, text
        assert (
            "partial  org/bukkit/command/CommandExecutor#onCommand("
            "Lorg/bukkit/command/CommandSender;Lorg/bukkit/command/Command;"
            "Ljava/lang/String;[Ljava/lang/String;)Z -> pmc_command_fn" in text
        ), text
        assert "gap  org/bukkit/plugin/java/JavaPlugin#saveDefaultConfig()V -> -" in text, text
        assert "org/bukkit/entity/Player#someField:I (field) -> -" in text, text
        assert "org/bukkit/inventory/Inventory -> pmc_host_api::inventory_get" in text, text
        assert "non-API reference(s) ignored" in text, text
        print("PASS mapped, partial and gap entries")

        report = json.loads(run(cli, ["map", jar, "--json"]).stdout)
        assert report["ok"] is True, report
        assert report["mode"] == "map", report
        # The fixture uses wider API (Inventory), so the analyzer's yellow score
        # travels with the mapping rather than being recomputed here.
        assert report["score"] == "yellow", report
        summary = report["summary"]
        assert summary["classes"] == 8, summary
        assert summary["members"] == 7, summary
        assert summary["mapped"] == 7, summary
        assert summary["partial"] == 6, summary
        assert summary["gap"] == 2, summary
        assert 0.86 <= summary["coverage"] <= 0.87, summary
        inventory = next(
            item for item in report["classes"] if item["reference"] == "org/bukkit/inventory/Inventory"
        )
        assert inventory["status"] == "partial", inventory
        assert inventory["native"].startswith("pmc_host_api::inventory_get"), inventory
        field = next(item for item in report["members"] if item["field"])
        assert field["owner"] == "org/bukkit/entity/Player", field
        assert field["status"] == "gap", field
        assert field["descriptor"] == "I", field
        print("PASS JSON report")

        # Two overloads of one method name: the descriptor is part of the key,
        # so the array form cannot inherit the single-line form's mapped status.
        overloaded = converter_test.class_bytes(
            "com/example/Overloaded",
            super_name="org/bukkit/plugin/java/JavaPlugin",
            methods=[
                ("org/bukkit/command/CommandSender", "sendMessage", "(Ljava/lang/String;)V"),
                ("org/bukkit/command/CommandSender", "sendMessage", "([Ljava/lang/String;)V"),
            ],
        )
        overload_jar = converter_test.make_jar(
            root / "overload.jar",
            {"com/example/Overloaded.class": overloaded},
            converter_test.MANIFEST,
        )
        text = run(cli, ["map", overload_jar]).stdout
        assert (
            "mapped  org/bukkit/command/CommandSender#sendMessage(Ljava/lang/String;)V"
            " -> pmc_host_api::reply" in text
        ), text
        assert (
            "partial  org/bukkit/command/CommandSender#sendMessage([Ljava/lang/String;)V"
            " -> pmc_host_api::reply" in text
        ), text
        assert "one line per call; the ABI has no array form" in text, text
        overload_report = json.loads(run(cli, ["map", overload_jar, "--json"]).stdout)
        messages = [item for item in overload_report["members"] if item["name"] == "sendMessage"]
        assert {item["descriptor"]: item["status"] for item in messages} == {
            "(Ljava/lang/String;)V": "mapped",
            "([Ljava/lang/String;)V": "partial",
        }, messages
        print("PASS overloads split by descriptor")

        # `--fail-under` gates the report's own coverage: 86% passes, 87% fails,
        # and the report is still printed before the exit status.
        assert run(cli, ["map", jar, "--fail-under", "86"]).returncode == 0
        gated = run(cli, ["map", jar, "--fail-under=87"], expect=1)
        assert "coverage: 86%" in gated.stdout, gated.stdout
        assert "below --fail-under 87%" in gated.stderr, gated.stderr
        proc = run(cli, ["map", jar, "--fail-under", "101"], expect=2)
        assert "0 to 100" in proc.stderr, proc.stderr
        proc = run(cli, ["map", jar, "--fail-under", "abc"], expect=2)
        assert "0 to 100" in proc.stderr, proc.stderr
        proc = run(cli, ["map", jar, "--fail-under"], expect=2)
        assert "--fail-under needs a percentage" in proc.stderr, proc.stderr
        proc = run(
            cli, ["map", jar, "--fail-under", "10", "--fail-under", "20"], expect=2
        )
        assert "given more than once" in proc.stderr, proc.stderr
        proc = run(cli, ["analyze", jar, "--fail-under", "10"], expect=2)
        assert "unknown option" in proc.stderr, proc.stderr
        print("PASS --fail-under gate")

        # NMS is an explicit gap and the analyzer's own red score travels with
        # the mapping, so a catalog cannot show a green map for an NMS plugin.
        deep = converter_test.class_bytes(
            "com/example/Deep", classes=["net/minecraft/server/level/ServerLevel"]
        )
        nms = converter_test.make_jar(
            root / "nms.jar", {"com/example/Deep.class": deep}, converter_test.MANIFEST
        )
        text = run(cli, ["map", nms]).stdout
        assert "score: red" in text, text
        assert "gap  net/minecraft/server/level/ServerLevel -> -" in text, text
        assert "server-internal (NMS/CraftBukkit): not exposed by ABI 1" in text, text
        print("PASS NMS is an explicit gap")

        unknown = converter_test.class_bytes(
            "com/example/Future", classes=["org/bukkit/SomeFutureApi"]
        )
        future = converter_test.make_jar(
            root / "future.jar",
            {"com/example/Future.class": unknown},
            converter_test.MANIFEST,
        )
        text = run(cli, ["map", future]).stdout
        assert "org/bukkit/SomeFutureApi -> -" in text, text
        assert "no mapping entry for this API class in slice 1" in text, text
        print("PASS unknown API class is a gap")

        missing = run(cli, ["map", root / "missing.jar"], expect=1)
        assert missing.stderr.strip(), missing.stderr
        print("PASS unreadable archive")

    print("converter-map: all checks passed")


if __name__ == "__main__":
    main()
