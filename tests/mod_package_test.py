#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The `.pmcmod` preflight, pack, extract and server-load gate.

The test builds real archives and signs them with `plusmc-mod keygen` / `sign`,
then proves the verifier accepts a matching host module and refuses the cases
that matter before a native module is ever loaded. It also packs a staging
directory, checks deterministic bytes, extracts it, and boots the real server
with `--mods` so the packaged module is loaded through the same gate.

Run: mod_package_test.py /path/to/plusmc-mod /path/to/greeter.dylib /path/to/plusmc
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402

TIMEOUT = 30


def run(args, expect=0):
    proc = subprocess.run(
        [str(arg) for arg in args],
        text=True,
        capture_output=True,
        timeout=TIMEOUT,
    )
    if expect is not None and proc.returncode != expect:
        raise AssertionError(
            f"command exited {proc.returncode}, expected {expect}: {proc.args}\n"
            f"stdout:\n{proc.stdout[-3000:]}\nstderr:\n{proc.stderr[-3000:]}"
        )
    return proc


def host_target(cli):
    return run([cli, "host"]).stdout.strip()


def make_key(cli, root, name):
    private = root / f"{name}.pem"
    public = root / f"{name}.pub.pem"
    run([cli, "keygen", "--private", private, "--public", public])
    return private, public


def manifest_for(host, module_path, module_bytes, **overrides):
    manifest = {
        "schema": 1,
        "id": "com.example.modtest",
        "name": "Mod test",
        "version": "1.0.0",
        "server": {
            "api": ">=1 <2",
            "engine": ">=0.1.0 <1",
            "targets": {
                host: {
                    "module": module_path,
                    "sha256": hashlib.sha256(module_bytes).hexdigest(),
                    "size": len(module_bytes),
                }
            },
        },
    }
    manifest.update(overrides)
    return manifest


def make_package(
    cli,
    root,
    *,
    manifest,
    module_path,
    module_bytes,
    private_key,
    signature=None,
    extra_entries=None,
    compression=zipfile.ZIP_STORED,
):
    manifest_bytes = json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()
    manifest_file = root / "mod.json"
    manifest_file.write_bytes(manifest_bytes)
    if signature is None:
        signature_file = root / "mod.sig"
        run([cli, "sign", "--key", private_key, "--in", manifest_file, "--out", signature_file])
        signature = signature_file.read_bytes()
    package = root / "test.pmcmod"
    with zipfile.ZipFile(package, "w", compression) as archive:
        archive.writestr("mod.json", manifest_bytes)
        archive.writestr("mod.sig", signature)
        archive.writestr(
            "+mc-plugin.toml",
            'name = "modtest"\nversion = "1.0.0"\napi = 1\nlibrary = "greeter"\n',
        )
        archive.writestr(module_path, module_bytes)
        for name, data in (extra_entries or {}).items():
            archive.writestr(name, data)
    return package


def verify(cli, package, *extra, expect=0):
    return run([cli, "verify", package, *extra], expect=expect)


def native_header(target):
    """A minimal binary with the format and architecture `target` promises.

    The gate reads only the header (docs/MOD_DISTRIBUTION.md §3 step 4), so a
    few dozen bytes are enough to exercise every promised target on every
    runner -- including the ones this machine cannot build for.
    """
    os_name, arch = target.split("-", 1)
    if os_name == "linux":
        header = bytearray(64)
        header[0:4] = b"\x7fELF"
        header[4] = 2  # 64-bit
        header[5] = 1  # little-endian
        struct.pack_into("<H", header, 18, 183 if arch == "aarch64" else 62)
        return bytes(header)
    if os_name == "macos":
        header = bytearray(32)
        header[0:4] = b"\xcf\xfa\xed\xfe"
        struct.pack_into("<I", header, 4, 0x0100000C if arch == "aarch64" else 0x01000007)
        return bytes(header)
    header = bytearray(96)
    header[0:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 64)
    header[64:68] = b"PE\0\0"
    struct.pack_into("<H", header, 68, 0xAA64 if arch == "aarch64" else 0x8664)
    struct.pack_into("<H", header, 88, 0x20B)  # PE32+
    return bytes(header)


