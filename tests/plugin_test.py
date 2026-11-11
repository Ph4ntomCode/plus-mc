#!/usr/bin/env python3
"""Phase 6 — the native plugin API, end to end.

The ABI (`src/pluginapi/plugin.h`), the loader (`src/pluginapi/plugin_host.*`)
and the example plugin (`examples/greeter`) are one feature, and this test walks
the whole of it against a real server and a real client:

  A. **load and enable.** `--plugins <dir>` discovers the manifest, dlopens the
     module, checks the descriptor's ABI and name against the manifest, calls
     `on_load` and then `on_enable`, and logs each step by name.
  B. **a cancelable chat event.** A normal chat line reaches the plugin, the
     plugin answers the sender through the reply handle, and setting
     `event.cancel` drops the broadcast: the sender sees the plugin's line and
     *not* the `<Name> message` relay it would otherwise get.
  C. **a command.** `/greet Bob` is not a built-in, so it reaches the plugin's
     registered command with the argument tokens and answers `/greet`.
  D. **the accessors and the writer.** `/whereami` fills a player snapshot
     (position, observed velocity, gamemode/health) and two world reads;
     `/pillar` writes through `world_set_block`, and the client receives the
     same `block_update` packets any engine block write produces.
  D3. **the entity listing.** `/entities` visits every live entity: the sender
     is a `minecraft:player`, and a summoned sheep joins the same snapshot.
  D4. **the player inventory.** `/invset` queues a write that the sender's own
     session applies on its next tick — the client receives the menu slot with
     an advanced revision — and `/invget` then reads the stack back, including
     the clear and the unknown-item/unknown-player refusals.
  D5. **the entity writes.** `/entdmg`, `/enttp` and `/entdespawn` address a
     mob by id: the client sees the damage event, hurt animation and health,
     an authoritative position sync, and Remove Entities; the discarded entity
     leaves the listing and never moves again.
  D6. **the player half.** A player's teleport and damage are queued for that
     player's own session, which applies them and sends its client the packets;
     a player despawn is refused.
  D7. **a lethal hit.** A plugin-damaged cow's death is resolved by the world
     tick, so the client sees the death event and Remove Entities and the world
     drops its loot and experience as real entities.
  D8. **the by-id lookup.** `/entget <id>` returns one entity's snapshot —
     type, display name, position, health, age, velocity and facing — without
     walking the listing; a falling mob reports downward velocity, a player's
     yaw/pitch and one-block x delta come from its own position packets and
     `player_get` reports the same delta, a diagonal move reaches `player_get`
     on all three axes, a plugin teleport reports zero velocity, and an unknown
     id or a discarded id answers NOT_FOUND.
  D9. **the append-only snapshots.** A fixture plugin declares the pre-velocity
     `pmc_player` and `pmc_entity` (the fields through `entity_id` and `age`),
     calls `player_get` and `entity_get` with those shorter sizes, and proves
     the host fills each prefix while leaving every byte where velocity now
     starts untouched; structs that stop before that prefix are refused
     (`player_get` returns -1, `entity_get` `PMC_ENTITY_INVALID`) and left
     alone; structs larger than the host's snapshot get the host's filled size
     in `struct_size` and keep their extension bytes; every host-owned snapshot
     the `entities` visitor hands out reports the host's size; and a
     `pmc_item` that stops before `name` is refused with
     `PMC_INVENTORY_OUT_OF_RANGE`.
  D10. **the host table prefix.** A fixture built against the original
     `pmc_host_api` (through `server_version`) checks the host's advertised
     `struct_size` covers that prefix, then registers and answers a command
     using only those fields.
  E. **the scheduler.** The plugin's 20-tick repeating task runs on the tick
     loop and logs a heartbeat carrying `pmc_host_api::tick_count`.
  F. **unload.** SIGTERM fires `PMC_EVENT_SERVER_STOP`, then `on_disable` and
     `on_unload` in reverse load order, before the process exits 0.
  G. **isolation.** A plugin whose manifest asks for a different ABI is refused
     with a line naming both versions, counted as failed, and the server still
     starts and serves — one broken plugin must not take a server down.
  H. **a typo is not silence.** `--plugins` at a path that is not a directory
     fails the start (`config: error` under `--validate-config`, exit 1 at
     boot) rather than running with none of the plugins the operator expected.
  I. **a real world.** A second server serves `--worldgen 42`, so `/whereami`
     must name a generated surface block and biome rather than the void's air.
  J. **the player lifecycle.** The example answers `PMC_EVENT_PLAYER_JOIN`
     through the event's reply handle (the joining client sees its welcome) and
     checks with `player_get` that the registry entry is already live; after
     the client closes, its `PMC_EVENT_PLAYER_LEAVE` handler checks that the
     entry is already gone.
  K. **the block events.** A real `use_item_on` places stone and a real
     `player_action` breaks it: `PMC_EVENT_BLOCK_PLACE` carries the placed
     state, the previous state and the cell, and `PMC_EVENT_BLOCK_BREAK`
     carries the removed state. `/blockguard` then denies each kind in turn:
     the world keeps its block (the client is sent the existing state), the
     stone stack is not consumed, and the event's reply handle tells the
     player.
  L. **item use on a block.** A real bucket picks a water source up and a
     filled bucket puts one down; flint and steel primes a TNT block. Each
     fires `PMC_EVENT_ITEM_USE_ON` with the item, clicked cell and face.
     `/itemguard deny` vetoes the pickup, the placement and the priming: the
     fluid and TNT stay, the item is neither swapped, worn nor consumed, the
     client gets the predicted cells back, and the reply handle tells the
     player. The allowed priming leaves a real `minecraft:tnt` entity.
  M. **runtime unload and hot reload.** `/plugins reload` disables, unloads
     and opens the module again while the player stays connected: an edit to
     `config.toml` made outside the host takes effect only through the reload,
     the fresh module's commands and listeners run, and a plugin another one
     depends on is refused until the dependent is unloaded.

Run: plugin_test.py /path/to/plusmc /path/to/greeter.so /path/to/+mc-plugin.toml \
                    [/path/to/oldabi.so /path/to/oldabi.toml \
                     [/path/to/oldhost.so /path/to/oldhost.toml]]
"""
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import wire_client  # noqa: E402
from nbt_component import chat_text  # noqa: E402
from wire_client import make_frame, read_packet  # noqa: E402

HOST = "127.0.0.1"
PROTO = 777
STEP_TIMEOUT = 30.0


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            b |= 0x80
        out.append(b)
        if not n:
            return bytes(out)


def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        shift += 7
        if not (b & 0x80):
            return val, pos
    return None, pos


def rd_string(buf, pos=0):
    length, pos = rd_varint(buf, pos)
    return buf[pos:pos + length].decode("utf-8"), pos + length


class Bot:
    """The same raw client the command tests use: join, then a reader thread."""

    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(2.0)
        self.threshold = -1
        self.entity_id = 0
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pkt_id, body=b""):
        self.reader.send(pkt_id, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self):
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() +
              struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00" * 16)
        while True:
            pid, body = self.recv()
            if pid == 0x03:
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:
                self.send(0x03)
                break
            else:
                raise AssertionError(f"unexpected login 0x{pid:02x}")
        while True:
            pid, body = self.recv()
            if pid == 0x0F:
                npacks, i = rd_varint(body)
                packs = []
                for _ in range(npacks):
                    ns, i = rd_string(body, i)
                    pk, i = rd_string(body, i)
                    ver, i = rd_string(body, i)
                    packs.append((ns, pk, ver))
                out = varint(len(packs))
                for ns, pk, ver in packs:
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) + pk.encode() +
                            varint(len(ver)) + ver.encode())
                self.send(0x07, out)
            elif pid == 0x03:
                self.send(0x03)
                break
        while True:
            pid, body = self.recv()
            if pid == 0x32:
                # login: the server's entity id for this player, first field.
                self.entity_id = struct.unpack(">I", body[:4])[0]
                break
        for _ in range(400):
            pid, body = self.recv()
            if pid == 0x2D:
                self.send(0x1C, body[:8])
            elif pid == 0x0B:
                batch_size, _ = rd_varint(body)
                self.send(0x0B, varint(batch_size))
                break
        self.reader.start()

    def send_chat(self, text):
        self.send(0x09, varint(len(text)) + text.encode())


def drain(bot, timeout=2.0):
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


SB_PLAYER_ACTION = 0x29
SB_SET_CARRIED_ITEM = 0x36
SB_USE_ITEM_ON = 0x42


def use_item_on(bot, x, y, z, face, seq):
    """ServerboundUseItemOn: click `(x,y,z)` on `face`, cursor at its centre."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + wire_client.pack_block_pos(x, y, z) + varint(face) +
             struct.pack(">fff", 0.5, 0.5, 0.5) + bytes([0, 0]) + varint(seq))


def stop_dig(bot, x, y, z, seq):
    """ServerboundPlayerAction STOP_DESTROY_BLOCK, which finishes a dig."""
    bot.send(SB_PLAYER_ACTION,
             varint(2) + wire_client.pack_block_pos(x, y, z) + bytes([0]) + varint(seq))


def block_updates(packets):
    """The block_update (0x08) cells in a drain, as `(x,y,z) -> state id`."""
    out = {}
    for pid, body in packets:
        if pid != 0x08 or len(body) < 9:
            continue
        raw = struct.unpack(">Q", body[:8])[0]
        state, _ = rd_varint(body, 8)
        out[wire_client.unpack_block_pos(raw)] = state
    return out


def chats(packets):
    return [chat_text(body) for pid, body in packets if pid == 0x7C]


def wait_for_chat(bot, predicate, command=None, timeout=STEP_TIMEOUT, what="reply"):
    """Send/drain until a chat line matches `predicate`.

    A command's reply can trail its tick by more than one quiet window when the
    full suite is loaded, and `/entities` can run before a just-summoned mob is
    registered. Retrying the command is safe: these commands are idempotent,
    and the assertion is about the behavior rather than one packet's timing.
    """
    deadline = time.time() + timeout
    seen = []
    while time.time() < deadline:
        if command is not None:
            bot.send_chat(command)
        lines = chats(drain(bot))
        seen.extend(lines)
        if any(predicate(line) for line in lines):
            return seen
        time.sleep(0.1)
    raise AssertionError(f"{what} missing after {timeout:.1f}s: {seen[-12:]}")


def decode_stack(buf, pos=0):
    """[varint count] then, when non-zero, [varint item id][component patch].
    A plain stack has an empty patch; this test only writes plain stacks."""
    count, pos = rd_varint(buf, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0 and removed == 0, f"unexpected component patch {added}/{removed}"
    return (item, count), pos


def decode_set_slot(body):
    """container_set_slot (0x14): [varint container][varint state][i16 slot][stack]."""
    cid, pos = rd_varint(body, 0)
    state, pos = rd_varint(body, pos)
    assert pos + 2 <= len(body), "container_set_slot truncated before the slot"
    slot = struct.unpack(">h", body[pos:pos + 2])[0]
    stack, pos = decode_stack(body, pos + 2)
    assert pos == len(body), f"container_set_slot: {len(body) - pos} trailing byte(s)"
    return cid, state, slot, stack


def decode_spawn_eid(body):
    """Add Entity (0x01): [varint entity id][uuid]..."""
    eid, _ = rd_varint(body, 0)
    return eid


def decode_spawn_type(body):
    """Add Entity's entity-type registry id, after the id and 16-byte uuid."""
    _, pos = rd_varint(body, 0)
    kind, _ = rd_varint(body, pos + 16)
    return kind


