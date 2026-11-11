#!/usr/bin/env python3
"""Phase 5 (ops) — `--validate-config`: the dry run a host runs before a restart.

`docs/PHASE5.md` §6 item 8 lists "config validation" as one half of what remained
of the ops item (the other is the metrics export, `tests/metrics_file_test.py`).
The value of the flag is that it answers the startup path's own questions —
server.properties ranges, the EULA gate, the world directory, the restore
target, the metrics file — without any of startup's *effects*: no listener, no
world opened, no `eula.txt` bootstrapped, no directory created.

That read-only property is what this test checks alongside the verdicts, because
it is the one a host relies on when it validates a live box: a validator that
creates the eula.txt it is complaining about would silently change the server it
was asked to inspect.

What it walks:

  A. **the good config.** A complete server.properties + eula.txt is valid:
     exit 0, `config: ok`, and a warning alone (port 0, a world directory that
     does not exist yet, no vanilla data) never fails it. Nothing on disk is
     created by the run.
  B. **server.properties values.** Each known key outside its vanilla range is a
     *error* naming the key — the loader would otherwise substitute its default
     silently, which is the misconfiguration being caught.
  C. **the EULA gate.** A hosting config with eula unset/false is an error, and
     the validator does not write the eula.txt it wants the operator to fix.
  D. **the world and the restore drill.** `--world` at a file, `--restore-backup`
     without a world, with a name that is not a snapshot, and with a path
     instead of a name — all errors, all before anything is opened.
  E. **--metrics-file.** A missing parent directory and a path that is a
     directory are both errors.
  F. **--config-dir and channels.** A config dir that does not exist, and a
     Login Plugin Request channel that is not `namespace:path`, are errors.
  G. **exit codes.** 0 = valid (warnings allowed), 1 = validation errors, 2 =
     argv the parser refuses.

Run: config_validate_test.py /path/to/plusmc
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

STEP_TIMEOUT = 60.0
PORT = 25566


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def run(binary, args, timeout=STEP_TIMEOUT):
    """`(exit code, stderr)`. Logs go to stderr; stdout stays machine-readable."""
    proc = subprocess.run([binary, "--validate-config"] + args, capture_output=True, text=True,
                          timeout=timeout)
    return proc.returncode, proc.stderr


def validate(binary, cfg_dir, extra=(), data_root=None, world=None):
    """The common shape: a temp config dir, a temp (absent) data root, a temp
    world path that the run must not create."""
    args = ["--config-dir", cfg_dir, "--data", data_root or os.path.join(cfg_dir, "vanilla"),
            "--port", str(PORT)]
    if world:
        args += ["--world", world]
    return run(binary, args + list(extra))


def property_config(cfg_dir, body, eula=True):
    write(os.path.join(cfg_dir, "server.properties"), body)
    if eula:
        write(os.path.join(cfg_dir, "eula.txt"), "#By changing the setting below\n"
                                                  "eula=true\n")


GOOD_PROPERTIES = (
    "#Minecraft server properties\n"
    "server-port=25566\n"
    "max-players=20\n"
    "motd=A +MC server\n"
    "view-distance=8\n"
    "gamemode=survival\n"
    "online-mode=true\n"
    "white-list=false\n"
    "level-name=world\n"
    "network-compression-threshold=256\n"
    "op-permission-level=4\n"
)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    root = tempfile.mkdtemp(prefix="plusmc-validate-")
    try:
        # --- A. the good config ------------------------------------------------
        good = os.path.join(root, "good")
        property_config(good, GOOD_PROPERTIES)
        world = os.path.join(root, "world")
        before = sorted(os.listdir(good))
        code, log = validate(binary, good, extra=["--port", "0"], world=world)
        assert "config: error" not in log, f"a good config must have no errors: {log}"
        assert "config: ok" in log, f"a good config must say so: {log}"
        assert code == 0, f"warnings alone must not fail (exit {code}): {log}"
        # The dry run is read-only: it did not create the world it warned about,
        # and it changed nothing in the config directory it read.
        assert not os.path.exists(world), "the validator must not create the world directory"
        assert sorted(os.listdir(good)) == before, \
            "the validator must not touch the config directory"
        for expected in ("port 0 picks a free port", "does not exist yet", "no usable vanilla data"):
            assert expected in log, f"a warning should mention {expected!r}: {log}"
        print("[1] valid config: exit 0, warnings reported, nothing created", flush=True)

        # A configured --worldgen host: server.properties always names a level
        # (`level-name`, default "world"), and that must not be read as --world.
        # The generated world is in memory, so the disk path the properties
        # imply is not part of the configuration the validator judges.
        code, log = validate(binary, good, extra=["--worldgen", "1"])
        assert code == 0 and "config: ok" in log, \
            f"--worldgen with a hosting config must be valid (exit {code}): {log}"
        assert "mutually exclusive" not in log, log
        print("[1b] configured --worldgen: valid, level-name is not a --world", flush=True)

        # Everything defaulted is valid too — but note that a server.properties
        # *is* a hosting config, so it turns the EULA gate on: without an
        # accepted eula.txt that is an error, and the validator must not write
        # the template the real start would.
        bare = os.path.join(root, "bare")
        write(os.path.join(bare, "server.properties"), "level-name=\n")
        code, log = validate(binary, bare, world=world)
        assert code == 1 and "eula.txt is not accepted" in log, \
            f"a properties file with no accepted eula is refused (exit {code}): {log}"
        assert not os.path.exists(os.path.join(bare, "eula.txt")), \
            "the validator must not write the template eula.txt it asks for"
        # With no hosting files at all the gate is inactive: the bare dev run is
        # valid, and an empty level-name is only worth a warning once there is a
        # file to say it in.
        dev = os.path.join(root, "dev")
        os.makedirs(dev, exist_ok=True)
        code, log = validate(binary, dev, world=world)
        assert code == 0 and "config: ok" in log, f"the bare dev run is valid (exit {code}): {log}"
        with_eula = os.path.join(root, "bare-eula")
        property_config(with_eula, "level-name=\n")
        code, log = validate(binary, with_eula, world=world)
        assert code == 0, f"a defaulted, EULA-accepted config is valid (exit {code}): {log}"
        assert "level-name is empty" in log, f"an empty level-name is worth a warning: {log}"
        print("[2] defaults: EULA gate on with properties present, off without; warnings pass",
              flush=True)

        # --- B. server.properties values --------------------------------------
        bad_values = [("server-port", "25566", "server-port=abc"),
                      ("view-distance", "99", "view-distance=99"),
                      ("max-players", "-5", "max-players=-5"),
                      ("online-mode", "maybe", "online-mode=maybe"),
                      ("gamemode", "banana", "gamemode=banana"),
                      ("network-compression-threshold", "99999999",
                       "network-compression-threshold=99999999"),
                      ("op-permission-level", "9", "op-permission-level=9")]
        for key, value, line in bad_values:
            cfg_dir = os.path.join(root, f"bad-{key}")
            body = "".join(f"{l}\n" for l in GOOD_PROPERTIES.strip().splitlines()
                           if not l.startswith(f"{key}="))
            property_config(cfg_dir, body + line + "\n")
            code, log = run(binary, ["--config-dir", cfg_dir,
                                     "--data", os.path.join(cfg_dir, "vanilla"),
                                     "--port", str(PORT)])
            assert code == 1, f"{key}={value} must fail validation (exit {code}): {log}"
            assert f"config: error: server.properties: {key}=" in log, \
                f"the error must name {key}: {log}"
        print(f"[3] {len(bad_values)} out-of-range/malformed properties keys: errors naming "
              f"each key", flush=True)

        # --- C. the EULA gate --------------------------------------------------
        for label, eula_text in (("missing", None), ("rejected", "eula=false\n")):
            cfg_dir = os.path.join(root, f"eula-{label}")
            property_config(cfg_dir, GOOD_PROPERTIES, eula=False)
            if eula_text is not None:
                write(os.path.join(cfg_dir, "eula.txt"), eula_text)
            code, log = validate(binary, cfg_dir)
            assert code == 1, f"eula {label} must be refused (exit {code}): {log}"
            assert "eula.txt is not accepted" in log, f"the refusal must say why: {log}"
            if eula_text is None:
                assert not os.path.exists(os.path.join(cfg_dir, "eula.txt")), \
                    "the validator must not write the eula.txt it asks for"
        # --accept-eula is the documented override and clears the gate. (The
        # directory is renamed so the reported config path cannot be mistaken
        # for a complaint about eula.txt.)
        accepted_dir = os.path.join(root, "gate-accepted")
        property_config(accepted_dir, GOOD_PROPERTIES, eula=False)
        code, log = validate(binary, accepted_dir, extra=["--accept-eula"])
        assert code == 0, f"--accept-eula must clear the gate (exit {code}): {log}"
        assert "is not accepted" not in log, f"no eula complaint once accepted: {log}"
        print("[4] EULA: missing and eula=false refused, --accept-eula clears it", flush=True)

        # --- D. the world and the restore drill --------------------------------
        as_file = os.path.join(root, "not-a-world")
        write(as_file, "this is a file, not a directory\n")
        code, log = validate(binary, good, world=as_file)
        assert code == 1 and "--world" in log and "not a directory" in log, \
            f"a --world that is a file must be an error: {log}"

        # `level-name` in a server.properties already points the runtime at a
        # world directory, so "no world to restore into" needs a config with no
        # properties at all (an empty one).
        empty_dir = os.path.join(root, "no-properties")
        os.makedirs(empty_dir, exist_ok=True)
        code, log = run(binary, ["--config-dir", empty_dir, "--data",
                                 os.path.join(empty_dir, "vanilla"),
                                 "--restore-backup", "20260101T000000Z"])
        assert code == 1 and "--restore-backup needs a world" in log, \
            f"a restore with no world must be an error: {log}"

        code, log = run(binary, ["--config-dir", good, "--data", os.path.join(good, "vanilla"),
                                 "--world", world, "--restore-backup", "20260101T000000Z"])
        assert code == 1 and "no snapshot at" in log, \
            f"a restore of a name that is not there must be an error: {log}"

        code, log = run(binary, ["--config-dir", good, "--data", os.path.join(good, "vanilla"),
                                 "--world", world, "--restore-backup", "../escape"])
        assert code == 1 and "is a path, not a snapshot name" in log, \
            f"a restore name with a separator must be refused: {log}"
        print("[5] world/restore: a file, a missing name and a path name are errors", flush=True)

        # --- E. --metrics-file -------------------------------------------------
        code, log = run(binary, ["--config-dir", good, "--data", os.path.join(good, "vanilla"),
                                 "--metrics-file", os.path.join(root, "nope", "m.jsonl")])
        assert code == 1 and "does not exist" in log, \
            f"a metrics file in a missing directory must be an error: {log}"

        code, log = run(binary, ["--config-dir", good, "--data", os.path.join(good, "vanilla"),
                                 "--metrics-file", root])
        assert code == 1 and "is a directory" in log, \
            f"a metrics *directory* must be an error: {log}"

        accepted = os.path.join(root, "metrics.jsonl")
        code, log = validate(binary, good, extra=["--metrics-file", accepted])
        assert code == 0, f"a writable metrics path must pass (exit {code}): {log}"
        assert not os.path.exists(accepted), "the validator must not create the metrics file"
        print("[6] --metrics-file: missing parent and directory refused, a writable path passes",
              flush=True)

        # --- F. --config-dir and channels --------------------------------------
        code, log = run(binary, ["--config-dir", os.path.join(root, "missing-dir"),
                                 "--data", os.path.join(root, "vanilla")])
        assert code == 1 and "config directory" in log and "does not exist" in log, \
            f"a missing config dir must be an error: {log}"

        code, log = validate(binary, good, extra=["--login-plugin-request", "notnamespaced"])
        assert code == 1 and "not a namespaced channel" in log, \
            f"an unnamespaced channel must be an error: {log}"
        code, log = validate(binary, good, extra=["--login-plugin-request", "velocity:player_info"])
        assert code == 0, f"a namespaced channel must pass (exit {code}): {log}"
        print("[7] config dir + login-plugin channels: refusals and the accepted form", flush=True)

        # --- G. exit codes -----------------------------------------------------
        proc = subprocess.run([binary, "--validate-config", "--nonsense"],
                              capture_output=True, text=True, timeout=STEP_TIMEOUT)
        assert proc.returncode == 2, f"an unknown flag is argv-parser territory (exit " \
                                     f"{proc.returncode}): {proc.stderr}"
        assert "unknown argument" in proc.stderr, proc.stderr
        print("[8] exit codes: 0 valid, 1 refused config, 2 refused argv", flush=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\nPASS: --validate-config — verdicts, read-only, and the exit codes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