def oversized_entry(root, package):
    """The same archive with one entry's declared size over the per-entry cap.

    A zip bomb is refused from the central directory, before anything is
    inflated, so the file stays tiny and the bytes behind the claim are never
    read. Patching the size here is what makes the claim hostile rather than
    the writer truthful.
    """
    data = bytearray(package.read_bytes())
    central = data.find(b"PK\x01\x02")
    assert central >= 0, "the test package has no central directory"
    struct.pack_into("<I", data, central + 24, 64 * 1024 * 1024 + 1)
    output = root / "bomb.pmcmod"
    output.write_bytes(bytes(data))
    return output


def wait_for_log(path, needle, timeout=TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            text = ""
        if needle in text:
            return text
        time.sleep(0.05)
    raise AssertionError(f"'{needle}' never reached the server log: {path.read_text(errors='replace')[-2000:]}")


def start_server(binary, mods_dir, cache, public_key, log_path):
    env = wire_client.server_env(None)
    handle = open(log_path, "wb")
    try:
        process = wire_client.start_process(
            [
                binary,
                "--host",
                "127.0.0.1",
                "--port",
                "0",
                "--offline-mode",
                "--mods",
                mods_dir,
                "--mod-key",
                public_key,
                "--mods-cache",
                cache,
            ],
            stdout=handle,
            stderr=subprocess.STDOUT,
            env=env,
        )
    except BaseException:
        handle.close()
        raise
    return process, handle


def stop_server(process, handle):
    handle.flush()
    process.terminate()
    try:
        code = process.wait(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        raise AssertionError("plusmc did not exit after a console stop request")
    finally:
        handle.close()
    return code


def make_staging_package(cli, root, host, greeter, private_key):
    stage = root / "stage"
    module_dir = stage / "server" / host
    module_dir.mkdir(parents=True)
    module_name = greeter.name
    shutil.copyfile(greeter, module_dir / module_name)
    manifest = {
        "schema": 1,
        "id": "com.example.packed",
        "name": "Packed greeter",
        "version": "1.0.0",
        "server": {
            "api": ">=1 <2",
            "engine": ">=0.1.0 <1",
            "targets": {host: {"module": f"server/{host}/{module_name}"}},
        },
    }
    (stage / "mod.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (stage / "+mc-plugin.toml").write_text(
        'name = "greeter"\n'
        'version = "1.8.0"\n'
        "api = 1\n"
        f'library = "server/{host}/greeter"\n',
        encoding="utf-8",
    )
    package = root / "packed.pmcmod"
    run([cli, "pack", "--dir", stage, "--out", package, "--key", private_key])
    return package, stage


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: mod_package_test.py <plusmc-mod> <greeter-module> <plusmc>"
        )
    cli = Path(sys.argv[1]).resolve()
    greeter = Path(sys.argv[2]).resolve()
    server_binary = Path(sys.argv[3]).resolve()
    module_bytes = greeter.read_bytes()
    host = host_target(cli)
    if not host or host == "unsupported":
        raise AssertionError(f"no supported host target: {host!r}")

    with tempfile.TemporaryDirectory(prefix="plusmc-mod-test-") as temporary:
        root = Path(temporary)
        private, public = make_key(cli, root, "key")
        module_path = f"server/{host}/{greeter.name}"
        manifest = manifest_for(host, module_path, module_bytes)
        package = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
        )

        ready = verify(cli, package, "--key", public).stdout
        assert "status: ready" in ready, ready
        assert f"host: {host}" in ready, ready
        assert f"target: {host}" in ready, ready
        assert f"module: {module_path}" in ready, ready
        assert "signature: signed" in ready, ready
        print("PASS ready package")

        deflated = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
            compression=zipfile.ZIP_DEFLATED,
        )
        assert "status: ready" in verify(cli, deflated, "--key", public).stdout
        print("PASS deflated archive")

        other = "linux-x86_64" if host != "linux-x86_64" else "macos-x86_64"
        wrong_host = make_package(
            cli,
            root,
            manifest=manifest_for(other, module_path, module_bytes),
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
        )
        refused = verify(cli, wrong_host, "--key", public, expect=1).stdout
        assert "status: refused" in refused, refused
        assert "no module for host" in refused, refused
        print("PASS wrong host")

        mismatch_manifest = manifest_for(host, module_path, b"\0" * len(module_bytes))
        mismatch = make_package(
            cli,
            root,
            manifest=mismatch_manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
        )
        refused = verify(cli, mismatch, "--key", public, expect=1).stdout
        assert "SHA-256 mismatch" in refused, refused
        print("PASS hash mismatch")

        bad_binary = make_package(
            cli,
            root,
            manifest=manifest_for(host, module_path, b"not a shared library"),
            module_path=module_path,
            module_bytes=b"not a shared library",
            private_key=private,
        )
        refused = verify(cli, bad_binary, "--key", public, expect=1).stdout
        assert "module is not" in refused, refused
        print("PASS binary header")

        # G1: every promised host target is exercised with a fixture binary,
        # on every runner, including targets this machine cannot build for.
        # The check is the header against the declared target, not the file
        # name, so a renamed module fails here rather than at dlopen.
        for target in (
            "macos-aarch64",
            "macos-x86_64",
            "linux-x86_64",
            "linux-aarch64",
            "windows-x86_64",
        ):
            header = native_header(target)
            fixture = make_package(
                cli,
                root,
                manifest=manifest_for(target, module_path, header),
                module_path=module_path,
                module_bytes=header,
                private_key=private,
            )
            report = verify(cli, fixture, "--key", public, "--host", target).stdout
            assert "status: ready" in report, report
            assert f"target: {target}" in report, report
            assert f"host: {target}" in report, report
        print("PASS all five host targets")

        # The header is checked against the target, not merely against its own
        # format: a correct ELF named as Windows is refused before load.
        foreign = make_package(
            cli,
            root,
            manifest=manifest_for("windows-x86_64", module_path, native_header("linux-x86_64")),
            module_path=module_path,
            module_bytes=native_header("linux-x86_64"),
            private_key=private,
        )
        refused = verify(
            cli, foreign, "--key", public, "--host", "windows-x86_64", expect=1
        ).stdout
        assert "module is not a PE binary" in refused, refused

        wrong_arch = make_package(
            cli,
            root,
            manifest=manifest_for("macos-aarch64", module_path, native_header("macos-x86_64")),
            module_path=module_path,
            module_bytes=native_header("macos-x86_64"),
            private_key=private,
        )
        refused = verify(
            cli, wrong_arch, "--key", public, "--host", "macos-aarch64", expect=1
        ).stdout
        assert "Mach-O architecture does not match macos-aarch64" in refused, refused
        print("PASS foreign format and architecture")

        # G2: a size claim over the per-entry cap is refused from the central
        # directory, not after allocating what it claims.
        bomb = oversized_entry(root, package)
        refused = verify(cli, bomb, "--key", public, expect=1).stdout
        assert "archive entry is too large" in refused, refused
        print("PASS zip bomb refused")

        native_client = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
            extra_entries={"client/evil.dll": b"not allowed"},
        )
        refused = verify(cli, native_client, "--key", public, expect=1).stdout
        assert "native client entry" in refused, refused
        print("PASS native client entry refused")

        signed = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
        )
        bad_signature = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
            signature=b"\0" * 64,
        )
        refused = verify(cli, bad_signature, "--key", public, expect=1).stdout
        assert "signature does not match" in refused, refused
        print("PASS bad signature")

        _, other_public = make_key(cli, root, "other")
        refused = verify(cli, signed, "--key", other_public, expect=1).stdout
        assert "signature does not match" in refused, refused
        print("PASS wrong trusted key")

        unsigned = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
            signature=b"",
        )
        refused = verify(cli, unsigned, "--key", public, expect=1).stdout
        assert "mod.sig must be exactly 64 bytes" in refused, refused
        unsigned_path = root / "unsigned.pmcmod"
        with zipfile.ZipFile(signed, "r") as source, zipfile.ZipFile(
            unsigned_path, "w", zipfile.ZIP_STORED
        ) as target:
            for item in source.infolist():
                if item.filename != "mod.sig":
                    target.writestr(item, source.read(item.filename))
        refused = verify(cli, unsigned_path, "--key", public, expect=1).stdout
        assert "unsigned" in refused, refused
        accepted = verify(cli, unsigned_path, "--allow-unsigned").stdout
        assert "status: ready" in accepted, accepted
        assert "signature: unsigned" in accepted, accepted
        print("PASS unsigned policy")

        old_engine = make_package(
            cli,
            root,
            manifest=manifest_for(
                host,
                module_path,
                module_bytes,
                server={
                    "api": ">=1 <2",
                    "engine": ">=99 <100",
                    "targets": manifest["server"]["targets"],
                },
            ),
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
        )
        refused = verify(cli, old_engine, "--key", public, expect=1).stdout
        assert "engine check failed" in refused, refused
        print("PASS engine range")

        traversal = make_package(
            cli,
            root,
            manifest=manifest,
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
            extra_entries={"../escape": b"no"},
        )
        refused = verify(cli, traversal, "--key", public, expect=1).stdout
        assert "path traversal" in refused, refused
        print("PASS path traversal")

        packed, _ = make_staging_package(cli, root, host, greeter, private)
        packed_again = root / "packed-again.pmcmod"
        run(
            [
                cli,
                "pack",
                "--dir",
                root / "stage",
                "--out",
                packed_again,
                "--key",
                private,
            ]
        )
        assert packed.read_bytes() == packed_again.read_bytes(), "pack is not deterministic"
        ready = verify(cli, packed, "--key", public).stdout
        assert "status: ready" in ready, ready
        print("PASS deterministic pack")

        extracted = root / "extracted"
        result = run([cli, "extract", packed, "--out", extracted, "--key", public]).stdout
        assert "status: extracted" in result, result
        assert (extracted / "+mc-plugin.toml").is_file()
        assert (extracted / module_path).read_bytes() == module_bytes
        print("PASS verified extract")

        # The real server path: --mods verifies/extracts, then the existing
        # plugin host loads the packaged module.
        mods_dir = root / "mods"
        mods_dir.mkdir()
        shutil.copyfile(packed, mods_dir / packed.name)
        cache = root / "cache"
        log_path = root / "server.log"
        process, handle = start_server(server_binary, mods_dir, cache, public, log_path)
        try:
            wait_for_log(log_path, "mods: verified")
            wait_for_log(log_path, "plugins: loaded greeter 1.8.0")
            wait_for_log(log_path, "plugins: enabled greeter")
        except BaseException:
            handle.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            raise
        code = stop_server(process, handle)
        log = log_path.read_text(encoding="utf-8", errors="replace")
        assert code == 0, f"server exited {code} after loading a package:\n{log[-3000:]}"
        assert "plugins: unloaded greeter" in log, log[-3000:]
        cache_entries = list(cache.iterdir())
        assert len(cache_entries) == 1, cache_entries
        assert (cache_entries[0] / ".plusmc-verified").is_file()
        print("PASS server --mods load")

        # G1's server half: a refused package is a package-level failure. The
        # same boot must still verify the good one, load its plugin and reach
        # the listener, with the refusal's reason in the log.
        mixed_dir = root / "mods-mixed"
        mixed_dir.mkdir()
        shutil.copyfile(packed, mixed_dir / packed.name)
        other_target = "linux-aarch64" if host != "linux-aarch64" else "windows-x86_64"
        refused_package = make_package(
            cli,
            root,
            manifest=manifest_for(other_target, module_path, module_bytes),
            module_path=module_path,
            module_bytes=module_bytes,
            private_key=private,
        )
        shutil.copyfile(refused_package, mixed_dir / "bad.pmcmod")
        mixed_log_path = root / "mixed.log"
        process, handle = start_server(server_binary, mixed_dir, cache, public, mixed_log_path)
        try:
            wait_for_log(mixed_log_path, "mods: 1 ready, 1 refused")
            wait_for_log(mixed_log_path, "no module for host")
            wait_for_log(mixed_log_path, "plugins: loaded greeter 1.8.0")
            wait_for_log(mixed_log_path, "listening on")
        except BaseException:
            handle.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            raise
        code = stop_server(process, handle)
        mixed_log = mixed_log_path.read_text(encoding="utf-8", errors="replace")
        assert code == 0, f"a refused package stopped the server (rc {code}):\n{mixed_log[-3000:]}"
        assert "plugins: enabled greeter" in mixed_log, mixed_log[-3000:]
        print("PASS refused package does not stop the server")

    print("mod-package: all checks passed")


if __name__ == "__main__":
    main()