def decode_position_sync(body):
    """entity_position_sync (0x23, 26.3): [varint id][varint PositionPath
    type = LINEAR 0][f64 x,y,z][f32 yaw][f32 pitch][u8 ground]. The old
    velocity tuple is kept as zeros for callers that unpack it."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    assert pos + 24 + 9 <= len(body), "entity_position_sync truncated"
    x, y, z = struct.unpack(">3d", body[pos:pos + 24])
    yaw, pitch = struct.unpack(">2f", body[pos + 24:pos + 32])
    ground = body[pos + 32]
    assert pos + 33 == len(body), f"entity_position_sync: {len(body) - pos - 33} trailing"
    return eid, x, y, z, 0.0, 0.0, 0.0, yaw, pitch, ground


def decode_player_position(body):
    """player_position (0x49): [varint teleport id][f64 x,y,z,vel] [f32 yaw,pitch]
    [u32 relatives]."""
    _, pos = rd_varint(body, 0)
    x, y, z, _vx, _vy, _vz = struct.unpack(">6d", body[pos:pos + 48])
    return x, y, z


def decode_entity_event(body):
    """entity_event (0x22): [u32 entity id][u8 event id]."""
    return struct.unpack(">I", body[:4])[0], body[4]


def decode_spawned_data(body):
    """One set_entity_data (0x63): [varint id][u8 index][varint serializer]
    returns (id, index, serializer, value_offset, body)."""
    eid, pos = rd_varint(body, 0)
    if pos >= len(body):
        return None
    index = body[pos]
    pos += 1
    serializer, pos = rd_varint(body, pos)
    return eid, index, serializer, pos, body


def decode_item_entity_stack(body):
    """The item stack of an item entity's DATA_ITEM (index 8, ITEM_STACK 7)."""
    parsed = decode_spawned_data(body)
    if parsed is None:
        return None
    eid, index, serializer, pos, raw = parsed
    if index != 8 or serializer != 7:
        return None
    count, pos = rd_varint(raw, pos)
    item, pos = rd_varint(raw, pos)
    added, pos = rd_varint(raw, pos)
    removed, pos = rd_varint(raw, pos)
    assert added == 0 and removed == 0, "unexpected loot component patch"
    return eid, item, count


def decode_orb_value(body):
    """The value of an experience orb's DATA_VALUE (index 8, INT 1)."""
    parsed = decode_spawned_data(body)
    if parsed is None:
        return None
    eid, index, serializer, pos, raw = parsed
    if index != 8 or serializer != 1:
        return None
    assert pos + 4 <= len(raw), "orb value word truncated"
    return eid, struct.unpack(">I", raw[pos:pos + 4])[0]


def decode_health_word(body):
    """set_entity_data (0x63) carrying health: [varint id][u8 index 9]
    [varint float serializer 3][f32 health][0xff]. None for another datum."""
    eid, pos = rd_varint(body, 0)
    if pos >= len(body) or body[pos] != 9:
        return None
    pos += 1
    serializer, pos = rd_varint(body, pos)
    assert serializer == 3, f"health should be the float serializer, got {serializer}"
    assert pos + 4 <= len(body), "health word truncated"
    health = struct.unpack(">f", body[pos:pos + 4])[0]
    return eid, health


# The fixture's /oldentget line: the pre-velocity pmc_entity prefix (id, type,
# display name, position, health, age) and whether the canary after `age`
# survived.
# The fixture's /entitysize line: the visited count and the pmc_entity
# struct_size every host-owned snapshot carried.
ENTITYSIZE_RE = re.compile(
    r'^\(entitysize\) (\d+) entit(?:y|ies), struct_size (\d+) \(all match\)$')

# The original-host-table fixture's line: the host's advertised size, the
# prefix it was compiled against, and the ABI version.
OLDHOST_RE = re.compile(
    r'^\(oldhost\) prefix ok: table (\d+) bytes, prefix (\d+) bytes, '
    r'abi 1, tick \d+, server \S.*$')

OLDENTGET_RE = re.compile(
    r'^\(oldentget\) entity (\d+): (minecraft:[a-z_]+) "([^"]*)" at '
    r'(-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+) '
    r'\(health (\d+\.\d+), age (\d+)\), (tail intact|tail written)$')

# The example's /entget line: id, type, display name, position, health, age,
# velocity (blocks/tick) and facing (degrees).
ENTGET_RE = re.compile(
    r'^\(entget\) entity (\d+): (minecraft:[a-z_]+) "([^"]*)" at '
    r'(-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+) '
    r'\(health (\d+\.\d+), age (\d+), vel (-?\d+\.\d+), (-?\d+\.\d+), '
    r'(-?\d+\.\d+), yaw (-?\d+\.\d+), pitch (-?\d+\.\d+)\)$')

# The example's /whereami line for the test's own bot: feet position, observed
# velocity (blocks/tick), gamemode/health, biome and the cells at and under the
# feet. Groups 1-3 are the position and 4-6 the velocity.
WHEREAMI_RE = re.compile(
    r'^\(whereami\) PluginBot at (-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+) '
    r'\(vel (-?\d+\.\d+), (-?\d+\.\d+), (-?\d+\.\d+); gamemode survival, '
    r'health \d+\.\d+\), biome minecraft:[a-z_]+,\s*'
    r'feet in minecraft:[a-z_]+(?:\[[^\]]*\])?, '
    r'standing on minecraft:[a-z_]+(?:\[[^\]]*\])?$')


def decode_remove(body):
    """remove_entities (0x4e): [varint count][varint id]..."""
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        ids.append(eid)
    return ids


def read_log(path):
    with open(path, "rb") as fh:
        return fh.read().decode("utf-8", "replace")


def wait_listening(log_path, proc):
    # Startup is event-driven: the listening line plus a completed connect is
    # the ready condition. There is deliberately no wall-clock ceiling; a
    # loaded full suite may take arbitrarily long to reach the bind, and a
    # genuine hang is bounded by CTest's overall test timeout instead.
    while True:
        match = re.search(r"listening on .*:(\d+)", read_log(log_path))
        if match:
            port = int(match.group(1))
            while True:
                try:
                    sock = socket.create_connection((HOST, port), timeout=1)
                    sock.close()
                    return port
                except OSError:
                    if proc.poll() is not None:
                        raise AssertionError(
                            f"the server exited before accepting connections: "
                            f"{read_log(log_path)[-800:]}")
                    time.sleep(0.02)
        if proc.poll() is not None:
            raise AssertionError(f"the server exited before listening: {read_log(log_path)[-800:]}")
        time.sleep(0.05)


def server_args(binary, data_root, extra):
    return [binary, "--host", HOST, "--port", "0", "--motd", "plugin test", "--offline-mode",
            "--gamemode", "0", "--data", data_root] + list(extra)


def start(binary, data_root, extra, log_path, env):
    with open(log_path, "wb") as log:
        proc = wire_client.start_process(server_args(binary, data_root, extra), stdout=log,
                                stderr=subprocess.STDOUT, env=wire_client.server_env(env))
    return proc, wait_listening(log_path, proc)


def stop(proc, log_path):
    proc.terminate()
    try:
        code = proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=5)
        raise AssertionError("the server did not exit on SIGTERM")
    return code, read_log(log_path)


def wait_for_log(log_path, needle, timeout=STEP_TIMEOUT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if needle in read_log(log_path):
            return
        time.sleep(0.1)
    raise AssertionError(f"'{needle}' never reached the log: {read_log(log_path)[-1200:]}")


def write_plugin(root, manifest_path, library, name=None, api=None, depends=None):
    """One plugin directory: the checked-in manifest (optionally edited) and
    the built module under the file name the host resolves."""
    directory = os.path.join(root, "plugins", name or "greeter")
    os.makedirs(directory, exist_ok=True)
    with open(manifest_path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if name is not None:
        text = re.sub(r'^name\s*=.*$', f'name = "{name}"', text, flags=re.M)
    if api is not None:
        text = re.sub(r'^api\s*=.*$', f"api = {api}", text, flags=re.M)
    if depends is not None:
        text = text.rstrip("\n") + "\ndepends = [" + ", ".join(
            json.dumps(dep) for dep in depends) + "]\n"
    with open(os.path.join(directory, "+mc-plugin.toml"), "w", encoding="utf-8") as fh:
        fh.write(text)
    shutil.copyfile(library, os.path.join(directory, os.path.basename(library)))
    return os.path.join(root, "plugins")


def run_validate(binary, plugins_dir, data_root, env):
    proc = subprocess.run([binary, "--validate-config", "--offline-mode", "--accept-eula",
                           "--plugins", plugins_dir, "--data", data_root],
                          capture_output=True, text=True, timeout=STEP_TIMEOUT, env=env)
    return proc.returncode, proc.stdout + proc.stderr


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc /path/to/greeter.so "
              "/path/to/+mc-plugin.toml", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    library = os.path.realpath(sys.argv[2])
    manifest = os.path.realpath(sys.argv[3])
    # The ABI fixtures are optional for a hand run but always given by ctest;
    # without them the D9/D10 arms are skipped, never silently passed.
    old_library = os.path.realpath(sys.argv[4]) if len(sys.argv) > 4 else None
    old_manifest = os.path.realpath(sys.argv[5]) if len(sys.argv) > 5 else None
    oldhost_library = os.path.realpath(sys.argv[6]) if len(sys.argv) > 6 else None
    oldhost_manifest = os.path.realpath(sys.argv[7]) if len(sys.argv) > 7 else None
    if ((old_library is None) != (old_manifest is None) or
            (oldhost_library is None) != (oldhost_manifest is None)):
        print(f"Usage: {sys.argv[0]} /path/to/plusmc /path/to/greeter.so "
              "/path/to/+mc-plugin.toml "
              "[/path/to/oldabi.so /path/to/oldabi.toml "
              "[/path/to/oldhost.so /path/to/oldhost.toml]]",
              file=sys.stderr)
        return 1
    data_root = os.path.join(os.path.dirname(manifest), "..", "..", "data", "vanilla")
    data_root = os.path.realpath(data_root)
    root = tempfile.mkdtemp(prefix="plusmc-plugin-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "1200"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "4000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"
    env["PLUSMC_AUTOSAVE_SECS"] = "0"

    plugins_dir = write_plugin(root, manifest, library)
    if old_library is not None:
        # The same directory: the main server loads the example and the fixture
        # together, so `/oldabi` reads the same player `/whereami` does.
        write_plugin(root, old_manifest, old_library, name="oldabi")
    if oldhost_library is not None:
        # The fixture names greeter as a dependency so the reload arm can prove
        # a dependent blocks closing the plugin it needs.
        write_plugin(root, oldhost_manifest, oldhost_library, name="oldhost",
                     depends=["greeter"])
    log_path = os.path.join(root, "plugins.log")
    bot = None
    try:
        # --- A-F. load, chat, command, accessors, scheduler, unload ----------
        proc, port = start(binary, data_root, ["--plugins", plugins_dir], log_path, env)
        try:
            wait_for_log(log_path, "plugins: loaded greeter 1.13.0 (25 commands, 9 listeners)")
            wait_for_log(log_path, "plugins: enabled greeter")
            wait_for_log(log_path, "[greeter] enabled")
            wait_for_log(log_path, "[greeter] server started on plusmc")
            print("[A] loaded, ABI-checked and enabled the example plugin", flush=True)

            bot = Bot("PluginBot", port)
            bot.join()
            print("[A] joined the server", flush=True)

            # J. the join event fires after the join burst and carries a reply
            # handle, so the example welcomes the player through it. Its
            # player_get also proves the registry entry is live by then.
            wait_for_log(log_path, "[greeter] saw join: PluginBot (player_get: live)")
            packets = drain(bot)
            lines = chats(packets)
            assert any("(greeter) welcome, PluginBot!" in line for line in lines), \
                f"the join event's welcome is missing: {lines}"
            print("[J] the plugin saw the live join and welcomed the player", flush=True)

            bot.send_chat("hello plugin")
            packets = drain(bot)
            lines = chats(packets)
            assert any("(greeter) PluginBot said: hello plugin" in line for line in lines), \
                f"the plugin's reply is missing: {lines}"
            assert not any("<PluginBot> hello plugin" in line for line in lines), \
                f"a cancelled chat line was still broadcast: {lines}"
            # The second chat listener does not cancel and must still have run:
            # a cancel is one handler's answer, not a stop-delivery signal.
            wait_for_log(log_path, "[greeter] audit saw chat from PluginBot: hello plugin")
            print("[B] chat reached the plugin, was answered and cancelled", flush=True)

            bot.send_chat("/greet Bob")
            packets = drain(bot)
            lines = chats(packets)
            assert any("Hello, Bob! (from the greeter example plugin)" in line for line in lines), \
                f"/greet reply missing: {lines}"
            print("[C] /greet reached the plugin's command with its argument", flush=True)

            # C2. the config writers: one call per type, then the getters read
            # the live values back, a key changes type, an invalid key is
            # refused, and the file the host rewrote carries every value.
            wait_for_chat(
                bot, lambda line: "(config) set greeting" in line,
                command="/config greeting saved-by-config",
                what="the greeting config write")
            wait_for_chat(
                bot, lambda line: "(config) set heartbeat_ticks" in line,
                command="/config heartbeat_ticks 5",
                what="the heartbeat config write")
            wait_for_chat(
                bot, lambda line: "(config) set debug" in line,
                command="/config debug true", what="the debug config write")

            wait_for_chat(
                bot, lambda line: '(config) greeting = "saved-by-config"' in line,
                command="/config greeting", what="the greeting config read")
            wait_for_chat(
                bot, lambda line: "(config) heartbeat_ticks = 5" in line,
                command="/config heartbeat_ticks", what="the heartbeat config read")
            wait_for_chat(
                bot, lambda line: "(config) debug = true" in line,
                command="/config debug", what="the debug config read")

            # A write replaces both the value and its type. The reader then
            # answers with the string form; the next write puts the integer back
            # for the restart assertion below.
            wait_for_chat(
                bot, lambda line: "(config) set heartbeat_ticks" in line,
                command="/config heartbeat_ticks fast",
                what="the heartbeat config type change")
            wait_for_chat(
                bot, lambda line: '(config) heartbeat_ticks = "fast"' in line,
                command="/config heartbeat_ticks",
                what="the string-typed heartbeat config read")
            wait_for_chat(
                bot, lambda line: "(config) set heartbeat_ticks" in line,
                command="/config heartbeat_ticks 5",
                what="the heartbeat config restore")

            wait_for_chat(
                bot, lambda line: "(config) refused bad/key (code -1)" in line,
                command="/config bad/key value",
                what="the invalid config-key refusal")
            print("[C2] the config writers set every type and the getters read them back",
                  flush=True)

            # D. the read-only accessors: a player snapshot plus the block and
            # biome at the player's feet. The line's shape is the assertion —
            # every field comes back filled, and the coordinates are the ones
            # the server has for the connection (the join spawn, inside the
            # world's own box).
            bot.send_chat("/whereami")
            packets = drain(bot)
            lines = chats(packets)
            where = [line for line in lines if line.startswith("(whereami) PluginBot at ")]
            assert where, f"/whereami reply missing: {lines}"
            match = WHEREAMI_RE.match(where[0])
            assert match, f"/whereami did not fill the snapshot: {where[0]}"
            y = float(match.group(2))
            assert -64 <= y < 320, f"/whereami reported a y outside the world: {y}"
            print("[D] /whereami read the player snapshot and the world", flush=True)

            bot.send_chat("/whereami PluginBot")
            packets = drain(bot)
            lines = chats(packets)
            named = [line for line in lines if line.startswith("(whereami) PluginBot at ")]
            assert named and named[0] == where[0], \
                f"the by-name lookup disagrees with the sender: {named} vs {where[0]}"
            print("[D] the by-name player lookup answers the same snapshot", flush=True)

            # D2. the write accessor: `/pillar 2` writes the cell at the sender's
            # feet and the one above it through the engine's own block-change
            # path. The reply names the position, the client receives both
            # block_update packets, and a fresh `/whereami` reads the new state
            # back — the same path a player's placement takes, not a column poke.
            bx = int(float(match.group(1)))
            by = int(float(match.group(2)))
            bz = int(float(match.group(3)))
            bot.send_chat("/pillar 2")
            packets = drain(bot)
            lines = chats(packets)
            expected = f"(pillar) placed 2 stone block(s) at {bx}, {by}, {bz}"
            assert any(line == expected for line in lines), \
                f"/pillar reply missing: {lines} (expected {expected!r})"
            updated = {}
            for pid, body in packets:
                if pid != 0x08 or len(body) < 9:
                    continue
                pos = wire_client.unpack_block_pos(struct.unpack(">q", body[:8])[0])
                state_id, _ = rd_varint(body, 8)
                updated[pos] = state_id
            assert updated.get((bx, by, bz), 0) != 0, \
                f"no block_update for the feet cell: {sorted(updated)}"
            assert updated.get((bx, by + 1, bz), 0) != 0, \
                f"no block_update for the cell above the feet: {sorted(updated)}"
            bot.send_chat("/whereami")
            packets = drain(bot)
            lines = chats(packets)
            after = [line for line in lines if line.startswith("(whereami) PluginBot at ")]
            assert after and "feet in minecraft:stone" in after[0], \
                f"/whereami did not read the plugin's write back: {after}"
            print("[D2] /pillar wrote blocks the engine broadcast and the accessors read back",
                  flush=True)

            # D11. the block events. A real placement packet and a real dig
            # packet make the session fire the plugin events; the example logs
            # the cell and both states, and `/blockguard` can deny either kind.
            # The client's predicted block and stack have to be restored, so a
            # cancelled action sends the existing block state and the slot.
            stone_id = updated[(bx, by, bz)]
            bot.send_chat("/give PluginBot minecraft:stone 4")
            lines = chats(drain(bot))
            assert any(line == "Gave 4 [minecraft:stone] to PluginBot" for line in lines), \
                f"/give did not hand out the stone: {lines}"
            bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", 0))
            drain(bot, 0.2)

            p1 = (bx + 1, by, bz)
            use_item_on(bot, bx, by, bz, 5, 401)
            packets = drain(bot)
            wait_for_log(
                log_path,
                f"[greeter] block place at ({p1[0]}, {p1[1]}, {p1[2]}): "
                "minecraft:stone (was minecraft:air)")
            updates = block_updates(packets)
            assert updates.get(p1) == stone_id, \
                f"the client did not get the placed stone at {p1}: {updates}"
            print("[K] a real placement fired PMC_EVENT_BLOCK_PLACE and changed the world",
                  flush=True)

            stop_dig(bot, *p1, 402)
            packets = drain(bot)
            wait_for_log(
                log_path,
                f"[greeter] block break at ({p1[0]}, {p1[1]}, {p1[2]}): "
                "minecraft:stone (was minecraft:stone)")
            updates = block_updates(packets)
            assert updates.get(p1) == 0, \
                f"the client did not get the broken cell back as air: {updates}"
            print("[K] a real dig fired PMC_EVENT_BLOCK_BREAK and cleared the world",
                  flush=True)

            p2 = (bx + 1, by + 1, bz)
            use_item_on(bot, bx, by + 1, bz, 5, 403)
            packets = drain(bot)
            wait_for_log(
                log_path,
                f"[greeter] block place at ({p2[0]}, {p2[1]}, {p2[2]}): "
                "minecraft:stone (was minecraft:air)")
            updates = block_updates(packets)
            assert updates.get(p2) == stone_id, \
                f"the second placement did not land at {p2}: {updates}"

            bot.send_chat("/blockguard place deny")
            lines = chats(drain(bot))
            assert any(line == "(blockguard) place=deny break=allow" for line in lines), \
                f"/blockguard did not set the place policy: {lines}"
            use_item_on(bot, bx, by, bz, 5, 404)
            packets = drain(bot)
            wait_for_log(
                log_path,
                f"[greeter] block place at ({p1[0]}, {p1[1]}, {p1[2]}): "
                "minecraft:stone (was minecraft:air) [denied]")
            lines = chats(packets)
            assert "(greeter) block place denied" in lines, \
                f"the cancelled placement did not answer the player: {lines}"
            updates = block_updates(packets)
            assert updates.get(p1) == 0, \
                f"a cancelled placement did not restore the air cell: {updates}"
            bot.send_chat("/invget 0")
            lines = chats(drain(bot))
            assert any(line.startswith("(invget) PluginBot slot 0: minecraft:stone x2 ")
                       for line in lines), \
                f"a cancelled placement consumed the stack: {lines}"
            print("[K] a denied placement kept the world and the stone stack", flush=True)

            bot.send_chat("/blockguard break deny")
            lines = chats(drain(bot))
            assert any(line == "(blockguard) place=deny break=deny" for line in lines), \
                f"/blockguard did not set the break policy: {lines}"
            stop_dig(bot, *p2, 405)
            packets = drain(bot)
            wait_for_log(
                log_path,
                f"[greeter] block break at ({p2[0]}, {p2[1]}, {p2[2]}): "
                "minecraft:stone (was minecraft:stone) [denied]")
            lines = chats(packets)
            assert "(greeter) block break denied" in lines, \
                f"the cancelled break did not answer the player: {lines}"
            updates = block_updates(packets)
            assert updates.get(p2) == stone_id, \
                f"a cancelled break did not restore the stone: {updates}"
            print("[K] a denied break kept the block", flush=True)

            bot.send_chat("/blockguard break allow")
            lines = chats(drain(bot))
            assert any(line == "(blockguard) place=deny break=allow" for line in lines), \
                f"/blockguard did not clear the break policy: {lines}"
            stop_dig(bot, *p2, 406)
            packets = drain(bot)
            wait_for_log(
                log_path,
                f"[greeter] block break at ({p2[0]}, {p2[1]}, {p2[2]}): "
                "minecraft:stone (was minecraft:stone)")
            updates = block_updates(packets)
            assert updates.get(p2) == 0, \
                f"the allowed break still left the block: {updates}"
            bot.send_chat("/blockguard place allow")
            drain(bot, 0.2)
            print("[K] the block events allow, deny and restore both actions", flush=True)

            # D3. the entity listing. The sender is a player entity in the
            # listing; a summoned sheep joins the same snapshot, so the call is
            # proven to walk more than the caller's own connection. The tally is
            # grouped by registry name, so a crowded scene still fits one line.
            # A sheep, unlike a hostile mob, has no far-despawn rule: a loaded
            # run can delay the command round trip without the fall deleting it.
            bot.send_chat("/summon minecraft:sheep")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "Summoned 1 minecraft:sheep" for line in lines), \
                f"/summon reply missing: {lines}"
            # The summon ring is at the player's feet over the void. Park the
            # sheep on the D2 pillar before listing it, so the listing sees it
            # at a stable place even if the fall has already started.
            spawn_ids = [decode_spawn_eid(body) for pid, body in packets if pid == 0x01]
            sheep_id = spawn_ids[0] if spawn_ids else None
            assert sheep_id is not None, "the D3 sheep had no Add Entity packet"
            tx, ty, tz = float(bx), float(by + 2), float(bz)
            bot.send_chat(f"/enttp {sheep_id} {tx} {ty} {tz}")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == f"(enttp) moved entity {sheep_id} to {tx:.2f}, {ty:.2f}, {tz:.2f}"
                       for line in lines), \
                f"/enttp for D3 reply missing: {lines}\n{read_log(log_path)[-1600:]}"
            lines = wait_for_chat(
                bot,
                lambda line: line.startswith("(entities) ") and "minecraft:sheep" in line,
                command="/entities", what="the summoned sheep in /entities")
            listing = [line for line in lines if line.startswith("(entities) ")]
            assert listing, f"/entities reply missing: {lines}"
            line = next(line for line in listing if "minecraft:sheep" in line)
            assert "minecraft:player x1" in line, f"the sender is not in the listing: {line}"
            assert "(players: PluginBot)" in line, f"the player name is missing: {line}"
            total = int(re.match(r"^\(entities\) (\d+) entit", line).group(1))
            assert total >= 2, f"the listing counted {total} entities: {line}"
            print(f"[D3] /entities listed the player and the summoned mob: {line}", flush=True)

            # D4. the inventory accessors. `/invset` only *queues*: the write is
            # applied by the sender's own session on its next tick, which is why
            # the following `/invget` is the packet that carries both the
            # container_set_slot and the read-back. Menu slot 36 is hotbar 0,
            # and the revision must have advanced for the client to accept it.
            bot.send_chat("/invset 0 minecraft:diamond 5")
            invset_packets = drain(bot)
            lines = chats(invset_packets)
            assert any(line == "(invset) queued slot 0 of PluginBot -> minecraft:diamond x5"
                       for line in lines), f"/invset reply missing: {lines}"
            bot.send_chat("/invget 0")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(invget) PluginBot slot 0: minecraft:diamond x5 "
                               "(damage 0, max 64)" for line in lines), \
                f"/invget did not read the queued write: {lines}"
            # The drain that applies the write is the target's own next tick,
            # which may be the /invget packet or a keep-alive response that
            # arrived first; the slot update is in one of the two windows.
            sets = [decode_set_slot(body) for pid, body in (invset_packets + packets)
                    if pid == 0x14]
            hotbar = [entry for entry in sets if entry[2] == 36]
            assert hotbar, f"no container_set_slot for menu slot 36: {sets}"
            cid, state, _, stack = hotbar[-1]
            assert cid == 0 and state >= 1, f"the write must advance the revision: {hotbar[-1]}"
            assert stack is not None and stack[1] == 5, \
                f"menu slot 36 should hold five items, got {stack}"
            print("[D4] /invset queued, the session sent the slot with a new revision, "
                  "/invget read it back", flush=True)

            # Clearing is count 0, and a cleared slot reads back as empty
            # rather than as an error. The same next-packet rule applies.
            bot.send_chat("/invset 0 minecraft:stone 0")
            clear_packets = drain(bot)
            lines = chats(clear_packets)
            assert any(line == "(invset) queued slot 0 of PluginBot to be cleared" for line in lines), \
                f"/invset clear reply missing: {lines}"
            bot.send_chat("/invget 0")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(invget) PluginBot slot 0 is empty" for line in lines), \
                f"/invget after the clear: {lines}"
            cleared = [entry for entry in
                       (decode_set_slot(body) for pid, body in (clear_packets + packets)
                        if pid == 0x14) if entry[2] == 36]
            assert cleared and cleared[-1][3] is None, \
                f"clearing the slot must send an empty stack: {cleared}"
            assert cleared[-1][1] > state, \
                f"the clear must advance the revision past {state}: {cleared[-1]}"
            print("[D4] /invset count 0 cleared the slot and /invget read it empty", flush=True)

            bot.send_chat("/invset 0 minecraft:no_such_item 1")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(invset) unknown item" for line in lines), \
                f"an unknown item must be refused: {lines}"
            bot.send_chat("/invget 0 NoSuchPlayer")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(invget) no such player" for line in lines), \
                f"an unknown player must be refused: {lines}"
            print("[D4] an unknown item and an unknown player are refused without inventing state",
                  flush=True)

            # The read is by name, not "the caller's own inventory": a second
            # session's published mirror answers the same call. (The write is
            # the sender's own slot in this example; the ABI itself accepts any
            # player name.)
            other = Bot("PluginOther", port)
            other.join()
            try:
                bot.send_chat("/invget 9 PluginOther")
                packets = drain(bot)
                lines = chats(packets)
                assert any(line == "(invget) PluginOther slot 9 is empty" for line in lines), \
                    f"the other player's inventory was not read by name: {lines}"
                print("[D4] /invget read another player's slot by name", flush=True)

                # A write to *another* player is queued for that player's
                # session: the sender gets "queued", and the target's own next
                # packet applies it and carries the slot update to its client.
                bot.send_chat("/invset 9 minecraft:emerald 3 PluginOther")
                packets = drain(bot)
                lines = chats(packets)
                assert any(line == "(invset) queued slot 9 of PluginOther -> minecraft:emerald x3"
                           for line in lines), f"/invset to another player: {lines}"
                other.send_chat("tick")  # the target's next tick drains the queue
                target_packets = drain(other)
                target_sets = [decode_set_slot(body) for pid, body in target_packets
                               if pid == 0x14]
                assert any(entry[2] == 9 and entry[3] is not None and entry[3][1] == 3
                           for entry in target_sets), \
                    f"the target did not receive its slot update: {target_sets}"
                bot.send_chat("/invget 9 PluginOther")
                packets = drain(bot)
                lines = chats(packets)
                assert any(line == "(invget) PluginOther slot 9: minecraft:emerald x3 "
                                   "(damage 0, max 64)" for line in lines), \
                    f"the cross-session write did not read back: {lines}"
                print("[D4] a write to another player was applied by that player's session",
                      flush=True)
            finally:
                other.close()

            # D5. the entity writes. Damage, teleport and despawn address a
            # freshly summoned pig by id — the D3 sheep stays parked on the
            # pillar for its listing. A pig has no far-despawn rule, so a slow
            # drain cannot delete it before these writes run. Damage sends the
            # damage event, the hurt animation and its new health; teleport
            # sends an authoritative position sync; despawn sends Remove
            # Entities and ends the tracking relationship, so the entity leaves
            # the listing and never moves again.
            bot.send_chat("/summon minecraft:pig")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "Summoned 1 minecraft:pig" for line in lines), \
                f"/summon for D5 reply missing: {lines}"
            pig_id = next((decode_spawn_eid(body) for pid, body in packets if pid == 0x01),
                          None)
            assert pig_id is not None, "the summoned pig had no Add Entity packet"
            # Put it on the pillar D2 raised, so a mob's own gravity does not
            # drop it out of the void while the rest of the arm runs.
            tx, ty, tz = float(bx), float(by + 2), float(bz)
            bot.send_chat(f"/enttp {pig_id} {tx} {ty} {tz}")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == f"(enttp) moved entity {pig_id} to {tx:.2f}, {ty:.2f}, {tz:.2f}"
                       for line in lines), f"/enttp reply missing: {lines}"
            syncs = [decode_position_sync(body) for pid, body in packets if pid == 0x23]
            moved = [sync for sync in syncs if sync[0] == pig_id]
            assert moved, f"no position sync for the pig: {[sync[0] for sync in syncs]}"
            _, sx, sy, sz, vx, vy, vz, _, _, _ = moved[-1]
            assert (abs(sx - tx), abs(sy - ty), abs(sz - tz)) == (0.0, 0.0, 0.0), \
                f"the sync carries {sx}, {sy}, {sz}, not {tx}, {ty}, {tz}"
            assert vx == vy == vz == 0.0, "a teleport must zero the velocity"
            print(f"[D5] /enttp sent the pig's position sync to {tx}, {ty}, {tz}", flush=True)

            bot.send_chat(f"/entdmg {pig_id} 5")
            packets = drain(bot)
            lines = chats(packets)
            hurt_line = [line for line in lines if line.startswith(f"(entdmg) entity {pig_id}: ")]
            assert hurt_line, f"/entdmg reply missing: {lines}"
            health_before, health_after = (float(v) for v in
                                           re.findall(r"(\d+\.\d+)", hurt_line[0])[:2])
            assert health_before > health_after > 0.0, \
                f"the hit should have moved the pig's health: {hurt_line[0]}"
            damage_events = [body for pid, body in packets if pid == 0x19]
            assert any(rd_varint(body)[0] == pig_id for body in damage_events), \
                f"no damage_event for the pig: {[(pid, len(body)) for pid, body in packets]}"
            assert any(pid == 0x2b and rd_varint(body)[0] == pig_id
                       for pid, body in packets), "no hurt_animation for the pig"
            health_words = [word for pid, body in packets if pid == 0x65
                            for word in [decode_health_word(body)] if word is not None]
            assert any(eid == pig_id and abs(value - health_after) < 0.05
                       for eid, value in health_words), \
                f"no health word with {health_after}: {health_words}"
            print(f"[D5] /entdmg landed the hit, sent the damage event, hurt animation "
                  f"and health ({health_after})", flush=True)

            bot.send_chat(f"/entdespawn {pig_id}")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == f"(entdespawn) discarded entity {pig_id}" for line in lines), \
                f"/entdespawn reply missing: {lines}"
            removed = [ids for pid, body in packets if pid == 0x4e
                       for ids in [decode_remove(body)]]
            assert any(pig_id in ids for ids in removed), \
                f"no Remove Entities for the pig: {removed}"
            bot.send_chat("/entities")
            packets = drain(bot)
            lines = chats(packets)
            listing = [line for line in lines if line.startswith("(entities) ")]
            assert listing and "minecraft:pig" not in listing[0], \
                f"the discarded pig is still listed: {listing}"
            stale = [pid for pid, body in packets
                     if pid in (0x23, 0x2b) and body and rd_varint(body)[0] == pig_id]
            assert not stale, f"the discarded pig still moved on the wire: {stale}"
            print("[D5] /entdespawn sent Remove Entities, dropped tracking and left the listing",
                  flush=True)

            bot.send_chat("/entdmg 999999 5")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(entdmg) no such entity" for line in lines), \
                f"an unknown entity must be refused: {lines}"
            bot.send_chat(f"/entdespawn {pig_id}")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(entdespawn) no such entity" for line in lines), \
                f"a second discard must be refused: {lines}"
            print("[D5] unknown and already-discarded ids are refused without inventing state",
                  flush=True)

            # D6. the player half of the writes. A player's state belongs to
            # their own session, so the plugin queues the move and the hit; the
            # target's next tick applies them through the same sinks `/tp` and
            # `/damage` use, and its client receives the position sync and the
            # damage packets. Despawning a player is refused by design.
            target = Bot("WriteTarget", port)
            target.join()
            try:
                spawns = [body for pid, body in drain(bot) if pid == 0x01]
                player_spawns = [body for body in spawns if decode_spawn_type(body) == 159]
                assert player_spawns, "the second player's Add Entity never arrived"
                player_id = decode_spawn_eid(player_spawns[-1])

                px, py, pz = float(bx) + 4.0, float(by), float(bz) + 4.0
                bot.send_chat(f"/enttp {player_id} {px} {py} {pz}")
                packets = drain(bot)
                lines = chats(packets)
                assert any(line.startswith(f"(enttp) queued player WriteTarget (entity {player_id}) ")
                           for line in lines), f"the player teleport was not queued: {lines}"
                target.send_chat("tick")
                target_packets = drain(target)
                moved = [decode_player_position(body) for pid, body in target_packets if pid == 0x49]
                assert moved, "the target's session sent no player_position for the queued move"
                sx, sy, sz = moved[-1]
                assert (abs(sx - px), abs(sy - py), abs(sz - pz)) == (0.0, 0.0, 0.0), \
                    f"the queued move landed at {sx}, {sy}, {sz}, not {px}, {py}, {pz}"
                viewer_packets = drain(bot)
                seen = [sync for sync in
                        (decode_position_sync(body) for pid, body in viewer_packets if pid == 0x23)
                        if sync[0] == player_id]
                assert seen, "the other player's client saw no entity teleport for the move"

                bot.send_chat("/whereami WriteTarget")
                packets = drain(bot)
                lines = chats(packets)
                where = [line for line in lines if line.startswith("(whereami) WriteTarget at ")]
                assert where, f"/whereami for the target is missing: {lines}"
                before = float(re.search(r"health (\d+\.\d+)", where[0]).group(1))
                bot.send_chat(f"/entdmg {player_id} 2")
                packets = drain(bot)
                lines = chats(packets)
                assert any(line == f"(entdmg) queued 2.0 damage for player WriteTarget "
                                   f"(entity {player_id})" for line in lines), \
                    f"the player hit was not queued: {lines}"
                target.send_chat("tick")
                target_packets = drain(target)
                assert any(pid == 0x19 and rd_varint(body)[0] == player_id
                           for pid, body in target_packets), \
                    "the target's client saw no damage_event for the queued hit"
                assert any(pid == 0x2b and rd_varint(body)[0] == player_id
                           for pid, body in target_packets), \
                    "the target's client saw no hurt_animation for the queued hit"
                bot.send_chat("/whereami WriteTarget")
                packets = drain(bot)
                lines = chats(packets)
                where = [line for line in lines if line.startswith("(whereami) WriteTarget at ")]
                after = float(re.search(r"health (\d+\.\d+)", where[0]).group(1))
                assert after < before, f"the queued hit did not move the health: {where[0]}"

                bot.send_chat(f"/entdespawn {player_id}")
                packets = drain(bot)
                lines = chats(packets)
                assert any(line == "(entdespawn) that entity cannot be written this way"
                           for line in lines), f"a player must not be despawnable: {lines}"
                print("[D6] player writes queued to the owning session; despawn is refused",
                      flush=True)
            finally:
                target.close()

            # D7. a lethal plugin hit. The damage itself only moves the health;
            # the world tick resolves the death, so the death event, the Remove
            # Entities, and the cow's guaranteed beef and 1..3 experience are
            # all the tick's packets, not the plugin call's. A cow is the pin
            # because its beef pool is the one drop that cannot roll zero.
            bot.send_chat("/summon minecraft:cow")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "Summoned 1 minecraft:cow" for line in lines), \
                f"/summon cow reply missing: {lines}"
            cow_id = next((decode_spawn_eid(body) for pid, body in packets if pid == 0x01),
                          None)
            assert cow_id is not None, "the summoned cow had no Add Entity packet"
            bot.send_chat(f"/entdmg {cow_id} 100")
            packets = drain(bot)
            deadline = time.time() + 3.0
            while time.time() < deadline:
                if any(pid == 0x22 and decode_entity_event(body)[0] == cow_id and
                       decode_entity_event(body)[1] == 3 for pid, body in packets):
                    break
                packets += drain(bot)
            deaths = [body for pid, body in packets if pid == 0x22 and
                      decode_entity_event(body)[0] == cow_id]
            assert any(decode_entity_event(body)[1] == 3 for body in deaths), \
                f"the cow's death event was never sent: {deaths}"
            removed = [ids for pid, body in packets if pid == 0x4e
                       for ids in [decode_remove(body)]]
            assert any(cow_id in ids for ids in removed), \
                f"the dead cow was not removed: {removed}"
            spawns = [(decode_spawn_eid(body), decode_spawn_type(body))
                      for pid, body in packets if pid == 0x01]
            item_ids = {eid for eid, kind in spawns if kind == 72}
            orb_ids = {eid for eid, kind in spawns if kind == 50}
            assert item_ids, f"the cow dropped no item entity: {spawns}"
            assert orb_ids, f"the cow dropped no experience orb: {spawns}"
            stacks = [stack for pid, body in packets if pid == 0x65
                      for stack in [decode_item_entity_stack(body)]
                      if stack is not None and stack[0] in item_ids]
            assert stacks and all(1 <= count <= 3 for _, _, count in stacks), \
                f"the loot stack is empty or out of the beef range: {stacks}"
            values = [value for pid, body in packets if pid == 0x65
                      for value in [decode_orb_value(body)]
                      if value is not None and value[0] in orb_ids]
            assert values and all(1 <= value <= 3 for _, value in values), \
                f"the orb value is outside the cow's 1..3 range: {values}"
            bot.send_chat("/entities")
            packets = drain(bot)
            lines = chats(packets)
            listing = [line for line in lines if line.startswith("(entities) ")]
            assert listing and "minecraft:cow" not in listing[0], \
                f"the dead cow is still listed: {listing}"
            print(f"[D7] lethal plugin damage: death event, Remove Entities, "
                  f"{len(item_ids)} loot entity and {len(orb_ids)} experience orb(s)", flush=True)

            # The plugin half of the same death: PMC_EVENT_ENTITY_DEATH fired
            # once, after the loot and orbs were resolved, and `/deaths` reads
            # back what the handler recorded.
            bot.send_chat("/deaths")
            packets = drain(bot)
            lines = chats(packets)
            want = f"(deaths) death: minecraft:cow #{cow_id}"
            assert any(line.startswith(want) for line in lines), \
                f"/deaths did not report the cow ({want}): {lines}"
            print("[D7] plugin entity-death event: " +
                  next(line for line in lines if line.startswith(want)), flush=True)

            # D16. the world clock and weather. `/setworldtime 6000` moves the
            # server's clock through the same path `/time set` takes, so the
            # client receives SetTime in the same drain; `/worldclock` reads the
            # clock and the level's mode back, and `/setweather` changes the mode
            # the level reports.
            bot.send_chat("/setworldtime 6000")
            packets = drain(bot)
            times = [struct.unpack(">q", body[:8])[0] for pid, body in packets if pid == 0x73]
            assert times and 6000 in times, \
                f"/setworldtime did not broadcast SetTime 6000: {times}"
            bot.send_chat("/worldclock")
            lines = chats(drain(bot))
            clock = [line for line in lines if line.startswith("(worldclock) ")]
            assert clock and clock[0].startswith("(worldclock) time 6") and "day 0" in clock[0], \
                f"/worldclock did not read the clock back: {clock}"
            bot.send_chat("/setweather thunder 400")
            lines = chats(drain(bot))
            assert any(line == "(setweather) weather thunder for 400 ticks" for line in lines), \
                f"/setweather thunder reply missing: {lines}"
            bot.send_chat("/worldclock")
            lines = chats(drain(bot))
            clock = [line for line in lines if line.startswith("(worldclock) ")]
            assert clock and clock[0].endswith("weather thunder"), \
                f"/worldclock did not report the mode set: {clock}"
            bot.send_chat("/setweather clear")
            lines = chats(drain(bot))
            assert any(line == "(setweather) weather clear" for line in lines), \
                f"/setweather clear reply missing: {lines}"
            print("[D16] world clock and weather: SetTime 6000, thunder/clear read back",
                  flush=True)

            # D17. player messaging. `/online` reads the live count, `/broadcast`
            # reaches exactly that many players (the sender included) and
            # `/dm` reaches one player by name — a name that is not in play is
            # refused rather than silently dropped. All three are one real
            # system_chat line, so the client's own chat decoder is the proof.
            bot.send_chat("/online")
            lines = chats(drain(bot))
            online = [line for line in lines if line.startswith("(online) ")]
            assert online, f"/online reply missing: {lines}"
            match = re.match(r"\(online\) (\d+) player\(s\)$", online[0])
            assert match and int(match.group(1)) >= 1, \
                f"/online did not report a live count: {online[0]}"
            online_count = int(match.group(1))
            bot.send_chat("/broadcast server restart in five")
            lines = chats(drain(bot))
            assert "server restart in five" in lines, \
                f"/broadcast did not reach the client: {lines}"
            assert f"(broadcast) sent to {online_count} player(s)" in lines, \
                f"/broadcast's count disagrees with /online: {lines}"
            bot.send_chat(f"/dm {bot.name} hello there")
            lines = chats(drain(bot))
            assert "hello there" in lines, f"/dm did not reach the named player: {lines}"
            assert f"(dm) sent to {bot.name}" in lines, \
                f"/dm's reply naming the target is missing: {lines}"
            bot.send_chat("/dm NoSuchPlayer hi")
            lines = chats(drain(bot))
            assert "(dm) no player named NoSuchPlayer" in lines, \
                f"/dm did not refuse an absent player: {lines}"
            print("[D17] player messaging: count, broadcast and direct line over the wire",
                  flush=True)

            # The same three calls across two real connections: a direct line
            # must reach the other session's socket, and a broadcast's count is
            # the whole registry, not the sender's own connection.
            other = Bot("PluginOther", port)
            other.join()
            try:
                drain(other)  # the join burst and the plugin's welcome
                bot.send_chat("/dm PluginOther cross-connection hello")
                lines = chats(drain(bot))
                assert "(dm) sent to PluginOther" in lines, \
                    f"/dm reply missing: {lines}"
                other_lines = chats(drain(other))
                assert "cross-connection hello" in other_lines, \
                    f"the direct line did not reach the other connection: {other_lines}"
                bot.send_chat("/broadcast two clients hear this")
                lines = chats(drain(bot))
                assert "(broadcast) sent to 2 player(s)" in lines, \
                    f"/broadcast count with two clients: {lines}"
                assert "two clients hear this" in lines, \
                    f"the broadcast did not reach the sender: {lines}"
                other_lines = chats(drain(other))
                assert "two clients hear this" in other_lines, \
                    f"the broadcast did not reach the other connection: {other_lines}"
                print("[D17] messaging crosses two real connections", flush=True)
            finally:
                other.close()

            # D18. player-state writes. `/setgm` flips the target's game mode
            # through the engine's own change path (GameEvent 3 + Abilities),
            # and `/heal`/`/feed` move the bar the server owns: the client's
            # set_health (0x68) is the proof. All three are queued for the
            # target's own session, so one flying packet drives the tick that
            # applies them.
            def d18_tick():
                bot.send(0x21, bytes([1]))
                return drain(bot)

            def d18_bars(pkts):
                """set_health (0x68): [f32 health][varint food][f32 saturation]."""
                out = []
                for pid, body in pkts:
                    if pid != 0x6A or len(body) < 5:
                        continue
                    value = struct.unpack(">f", body[:4])[0]
                    food, pos = rd_varint(body, 4)
                    saturation = struct.unpack(">f", body[pos:pos + 4])[0]
                    out.append((value, food, saturation))
                return out

            def d18_events(pkts):
                """game_event (0x27): [u8 event][f32 value]."""
                return [(body[0], struct.unpack(">f", body[1:5])[0])
                        for pid, body in pkts if pid == 0x27 and len(body) >= 5]

            def d18_drive(wanted, seconds=3.0):
                """Drive the bot's own tick (a flying packet) until `wanted`
                holds on the packets seen, or the deadline passes."""
                seen = []
                deadline = time.time() + seconds
                while True:
                    seen += d18_tick()
                    if wanted(seen) or time.time() >= deadline:
                        return seen
                    time.sleep(0.05)

            # The command's own packet can be the tick that drains the queue, so
            # the search spans the command's drain and any ticks after it.
            bot.send_chat(f"/setgm {bot.name} creative")
            packets = drain(bot)
            lines = chats(packets)
            assert f"(setgm) queued creative for {bot.name}" in lines, \
                f"/setgm reply missing: {lines}"
            packets += d18_drive(lambda seen: (3, 1.0) in d18_events(seen))
            assert (3, 1.0) in d18_events(packets), \
                f"/setgm creative sent no GameEvent: {d18_events(packets)}"
            assert any(pid == 0x41 for pid, _ in packets), \
                "the game-mode change sent no abilities packet"

            bot.send_chat(f"/heal {bot.name} 9.5")
            packets = drain(bot)
            lines = chats(packets)
            assert f"(heal) queued health 9.50 for {bot.name}" in lines, \
                f"/heal reply missing: {lines}"
            packets += d18_drive(
                lambda seen: any(abs(health - 9.5) < 0.001 and food == 20
                                 for health, food, _ in d18_bars(seen)))
            bar = d18_bars(packets)
            assert any(abs(health - 9.5) < 0.001 and food == 20
                       for health, food, _ in bar), \
                f"/heal did not set health 9.5: {bar}"

            bot.send_chat(f"/feed {bot.name} 12 6.5")
            packets = drain(bot)
            lines = chats(packets)
            assert f"(feed) queued food 12 saturation 6.50 for {bot.name}" in lines, \
                f"/feed reply missing: {lines}"
            packets += d18_drive(
                lambda seen: any(food == 12 and abs(saturation - 6.5) < 0.001
                                 for _, food, saturation in d18_bars(seen)))
            bar = d18_bars(packets)
            assert any(food == 12 and abs(saturation - 6.5) < 0.001
                       for _, food, saturation in bar), \
                f"/feed did not set food 12/6.5: {bar}"

            # Back to the state the rest of the suite expects a fresh player to
            # be in: full health, full food, survival.
            bot.send_chat(f"/heal {bot.name}")
            restored = drain(bot)
            bot.send_chat(f"/feed {bot.name} 20 5")
            restored += drain(bot)
            restored += d18_drive(
                lambda seen: any(health == 20.0 and food == 20 and
                                 abs(saturation - 5.0) < 0.001
                                 for health, food, saturation in d18_bars(seen)))
            assert any(health == 20.0 and food == 20 and abs(saturation - 5.0) < 0.001
                       for health, food, saturation in d18_bars(restored)), \
                f"the restore did not return the bar to 20/20/5: {d18_bars(restored)}"
            bot.send_chat(f"/setgm {bot.name} survival")
            packets = drain(bot)
            lines = chats(packets)
            assert f"(setgm) queued survival for {bot.name}" in lines, \
                f"/setgm survival reply missing: {lines}"
            packets += d18_drive(lambda seen: (3, 0.0) in d18_events(seen))
            assert (3, 0.0) in d18_events(packets), d18_events(packets)

            bot.send_chat("/heal NoSuchPlayer 10")
            lines = chats(drain(bot))
            assert "(heal) no player named NoSuchPlayer" in lines, \
                f"/heal did not refuse an absent player: {lines}"
            print("[D18] player state writes: gamemode, health and food over the wire",
                  flush=True)

            # D19. the by-name entity lookup. A player resolves by account name
            # case-insensitively, a mob by display or registry name, and a name
            # nobody has answers without inventing an entity.
            bot.send_chat(f"/find {bot.name.lower()}")
            lines = chats(drain(bot))
            found = [line for line in lines
                     if line.startswith(f"(find) {bot.name.lower()}: ")]
            assert found and f"#{bot.entity_id} minecraft:player" in found[0], \
                f"/find did not resolve the player case-insensitively: {lines}"
            bot.send_chat("/summon minecraft:sheep")
            drain(bot)
            bot.send_chat("/find minecraft:sheep")
            lines = chats(drain(bot))
            assert any(line.startswith("(find) minecraft:sheep: #") and
                       "minecraft:sheep" in line for line in lines), \
                f"/find did not resolve a mob by registry name: {lines}"
            bot.send_chat("/find sheep")
            lines = chats(drain(bot))
            assert any(line.startswith("(find) sheep: #") and "minecraft:sheep" in line
                       for line in lines), \
                f"/find did not resolve a mob by display name: {lines}"
            bot.send_chat("/find NoSuchEntity")
            lines = chats(drain(bot))
            assert "(find) no entity named NoSuchEntity" in lines, \
                f"/find did not refuse an absent name: {lines}"
            print("[D19] entity_find resolved a player, a mob and refused a missing name",
                  flush=True)

            # D8. the by-id lookup. `/entget <id>` is one registry read, not a
            # walk of the listing, and it fills the same snapshot fields the
            # visitor sees. A fresh pig pins the mob half; the unknown and
            # discarded ids pin the refusal, so a plugin cannot mistake an
            # empty struct for an entity that is gone.
            bot.send_chat("/summon minecraft:pig")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "Summoned 1 minecraft:pig" for line in lines), \
                f"/summon for D8 reply missing: {lines}"
            lookup_id = next((decode_spawn_eid(body) for pid, body in packets if pid == 0x01),
                             None)
            assert lookup_id is not None, "the D8 pig had no Add Entity packet"
            # Put it in clear air away from the pillar, so the snapshot's
            # velocity is the tick's fall rather than a landing's zero.
            bot.send_chat(f"/enttp {lookup_id} {float(bx) + 6.0:.2f} "
                          f"{float(by) + 10.0:.2f} {float(bz) + 6.0:.2f}")
            drain(bot)
            bot.send_chat(f"/entget {lookup_id}")
            packets = drain(bot)
            lines = chats(packets)
            lookup = [line for line in lines
                      if line.startswith(f"(entget) entity {lookup_id}: ")]
            assert lookup, f"/entget reply missing: {lines}"
            match = ENTGET_RE.match(lookup[0])
            assert match, f"/entget did not fill the snapshot: {lookup[0]}"
            assert int(match.group(1)) == lookup_id, lookup[0]
            assert match.group(2) == "minecraft:pig", lookup[0]
            assert match.group(3) == "Pig", f"the display name is wrong: {lookup[0]}"
            assert float(match.group(7)) == 10.0, f"a fresh pig's health: {lookup[0]}"
            assert float(match.group(10)) < 0.0, \
                f"a falling pig should have downward velocity: {lookup[0]}"
            print(f"[D8] /entget returned one entity's snapshot: {lookup[0]}", flush=True)

            # A player's facing and velocity are observed from its own position
            # packets: the first packet starts the interval and reports 0, the
            # second one block over reports a positive x delta. Both packets
            # carry the look the snapshot must read back.
            sx, sy, sz = float(bx), float(by), float(bz)
            bot.send(0x1F, struct.pack(">dddffB", sx, sy, sz, 90.0, 45.0, 1))
            bot.send(0x1F, struct.pack(">dddffB", sx + 1.0, sy, sz, 90.0, 45.0, 1))
            drain(bot)
            bot.send_chat(f"/entget {bot.entity_id}")
            packets = drain(bot)
            lines = chats(packets)
            player_lookup = [line for line in lines
                             if line.startswith(f"(entget) entity {bot.entity_id}: ")]
            assert player_lookup, f"/entget for the player's own id: {lines}"
            player_match = ENTGET_RE.match(player_lookup[0])
            assert player_match, f"/entget did not fill the player snapshot: {player_lookup[0]}"
            assert player_match.group(2) == "minecraft:player" and \
                player_match.group(3) == "PluginBot", player_lookup[0]
            velocity = (float(player_match.group(9)), float(player_match.group(10)),
                        float(player_match.group(11)))
            assert velocity[0] > 0.0 and abs(velocity[1]) < 0.01 and abs(velocity[2]) < 0.01, \
                f"the one-block move should be an x delta: {player_lookup[0]}"
            facing = (float(player_match.group(12)), float(player_match.group(13)))
            assert facing == (90.0, 45.0), \
                f"the player's facing is not its position packet's: {player_lookup[0]}"

            # player_get reads the same registry fields, so the snapshot it
            # fills must carry the same observed delta as /entget.
            bot.send_chat("/whereami")
            packets = drain(bot)
            lines = chats(packets)
            where_line = [line for line in lines if line.startswith("(whereami) PluginBot at ")]
            assert where_line, f"/whereami reply missing: {lines}"
            where_match = WHEREAMI_RE.match(where_line[0])
            assert where_match, f"/whereami did not fill the snapshot: {where_line[0]}"
            where_velocity = tuple(float(where_match.group(i)) for i in (4, 5, 6))
            assert all(abs(a - b) < 1e-6 for a, b in zip(where_velocity, velocity)), \
                f"player_get velocity disagrees with entity_get: {where_line[0]}"
            print(f"[D8] /entget read the player's observed velocity {velocity} and facing "
                  f"{facing[0]}/{facing[1]}; /whereami's player_get agrees: {where_line[0]}",
                  flush=True)

            # A diagonal move puts a delta on every axis: each component
            # player_get reports is asserted, not just x. The pair starts from
            # the position the one-block move left the player at, so the exact
            # vector is (1, 1, 1) blocks per tick.
            bot.send(0x1F, struct.pack(">dddffB", sx, sy, sz, 90.0, 45.0, 1))
            bot.send(0x1F, struct.pack(">dddffB", sx + 1.0, sy + 1.0, sz + 1.0, 90.0, 45.0, 1))
            drain(bot)
            bot.send_chat("/whereami")
            packets = drain(bot)
            lines = chats(packets)
            diagonal = [line for line in lines if line.startswith("(whereami) PluginBot at ")]
            assert diagonal, f"/whereami after the diagonal move: {lines}"
            diagonal_match = WHEREAMI_RE.match(diagonal[0])
            assert diagonal_match, f"/whereami did not fill the snapshot: {diagonal[0]}"
            diagonal_position = tuple(float(diagonal_match.group(i)) for i in (1, 2, 3))
            assert diagonal_position == (sx + 1.0, sy + 1.0, sz + 1.0), \
                f"the diagonal move did not land: {diagonal[0]}"
            diagonal_velocity = tuple(float(diagonal_match.group(i)) for i in (4, 5, 6))
            assert diagonal_velocity == (1.0, 1.0, 1.0), \
                f"a diagonal move must report a delta on every axis: {diagonal[0]}"
            print(f"[D8] a diagonal move reached player_get on all three axes: {diagonal[0]}",
                  flush=True)

            # A plugin teleport is not movement: `/enttp` queues the move for
            # this player's own session, which applies it on its next tick and
            # zeroes the observed delta. The next `/whereami` must report the
            # destination and a zero vector, not the jump as one tick's motion.
            tx, ty, tz = sx + 4.0, sy + 2.0, sz + 4.0
            bot.send_chat(f"/enttp {bot.entity_id} {tx:.2f} {ty:.2f} {tz:.2f}")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line.startswith(f"(enttp) queued player PluginBot "
                                       f"(entity {bot.entity_id}) ")
                       for line in lines), \
                f"the player teleport was not queued: {lines}"
            # The queued write is applied by this player's own next survival
            # tick; a plain chat packet is that tick.
            bot.send_chat("tick")
            drain(bot)
            bot.send_chat("/whereami")
            packets = drain(bot)
            lines = chats(packets)
            teleported = [line for line in lines if line.startswith("(whereami) PluginBot at ")]
            assert teleported, f"/whereami after the teleport: {lines}"
            teleported_match = WHEREAMI_RE.match(teleported[0])
            assert teleported_match, f"/whereami did not fill the snapshot: {teleported[0]}"
            teleported_position = tuple(float(teleported_match.group(i)) for i in (1, 2, 3))
            assert teleported_position == (tx, ty, tz), \
                f"the plugin teleport did not move the player: {teleported[0]}"
            teleported_velocity = tuple(float(teleported_match.group(i)) for i in (4, 5, 6))
            assert teleported_velocity == (0.0, 0.0, 0.0), \
                f"a teleport must not report the jump as velocity: {teleported[0]}"
            print(f"[D8] a plugin teleport zeroed the observed velocity: {teleported[0]}",
                  flush=True)

            bot.send_chat("/entget 999999")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(entget) no such entity" for line in lines), \
                f"an unknown id must answer NOT_FOUND: {lines}"
            bot.send_chat("/entget 0")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(entget) usage: /entget <entity id>" for line in lines), \
                f"a non-positive id must be rejected: {lines}"
            # D5's pig was discarded, so its id is the stale case; D8's own
            # pig may fall out of the void before this arm ends, which is not
            # this test's subject.
            bot.send_chat(f"/entget {pig_id}")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(entget) no such entity" for line in lines), \
                f"a discarded id must answer NOT_FOUND: {lines}"
            print("[D8] unknown and discarded ids answer NOT_FOUND; a bad id is rejected",
                  flush=True)

            # D9. the append-only snapshots, proved by a fixture module built
            # for the older layout. It declares each original struct's size,
            # keeps every byte where `vx` now starts as a canary, and calls
            # player_get and entity_get through those shorter views. The host
            # must fill each prefix and leave the canary alone — writing
            # `sizeof(pmc_player)` or `sizeof(pmc_entity)` bytes would
            # overwrite it, which is the regression this pins.
            if old_library is not None:
                bot.send_chat("/oldabi PluginBot")
                packets = drain(bot)
                lines = chats(packets)
                old = [line for line in lines if line.startswith("(oldabi) PluginBot at ")]
                assert old, f"/oldabi reply missing: {lines}"
                assert old[0].endswith(", tail intact"), \
                    f"player_get wrote past the pre-velocity struct: {old[0]}"
                assert f"(entity {bot.entity_id}," in old[0], \
                    f"/oldabi did not fill the old prefix: {old[0]}"
                print(f"[D9] an older plugin's shorter pmc_player was filled and whole: "
                      f"{old[0]}", flush=True)

                # The other half of the player size rule: a struct that ends
                # before the pre-velocity prefix is not a shorter prefix. The
                # live name proves the size, not the lookup, is what is
                # refused, and the canary proves the refusal wrote nothing.
                bot.send_chat("/oldptrunc PluginBot")
                packets = drain(bot)
                lines = chats(packets)
                player_truncated = [line for line in lines if line.startswith("(oldptrunc) ")]
                assert player_truncated, f"/oldptrunc reply missing: {lines}"
                assert player_truncated[0] == "(oldptrunc) rc -1, struct untouched", \
                    f"an undersized pmc_player was not rejected cleanly: " \
                    f"{player_truncated[0]}"
                print(f"[D9] an undersized pmc_player was rejected without a write: "
                      f"{player_truncated[0]}", flush=True)

                # A caller built against a newer header declares a larger
                # struct. The host fills its own size (velocity is the last
                # player field it knows), reports that count in struct_size,
                # and leaves the extension alone.
                bot.send_chat("/newplayer PluginBot")
                packets = drain(bot)
                lines = chats(packets)
                future_player = [line for line in lines
                                 if line.startswith("(newplayer) PluginBot at ")]
                assert future_player, f"/newplayer reply missing: {lines}"
                assert "struct_size 144 of 160, extension intact" in future_player[0], \
                    f"a larger pmc_player did not report the host's fill: {future_player[0]}"
                assert "(vel 0.000, 0.000, 0.000)" in future_player[0], \
                    f"the host did not fill its own player size: {future_player[0]}"
                print(f"[D9] a newer pmc_player reported the host's size: "
                      f"{future_player[0]}", flush=True)

                bot.send_chat(f"/oldentget {bot.entity_id}")
                packets = drain(bot)
                lines = chats(packets)
                old_entity = [line for line in lines
                              if line.startswith(f"(oldentget) entity {bot.entity_id}: ")]
                assert old_entity, f"/oldentget reply missing: {lines}"
                old_entity_match = OLDENTGET_RE.match(old_entity[0])
                assert old_entity_match, \
                    f"/oldentget did not fill the snapshot: {old_entity[0]}"
                assert old_entity_match.group(1) == str(bot.entity_id), old_entity[0]
                assert old_entity_match.group(2) == "minecraft:player", old_entity[0]
                assert old_entity_match.group(3) == "PluginBot", old_entity[0]
                assert old_entity_match.group(9) == "tail intact", \
                    f"entity_get wrote past the pre-velocity struct: {old_entity[0]}"
                print(f"[D9] an older plugin's shorter pmc_entity was filled and whole: "
                      f"{old_entity[0]}", flush=True)

                # The other half of the size rule: a struct that ends before
                # the pre-velocity prefix is not a shorter prefix. A live id
                # proves the size, not the lookup, is what is refused, and the
                # canary proves the refusal wrote nothing.
                bot.send_chat(f"/oldtrunc {bot.entity_id}")
                packets = drain(bot)
                lines = chats(packets)
                truncated = [line for line in lines if line.startswith("(oldtrunc) ")]
                assert truncated, f"/oldtrunc reply missing: {lines}"
                assert truncated[0] == "(oldtrunc) rc -3, struct untouched", \
                    f"an undersized pmc_entity was not rejected cleanly: {truncated[0]}"
                print(f"[D9] an undersized pmc_entity was rejected without a write: "
                      f"{truncated[0]}", flush=True)

                # The same forward-compat case for the entity snapshot: yaw
                # and pitch are the last fields this host knows, so they prove
                # the fill reached the host's size while the extension stayed
                # and struct_size reported that size.
                bot.send_chat(f"/newentity {bot.entity_id}")
                packets = drain(bot)
                lines = chats(packets)
                future_entity = [line for line in lines
                                 if line.startswith(f"(newentity) entity {bot.entity_id}: ")]
                assert future_entity, f"/newentity reply missing: {lines}"
                assert "struct_size 208 of 224, extension intact" in future_entity[0], \
                    f"a larger pmc_entity did not report the host's fill: {future_entity[0]}"
                assert 'minecraft:player "PluginBot"' in future_entity[0], \
                    f"/newentity did not fill the prefix: {future_entity[0]}"
                assert "yaw 90.0, pitch 45.0" in future_entity[0], \
                    f"the host did not fill its own entity size: {future_entity[0]}"
                print(f"[D9] a newer pmc_entity reported the host's size: "
                      f"{future_entity[0]}", flush=True)

                # The third path: the `entities` visitor receives host-owned
                # snapshots, so every one must carry the host's full
                # sizeof(pmc_entity) — there is no caller size to shrink to.
                bot.send_chat("/entitysize")
                packets = drain(bot)
                lines = chats(packets)
                sizes = [line for line in lines if line.startswith("(entitysize) ")]
                assert sizes, f"/entitysize reply missing: {lines}"
                size_match = ENTITYSIZE_RE.match(sizes[0])
                assert size_match, f"/entitysize did not fill the listing: {sizes[0]}"
                assert int(size_match.group(1)) >= 1, sizes[0]
                assert int(size_match.group(2)) == 208, sizes[0]
                print(f"[D9] the entities visitor carried the host's struct_size: "
                      f"{sizes[0]}", flush=True)

                # The item snapshot has an exact-size rule, not an append-only
                # one: a struct that stops before `name` is refused. Slot 0 and
                # the live name are valid, so the declared size is the only
                # reason for the answer.
                bot.send_chat("/invtrunc PluginBot")
                packets = drain(bot)
                lines = chats(packets)
                inv_truncated = [line for line in lines if line.startswith("(invtrunc) ")]
                assert inv_truncated, f"/invtrunc reply missing: {lines}"
                assert inv_truncated[0] == "(invtrunc) rc -2, struct untouched", \
                    f"an undersized pmc_item was not rejected cleanly: {inv_truncated[0]}"
                print(f"[D9] an undersized pmc_item was rejected without a write: "
                      f"{inv_truncated[0]}", flush=True)
            else:
                print("[D9] skipped: no old-ABI fixture on the command line", flush=True)

            # D10. the host table's prefix. The table only grows; a plugin
            # built against the original fields (through `server_version`)
            # must load, see a `struct_size` that covers its prefix, and be
            # able to log, register and answer through those fields alone.
            if oldhost_library is not None:
                wait_for_log(log_path, "old-host fixture: read the original table prefix")
                bot.send_chat("/oldhost")
                packets = drain(bot)
                lines = chats(packets)
                oldhost = [line for line in lines if line.startswith("(oldhost) ")]
                assert oldhost, f"/oldhost reply missing: {lines}"
                oldhost_match = OLDHOST_RE.match(oldhost[0])
                assert oldhost_match, f"/oldhost did not read the prefix: {oldhost[0]}"
                assert int(oldhost_match.group(1)) >= int(oldhost_match.group(2)), \
                    f"the host table does not cover the original prefix: {oldhost[0]}"
                print(f"[D10] the original host-table prefix loaded and answered: "
                      f"{oldhost[0]}", flush=True)
            else:
                print("[D10] skipped: no old-host fixture on the command line", flush=True)

            bot.send_chat("/whereami NoSuchPlayer")
            packets = drain(bot)
            lines = chats(packets)
            assert any(line == "(whereami) no player named 'NoSuchPlayer'" for line in lines), \
                f"the missing-player answer is wrong: {lines}"
            print("[D] an unknown player answers without inventing a snapshot", flush=True)

            wait_for_log(log_path, "[greeter] heartbeat at tick", timeout=10.0)
            print("[E] the 20-tick scheduler task ran on the tick loop", flush=True)

            # L. the item-use event. Buckets, flint and steel and TNT priming
            # all reach it before their effect; `/itemguard` vetoes them. The
            # TNT arm is last on purpose: an allowed priming starts a fuse, and
            # the server is stopped right after these assertions.
            def set_carried(slot):
                bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", slot))
                drain(bot, 0.2)

            def invget():
                bot.send_chat("/invget 0")
                lines = chats(drain(bot))
                found = [line for line in lines if line.startswith("(invget) PluginBot slot 0: ")]
                assert found, f"/invget slot 0 missing: {lines}"
                return found[0]

            def set_slot(item):
                bot.send_chat(f"/invset 0 {item} 1")
                lines = chats(drain(bot))
                assert any(line == f"(invset) queued slot 0 of PluginBot -> {item} x1"
                           for line in lines), f"/invset {item}: {lines}"
                set_carried(0)
                deadline = time.time() + 2.0
                while time.time() < deadline:
                    line = invget()
                    if f"{item} x1" in line:
                        return
                    time.sleep(0.1)
                raise AssertionError(f"/invset {item} did not apply: {invget()}")

            def setblock(cell, block):
                bot.send_chat(f"/setblock {cell[0]} {cell[1]} {cell[2]} {block}")
                updates = block_updates(drain(bot))
                assert cell in updates, f"/setblock {block} at {cell} sent no update: {updates}"
                return updates[cell]

            def item_use(cell, item, block, seq):
                needle = (f"[greeter] item use at ({cell[0]}, {cell[1]}, {cell[2]}) "
                          f"face 1: {item} on {block}")
                before = read_log(log_path).count(needle)
                use_item_on(bot, cell[0], cell[1], cell[2], 1, seq)
                deadline = time.time() + STEP_TIMEOUT
                while time.time() < deadline:
                    if read_log(log_path).count(needle) > before:
                        return drain(bot)
                    time.sleep(0.1)
                raise AssertionError(f"no item-use event for {item} on {block} at {cell}: "
                                     f"{read_log(log_path)[-1200:]}")

            def itemguard(mode):
                bot.send_chat(f"/itemguard {mode}")
                lines = chats(drain(bot))
                assert f"(itemguard) use={mode}" in lines, f"/itemguard {mode}: {lines}"

            water_cell = (bx + 2, by + 1, bz)
            water_id = setblock(water_cell, "minecraft:water")
            set_slot("minecraft:bucket")
            packets = item_use(water_cell, "minecraft:bucket", "minecraft:water", 407)
            assert block_updates(packets).get(water_cell) == 0, \
                f"the bucket did not take the source: {block_updates(packets)}"
            assert "minecraft:water_bucket x1" in invget(), invget()
            print("[L] a real bucket pickup fired PMC_EVENT_ITEM_USE_ON", flush=True)

            water_id = setblock(water_cell, "minecraft:water")
            set_slot("minecraft:bucket")
            itemguard("deny")
            packets = item_use(water_cell, "minecraft:bucket", "minecraft:water", 408)
            assert "(greeter) item use denied" in chats(packets), chats(packets)
            assert block_updates(packets).get(water_cell) == water_id, \
                f"a denied pickup did not restore the source: {block_updates(packets)}"
            assert "minecraft:bucket x1" in invget(), invget()
            print("[L] a denied bucket pickup kept the source and the bucket", flush=True)

            itemguard("allow")
            packets = item_use(water_cell, "minecraft:bucket", "minecraft:water", 409)
            assert block_updates(packets).get(water_cell) == 0, \
                f"the allowed pickup did not take the source: {block_updates(packets)}"
            assert "minecraft:water_bucket x1" in invget(), invget()

            stone_cell = (bx + 4, by, bz)
            target_cell = (bx + 4, by + 1, bz)
            setblock(target_cell, "minecraft:air")
            setblock(stone_cell, "minecraft:stone")
            itemguard("deny")
            packets = item_use(stone_cell, "minecraft:water_bucket", "minecraft:stone", 410)
            assert "(greeter) item use denied" in chats(packets), chats(packets)
            assert block_updates(packets).get(target_cell) == 0, \
                f"a denied bucket placement wrote water: {block_updates(packets)}"
            assert "minecraft:water_bucket x1" in invget(), invget()
            print("[L] a denied bucket placement kept the water and the bucket", flush=True)

            itemguard("allow")
            packets = item_use(stone_cell, "minecraft:water_bucket", "minecraft:stone", 411)
            assert block_updates(packets).get(target_cell) == water_id, \
                f"the allowed bucket placement wrote no water: {block_updates(packets)}"
            assert "minecraft:bucket x1" in invget(), invget()

            tnt_cell = (bx + 3, by, bz)
            tnt_id = setblock(tnt_cell, "minecraft:tnt")
            set_slot("minecraft:flint_and_steel")
            itemguard("deny")
            packets = item_use(tnt_cell, "minecraft:flint_and_steel", "minecraft:tnt", 412)
            assert "(greeter) item use denied" in chats(packets), chats(packets)
            assert block_updates(packets).get(tnt_cell) == tnt_id, \
                f"a denied priming did not restore the TNT: {block_updates(packets)}"
            flint = invget()
            assert "minecraft:flint_and_steel x1" in flint and "damage 0" in flint, flint
            print("[L] a denied TNT priming kept the block and the flint", flush=True)

            itemguard("allow")
            packets = item_use(tnt_cell, "minecraft:flint_and_steel", "minecraft:tnt", 413)
            assert block_updates(packets).get(tnt_cell) == 0, \
                f"the allowed priming did not take the block: {block_updates(packets)}"
            wait_for_log(log_path, f"PluginBot primed TNT at ({tnt_cell[0]}, "
                                   f"{tnt_cell[1]}, {tnt_cell[2]})")
            bot.send_chat("/entities")
            lines = chats(drain(bot))
            listing = [line for line in lines if line.startswith("(entities) ")]
            assert listing and "minecraft:tnt x1" in listing[0], \
                f"the primed TNT is not in the entity listing: {listing}"
            flint = invget()
            assert "minecraft:flint_and_steel x1" in flint and "damage 1" in flint, flint
            print("[L] an allowed TNT priming fired the event and left a primed entity",
                  flush=True)

            # M. runtime unload and hot reload. An edit made to config.toml
            # outside the host is invisible to the running module; a reload is
            # what makes it take effect, and the fresh load line proves the new
            # module read the file. A plugin another loaded plugin depends on
            # cannot be closed first: oldhost names greeter in its manifest.
            config_path = os.path.join(plugins_dir, "greeter", "data", "config.toml")
            with open(config_path, "w", encoding="utf-8") as fh:
                fh.write('greeting = "read-on-reload"\nheartbeat_ticks = 5\ndebug = true\n')
            bot.send_chat("/config greeting")
            lines = chats(drain(bot))
            assert '(config) greeting = "saved-by-config"' in lines, \
                f"the running module saw a file edit it was not reloaded for: {lines}"
            if oldhost_library is not None:
                bot.send_chat("/plugins reload greeter")
                lines = chats(drain(bot))
                assert any("Cannot reload plugin 'greeter': oldhost depends on it" in line
                           for line in lines), \
                    f"a dependent did not block the reload: {lines}"
                bot.send_chat("/plugins unload oldhost")
                lines = chats(drain(bot))
                assert "Unloading plugin 'oldhost'" in lines, f"/plugins unload: {lines}"
                wait_for_log(log_path, "plugins: unloaded oldhost")
            bot.send_chat("/plugins reload greeter")
            lines = chats(drain(bot))
            assert "Reloading plugin 'greeter'" in lines, f"/plugins reload: {lines}"
            wait_for_log(log_path, "plugins: reloaded greeter 1.13.0")
            wait_for_log(
                log_path,
                "[greeter] config: greeting=read-on-reload heartbeat_ticks=5 debug=1")
            bot.send_chat("/config greeting")
            lines = chats(drain(bot))
            assert '(config) greeting = "read-on-reload"' in lines, \
                f"the reloaded module did not read the file: {lines}"
            bot.send_chat("/greet Reloaded")
            lines = chats(drain(bot))
            assert any("Hello, Reloaded!" in line for line in lines), \
                f"the reloaded module's command did not run: {lines}"
            bot.send_chat("/plugins")
            lines = chats(drain(bot))
            assert any(line.startswith("greeter 1.13.0 (enabled)") for line in lines), \
                f"/plugins listing: {lines}"
            if oldhost_library is not None:
                assert not any(line.startswith("oldhost ") for line in lines), \
                    f"an unloaded plugin is still listed: {lines}"
            # Put the three values back through the new module's writers, so the
            # restart arm below still checks what the host wrote.
            wait_for_chat(bot, lambda line: "(config) set greeting" in line,
                          command="/config greeting saved-by-config",
                          what="config greeting write")
            wait_for_chat(bot, lambda line: "(config) set heartbeat_ticks" in line,
                          command="/config heartbeat_ticks 5",
                          what="config heartbeat_ticks write")
            wait_for_chat(bot, lambda line: "(config) set debug" in line,
                          command="/config debug true",
                          what="config debug write")
            print("[M] hot reload closed the module and the new one re-read its config",
                  flush=True)

            # J. leave fires only after the registry entry and leave burst are
            # gone; closing the client is the session teardown that drives it,
            # and the handler's player_get confirms the entry is already gone.
            bot.close()
            bot = None
            wait_for_log(log_path, "[greeter] saw leave: PluginBot (player_get: gone)")
            print("[J] the plugin saw the leave event after the entry was gone", flush=True)
        finally:
            if bot is not None:
                bot.close()
                bot = None
            code, log = stop(proc, log_path)
        assert code == 0, f"SIGTERM must exit 0, got {code}: {log[-800:]}"
        assert "[greeter] disabled" in log, "on_disable did not run"
        assert "plugins: disabled greeter" in log, "the host did not log the disable"
        assert "plugins: unloaded greeter" in log, "the library was not unloaded"
        print("[F] stop fired the event, disable and unload, and exited 0", flush=True)

        # C3. persistence: the writers replaced the plugin's config.toml, and a
        # second boot with the same data directory reads the values back. This
        # is the half a same-process getter cannot prove.
        saved_path = os.path.join(plugins_dir, "greeter", "data", "config.toml")
        with open(saved_path, "r", encoding="utf-8") as fh:
            saved = fh.read()
        assert 'greeting = "saved-by-config"' in saved, f"written string missing: {saved}"
        assert "heartbeat_ticks = 5" in saved, f"written integer missing: {saved}"
        assert "debug = true" in saved, f"written boolean missing: {saved}"
        reload_log = os.path.join(root, "reload.log")
        proc, port = start(binary, data_root, ["--plugins", plugins_dir], reload_log, env)
        try:
            wait_for_log(
                reload_log,
                "[greeter] config: greeting=saved-by-config heartbeat_ticks=5 debug=1")
        finally:
            code, log = stop(proc, reload_log)
        assert code == 0, f"the restart that re-read the config did not stop cleanly: {log[-800:]}"
        print("[C3] the written config survived a restart", flush=True)

        # --- F. one broken plugin is skipped, not fatal ----------------------
        bad_root = tempfile.mkdtemp(prefix="plusmc-plugin-badabi-")
        try:
            bad_dir = write_plugin(bad_root, manifest, library, name="greeter2", api=2)
            bad_log = os.path.join(bad_root, "bad.log")
            proc, port = start(binary, data_root, ["--plugins", bad_dir], bad_log, env)
            try:
                code, log = stop(proc, bad_log)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=5)
            assert "wants ABI 2 but this engine is ABI 1" in log, \
                f"the ABI refusal did not name both versions: {log[-900:]}"
            assert "plugins: 0 loaded, 1 failed" in log, f"the failure was not counted: {log[-900:]}"
            assert "listening on" in log and code == 0, \
                "a refused plugin must not stop the server from serving"
            print("[G] a wrong-ABI plugin was refused, counted, and the server still served",
                  flush=True)
        finally:
            shutil.rmtree(bad_root, ignore_errors=True)

        # --- G. a typo is an error, not a silent empty plugin set -----------
        missing = os.path.join(root, "no-such-plugins")
        code, output = run_validate(binary, missing, data_root, env)
        assert code == 1 and "--plugins" in output and "not a directory" in output, \
            f"--validate-config must name the bad --plugins path (rc {code}): {output[-900:]}"
        log_path = os.path.join(root, "missing.log")
        with open(log_path, "wb") as log:
            proc = wire_client.start_process(server_args(binary, data_root, ["--plugins", missing]),
                                    stdout=log, stderr=subprocess.STDOUT,
                                    env=wire_client.server_env(env))
        code = proc.wait(timeout=STEP_TIMEOUT)
        log = read_log(log_path)
        assert code == 1 and "not a directory" in log, \
            f"a bad --plugins path must fail the start (rc {code}): {log[-900:]}"
        print("[H] a --plugins path that is not a directory fails the start", flush=True)

        # --- H. the accessors read a real generated world -------------------
        # The earlier arm's server is the void world, where every block read is
        # air (a truthful answer, but one a broken accessor could also give).
        # This one serves `--worldgen 42`, so the block under the spawn is what
        # the generator put there and the biome is a real one.
        world_root = tempfile.mkdtemp(prefix="plusmc-plugin-world-")
        try:
            world_plugins = write_plugin(world_root, manifest, library)
            world_log = os.path.join(world_root, "plugins.log")
            proc, port = start(binary, data_root,
                               ["--plugins", world_plugins, "--worldgen", "42",
                                "--view-distance", "2"], world_log, env)
            world_bot = None
            try:
                world_bot = Bot("WorldBot", port)
                world_bot.join()
                # A generated world materialises the spawn column through the
                # session's own streaming, which can land a moment after the
                # join; an unloaded chunk reads as air (and the accessor does
                # not force a load). Poll until the column is there, then pin
                # the answer. The deadline is generous because generation is
                # the point of the arm.
                deadline = time.time() + 20.0
                match = None
                while time.time() < deadline:
                    world_bot.send_chat("/whereami")
                    packets = drain(world_bot, timeout=20.0)
                    lines = chats(packets)
                    where = [line for line in lines if line.startswith("(whereami) WorldBot at ")]
                    assert where, f"/whereami in a generated world replied with: {lines}"
                    match = re.search(
                        r"biome (minecraft:[a-z_]+), feet in (minecraft:[a-z_]+), "
                        r"standing on (minecraft:[a-z_]+)", where[0])
                    assert match, f"/whereami did not name the world's content: {where[0]}"
                    if match.group(3) != "minecraft:air":
                        break
                    time.sleep(0.25)
                assert match.group(3) != "minecraft:air", \
                    f"/whereami never saw a generated surface block: {where[0]}"
                assert match.group(1) != "minecraft:the_void", \
                    f"/whereami reported the void biome in a generated world: {where[0]}"
                print(f"[I] /whereami read a generated world: {match.group(1)}, "
                      f"standing on {match.group(3)}", flush=True)
            finally:
                if world_bot is not None:
                    world_bot.close()
                code, log = stop(proc, world_log)
            assert code == 0, f"the generated-world arm must stop cleanly: {log[-800:]}"
        finally:
            shutil.rmtree(world_root, ignore_errors=True)

        # --- J. plugin command operator levels -------------------------------
        # Both names are in ops.json, so the engine's existing all-commands gate
        # lets them through; the plugin's level-3 command then distinguishes the
        # level-2 operator from the level-4 one.
        perm_root = tempfile.mkdtemp(prefix="plusmc-plugin-perms-")
        try:
            perm_config = os.path.join(perm_root, "config")
            os.makedirs(perm_config)
            with open(os.path.join(perm_config, "eula.txt"), "w", encoding="utf-8") as fh:
                fh.write("eula=true\n")
            with open(os.path.join(perm_config, "ops.json"), "w", encoding="utf-8") as fh:
                json.dump([
                    {"uuid": "33333333-3333-3333-3333-333333333333",
                     "name": "OpBuilder", "level": 2, "bypassesPlayerLimit": False},
                    {"uuid": "44444444-4444-4444-4444-444444444444",
                     "name": "OpAdmin", "level": 4, "bypassesPlayerLimit": False},
                ], fh, indent=2)
            perm_plugins = write_plugin(perm_root, manifest, library)
            perm_log = os.path.join(perm_root, "perms.log")
            proc, port = start(binary, data_root,
                               ["--plugins", perm_plugins, "--config-dir", perm_config],
                               perm_log, env)
            builder = None
            admin = None
            try:
                wait_for_log(perm_log, "ops: 2 operators")
                builder = Bot("OpBuilder", port)
                builder.join()
                builder.send_chat("/opcmd")
                lines = chats(drain(builder, timeout=4.0))
                assert any("requires operator level 3" in line for line in lines), lines
                print("[J] a level-2 operator was denied a level-3 plugin command", flush=True)
                admin = Bot("OpAdmin", port)
                admin.join()
                admin.send_chat("/opcmd")
                lines = chats(drain(admin, timeout=4.0))
                assert any("(opcmd) operator level 3 command ran for OpAdmin (level 4)" in line
                           for line in lines), lines
                print("[J] a level-4 operator ran the level-3 command", flush=True)
            finally:
                if builder is not None:
                    builder.close()
                if admin is not None:
                    admin.close()
                code, log = stop(proc, perm_log)
            assert code == 0, f"the permissions arm must stop cleanly: {log[-800:]}"
        finally:
            shutil.rmtree(perm_root, ignore_errors=True)

        print("\nPASS: plugin API (load, chat, command, accessors, entities, lookup, "
              "inventory, entity writes, old ABI, host table prefix, player lifecycle, "
              "block events, item use, scheduler, unload, isolation)", flush=True)
        return 0
    finally:
        if bot is not None:
            bot.close()
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
