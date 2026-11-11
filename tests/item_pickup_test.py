#!/usr/bin/env python3
"""Integration test: dropped items as entities over the wire (Phase 4 follow-up).

The block-break drop used to be *spawn-and-forget*: an Add Entity and a Set
Entity Data, broadcast, with nothing behind them. The client drew an item that
did not exist — it never fell, never expired and could never be picked up — so a
survival world was unplayable past the first block. The user's manual acceptance
found exactly that: break stone and the cobblestone sat there forever.

The simulation is world/item_entity.{h,cpp} (pure, and unit-tested without a
client); the session half is what this test drives, and every observation is one
the old code could not produce:

  1. **A drop is a real entity.** `/setblock ... destroy` spawns one through the
     same `spawn_item_drop` the break path uses: an Add Entity of entity type 71,
     its Set Entity Data carrying item 55 (dirt, the block the command destroys),
     and then *position syncs* — the drop falls, which only happens if a
     server-side body is ticking it.
  2. **It comes to rest on the floor.** The last position sync of the drop has it
     at the platform's top face (y = 64.0), with the on-ground flag set: gravity,
     collision and the drag all ran, not just a shove.
  3. **Two drops merge.** A second drop in the same cell makes the survivor's Set
     Entity Data change to count 2 (vanilla's `ItemEntity.merge` calls `setItem`,
     which is what makes DATA_ITEM dirty) and a `remove_entities` for the absorbed
     one — two packets that only a server-side merge can produce.
  4. **The pickup is range-gated.** While the bot stands three blocks away,
     *nothing* is taken: the negative control that separates "the item is real"
     from "the item is collected". The unit tests pin `Player.aiStep`'s inflated
     pickup box; this is the wire's proof of it.
  5. **Walking into the drop collects it.** `take_item_entity` (0x7c) with the
     item's id, the bot's id and the amount — 2, the merged stack — followed by
     `container_set_content` (0x12) showing that stack in the bot's own menu.
     That is `ItemEntity.playerTouch` end to end: the entity vanished *because*
     `Inventory.add` accepted it, which is the whole point of the fix.
  6. **A creative drop is a real entity too.** `set_creative_slot` (0x38) with
     slot -1 is the creative screen's throw: `player.drop(stack, true)` under
     vanilla's `hasInfiniteMaterials` gate. It spawns an Add Entity at the
     thrower's eye height less 0.3 (not a block centre), carrying the whole
     three-dirt stack, with no `container_set_slot` — a negative slot is not a
     write. It then falls and comes to rest like any other drop.

Both server clocks are left at their defaults. A drop's own tick is the world
clock, but the *pickup drain* is the survival tick, and that clock advances on
received serverbound packets (`tick_survival_on_packet`) — so while the bot
stands in the drop it sends the same one `flying` packet per tick a real client
does. `PLUSMC_SURVIVAL_CLOCK=packet` is deliberately *not* set: it would make the
survival tick one packet rather than the wall clock the world tick runs on.

Run: item_pickup_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Clientbound packet ids.
CB_SPAWN_ENTITY = 0x01
CB_CONTAINER_SET_CONTENT = 0x12
CB_CONTAINER_SET_SLOT = 0x14
CB_ENTITY_POSITION_SYNC = 0x23
CB_KEEP_ALIVE = 0x2D
CB_REMOVE_ENTITIES = 0x4E
CB_TAKE_ITEM_ENTITY = 0x7F
CB_SET_ENTITY_DATA = 0x65
CB_UPDATE_ATTRIBUTES = 0x86

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_POSITION = 0x1E
SB_SET_CREATIVE_SLOT = 0x39
SB_KEEP_ALIVE = 0x1C
SB_FLYING = 0x21

# minecraft:entity_type protocol id of minecraft:item (src/protocol/ids.h).
ET_ITEM = 72
# Item ids (src/world/item_registry.h). The drop is dirt, not stone, on purpose:
# stone needs a pickaxe to drop anything at all (`Block.dropResources`'s tool
# gate), and this test is about the drop's *life after the spawn*, not about the
# tool. `/setblock`'s `destroy` mode goes through the same drop resolution a
# break does.
ITEM_DIRT = 55

# ItemEntity.DATA_ITEM's synched-data index and the ITEM_STACK serializer id
# (src/server/entity_packets.h).
DATA_ITEM_INDEX = 8
ITEM_STACK_SERIALIZER = 7

# The player inventory menu: 46 slots, hotbar 0 at menu slot 36
# (src/world/inventory.h).
PLAYER_MENU_SIZE = 46
MENU_HOTBAR_FIRST = 36

# The platform's blocks sit at y = 63, so its top face — where a drop lands — is
# y = 64.0 and the bot stands at y = 64.0.
FLOOR_Y = 63
STAND_Y = 64.0
PLATFORM = [(x, FLOOR_Y, z) for x in range(6) for z in range(6)]
# The drop's own cell: one block up from the platform, three blocks from where
# the bot stands — outside the (1.0, 0.5, 1.0) pickup inflation (its reach from
# the bot's centre is 1.3 on each axis), so nothing can be collected early.
DROP_CELL = (3, STAND_Y, 3)
# A walled cup around that cell, two blocks tall. `item_launch`'s up-to-0.1
# horizontal velocity is dragged by only 0.98 per tick, so a drop thrown into
# open air can end up more than a block from where it spawned — far enough that
# two drops never merge (the scan's reach is 0.25 + 2*0.5), and at a platform's
# edge far enough to slide off it. The walls bound the drift to half a block,
# which is what makes the merge below deterministic rather than lucky. They are
# two tall because the launch's apex (~64.9, from y = 64.5 at 0.04 gravity) puts
# the drop's 0.25-high box level with the top of a single wall.
CUP_WALLS = [(x, y, z)
             for x, z in ((2, 3), (4, 3), (3, 2), (3, 4))
             for y in (STAND_Y, STAND_Y + 1)]
DROP_CENTRE = (DROP_CELL[0] + 0.5, DROP_CELL[1] + 0.5, DROP_CELL[2] + 0.5)
BOT_HOME = (0.5, STAND_Y, 0.5)
BOT_AT_DROP = (DROP_CELL[0] + 0.5, STAND_Y, DROP_CELL[2] + 0.5)


# --- framing -----------------------------------------------------------------

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
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos + ln].decode("utf-8"), pos + ln


# --- decoders ----------------------------------------------------------------

def read_spawn(body):
    """Add Entity: [varint eid][uuid 16][varint type][f64 x y z][LpVec3]
    [u8 pitch][u8 yaw][u8 head yaw][varint data]."""
    eid, pos = rd_varint(body, 0)
    uuid = body[pos:pos + 16]
    pos += 16
    etype, pos = rd_varint(body, pos)
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    # LpVec3 (src/protocol/lp_vec.h): a single 0 byte means a zero vector,
    # otherwise two marker bytes, a big-endian u32 and (when the marker's low
    # three bits say the scale did not fit) a scale varint. The launch velocity
    # is well under 1, so this is the flat form tonight.
    first = body[pos]
    if first == 0:
        pos += 1
    else:
        pos += 6
        if first & 0x4:
            _, pos = rd_varint(body, pos)
    pos += 3
    data, pos = rd_varint(body, pos)
    assert pos == len(body), f"spawn_entity: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "uuid": uuid, "type": etype, "x": x, "y": y, "z": z, "data": data}


def read_position_sync(body):
    """entity_position_sync (0x23, 26.3): [varint eid][varint PositionPath
    type = LINEAR 0][f64 x y z][f32 yRot][f32 xRot][bool onGround]. The delta
    movement is no longer on the wire."""
    eid, pos = rd_varint(body, 0)
    path_type, pos = rd_varint(body, pos)
    assert path_type == 0, f"position_sync path type {path_type}"
    x, y, z = struct.unpack(">ddd", body[pos:pos + 24])
    pos += 24
    pos += 8  # yRot, xRot
    ground = body[pos]
    pos += 1
    assert pos == len(body), f"position_sync: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "x": x, "y": y, "z": z,
            "ground": bool(ground)}


def read_item_data(body):
    """ItemEntity's Set Entity Data, decoded byte for byte:

    [varint eid][u8 DATA_ITEM index][varint ITEM_STACK serializer][varint count]
    [varint item][varint 0 added components][varint 0 removed][0xFF].

    Every other entity's Set Entity Data is a different shape (a mob's health
    word, a player's flags), so this asserts the whole packet rather than
    scanning: a drop's data must not be a mob's.
    """
    eid, pos = rd_varint(body, 0)
    assert body[pos] == DATA_ITEM_INDEX, f"item data index {body[pos]} != {DATA_ITEM_INDEX}"
    pos += 1
    serializer, pos = rd_varint(body, pos)
    assert serializer == ITEM_STACK_SERIALIZER, f"serializer {serializer} is not ITEM_STACK"
    count, pos = rd_varint(body, pos)
    item = 0
    if count > 0:
        item, pos = rd_varint(body, pos)
        added, pos = rd_varint(body, pos)
        assert added == 0, f"a drop carries {added} component(s)"
        removed, pos = rd_varint(body, pos)
        assert removed == 0, f"a drop removes {removed} component(s)"
    assert body[pos] == 0xFF, f"item data does not terminate: {body[pos]:#x}"
    pos += 1
    assert pos == len(body), f"item data: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "item": item, "count": count}


def read_remove_entities(body):
    """remove_entities (0x4D): [varint count][varint id]..."""
    count, pos = rd_varint(body, 0)
    ids = []
    for _ in range(count):
        eid, pos = rd_varint(body, pos)
        ids.append(eid)
    assert pos == len(body), f"remove_entities: {len(body) - pos} trailing byte(s)"
    return ids


def read_take_item(body):
    """take_item_entity (0x7c): [varint item][varint player][varint amount]."""
    item, pos = rd_varint(body, 0)
    player, pos = rd_varint(body, pos)
    amount, pos = rd_varint(body, pos)
    assert pos == len(body), f"take_item_entity: {len(body) - pos} trailing byte(s)"
    return item, player, amount


def read_content(body):
    """container_set_content (0x12): [varint window][varint state][varint slots]
    [stacks...][carried stack]. Returns (window, [(item, count)])."""
    window, pos = rd_varint(body, 0)
    _, pos = rd_varint(body, pos)
    slots, pos = rd_varint(body, pos)
    out = []
    for _ in range(slots):
        count, pos = rd_varint(body, pos)
        if count == 0:
            out.append((0, 0))
            continue
        item, pos = rd_varint(body, pos)
        # See health_test.read_stack: the patch counts come first, and a
        # component value has no wire length, so only an empty patch can be
        # skipped safely here.
        added, pos = rd_varint(body, pos)
        removed, pos = rd_varint(body, pos)
        assert added == 0 and removed == 0, f"unmodelled stack component patch {added}/{removed}"
        out.append((item, count))
    return window, out


# --- the client --------------------------------------------------------------

class Bot:
    """A client that reads on a background thread (`wire_client.Reader`), so a
    drain window can never abandon a half-read frame. Keep-alives are answered
    from the thread."""

    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.reader = wire_client.Reader(self.sock, self)
        self.entity_id = None

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def start_reader(self):
        self.reader.start()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self):
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode()
              + struct.pack(">H", 0) + varint(2))
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
                raise AssertionError(f"unexpected login packet 0x{pid:02x}")
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
                    out += (varint(len(ns)) + ns.encode() + varint(len(pk)) + pk.encode()
                            + varint(len(ver)) + ver.encode())
                self.send(0x07, out)
            elif pid == 0x03:
                self.send(0x03)
                break
        while True:
            pid, _ = self.recv()
            if pid == 0x32:
                break
        for _ in range(400):
            pid, body = self.recv()
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        self.start_reader()

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.5):
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def setblock(bot, cell, block, mode=None, timeout=0.4):
    """`/setblock` one cell; returns that command's own packet window."""
    drain(bot, 0.0)  # drop anything queued from the setup before it
    x, y, z = cell
    text = f"setblock {x} {y} {z} {block}" + (f" {mode}" if mode else "")
    send_chat_command(bot, text)
    return drain(bot, timeout)


def tick_until(bot, predicate, timeout=10.0, step=0.05):
    """Drain while sending one `flying` packet per iteration.

    A real client sends one of these every tick, and the server's survival clock
    is driven by received packets (`tick_survival_on_packet`): a bot that stands
    still sends nothing, so `step_survival` — and with it the item-pickup drain —
    never runs at all. This is the bot being a client, not a clock hack.
    """
    seen = []
    end = time.time() + timeout
    while time.time() < end:
        bot.send(SB_FLYING, bytes([1]))
        seen += drain(bot, step)
        if predicate(seen):
            break
    return seen


def wait_for(bot, predicate, timeout=8.0, step=0.25):
    """Drain in short windows until `predicate(seen)` is truthy (or time out).
    `seen` is every packet the session has produced so far, in order."""
    seen = []
    end = time.time() + timeout
    while time.time() < end:
        seen += drain(bot, step)
        if predicate(seen):
            return seen
    return seen


START = time.time()


def step(text):
    print(f"[{time.time() - START:6.2f}s] {text}", flush=True)


def wait_listening(proc, timeout=20):
    deadline = time.time() + timeout
    port = None
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
            m = re.search(r"listening on.*:(\d+)", line)
            if m:
                port = int(m.group(1))
                break

    def pump():
        for raw in iter(proc.stdout.readline, b""):
            sys.stderr.write(raw.decode(errors="replace"))

    threading.Thread(target=pump, daemon=True).start()
    if port is None:
        raise RuntimeError("server did not start")
    for _ in range(30):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            s.close()
            break
        except OSError:
            time.sleep(0.1)
    return port


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    world = tempfile.mkdtemp(prefix="plusmc-itempickup-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"

    proc = None
    try:
        step(f"world dir: {world}")
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "item pickup test",
             "--offline-mode", "--gamemode", "1", "--data", data_root,
             "--world", world, "--no-flat-world"], wait_listening, env=env)
        step(f"server listening on {port}")

        bot = Bot("ItemBot", port)
        wire_client.record(bot, note="item-pickup: drops falling, merging and being collected")
        bot.join()
        bot.start_reader()
        packets = drain(bot, 1.0)
        for body in all_of(packets, CB_UPDATE_ATTRIBUTES):
            bot.entity_id, _ = rd_varint(body, 0)
        assert bot.entity_id is not None, "the join must send update_attributes"
        bot.move_to(*BOT_HOME)
        time.sleep(0.2)

        # A 6x6 stone platform at y = 63, so the drop's cell (and the bot's own)
        # stands on something, plus the cup's walls. The world is empty
        # otherwise, and nothing here is measured: the burst goes out
        # unverified and one drain waits for it.
        for cell in PLATFORM + CUP_WALLS:
            send_chat_command(bot, f"setblock {cell[0]} {cell[1]} {cell[2]} stone")
        drain(bot, 1.0)
        setblock(bot, DROP_CELL, "dirt")
        step("-> platform placed; the drop's cell holds dirt")

        # --- 1/2. a drop is a real entity, and it falls ------------------------
        window = setblock(bot, DROP_CELL, "air", mode="destroy")
        spawns = [read_spawn(b) for b in all_of(window, CB_SPAWN_ENTITY)]
        drops = [s for s in spawns if s["type"] == ET_ITEM]
        assert len(drops) == 1, (
            f"the destroy spawned {len(drops)} item entities, not one: "
            f"{[(s['type'], s['x'], s['y'], s['z']) for s in spawns]}")
        drop = drops[0]
        assert abs(drop["x"] - DROP_CENTRE[0]) < 1e-9 and \
               abs(drop["z"] - DROP_CENTRE[2]) < 1e-9, (
            f"the drop must spawn at the block's centre, got "
            f"({drop['x']}, {drop['y']}, {drop['z']})")
        data = [read_item_data(b) for b in all_of(window, CB_SET_ENTITY_DATA)
                if rd_varint(b, 0)[0] == drop["entity"]]
        assert data, "the drop's Set Entity Data never arrived"
        assert data[0]["item"] == ITEM_DIRT and data[0]["count"] == 1, (
            f"the drop is {data[0]}, not one dirt")
        step(f"  OK drop {drop['entity']}: an Add Entity of type {ET_ITEM} at the "
             f"block's centre, carrying one dirt")

        # It falls: a position sync is the only packet that can show a
        # server-side body ticking. y = 64.0 (the platform's top face) is the
        # last place a *falling* drop can stop.
        seen = wait_for(
            bot,
            lambda pk: any(read_position_sync(b)["entity"] == drop["entity"]
                           and read_position_sync(b)["ground"]
                           and abs(read_position_sync(b)["y"] - STAND_Y) < 1e-9
                           for b in all_of(pk, CB_ENTITY_POSITION_SYNC)),
            timeout=8.0)
        syncs = [read_position_sync(b) for b in all_of(seen, CB_ENTITY_POSITION_SYNC)
                 if read_position_sync(b)["entity"] == drop["entity"]]
        assert syncs, "the drop never got a position sync (it is not being ticked)"
        assert len(syncs) >= 2, f"only {len(syncs)} position sync(s): nothing fell"
        landed = [s for s in syncs if s["ground"]][-1]
        assert abs(landed["y"] - STAND_Y) < 1e-9, (
            f"the drop came to rest at y={landed['y']}, not on the platform's top "
            f"face ({STAND_Y})")
        step(f"  OK it fell through {len(syncs)} syncs and rests on the floor at y={landed['y']}")

        # --- 3. a second drop merges --------------------------------------
        setblock(bot, DROP_CELL, "dirt")
        window = setblock(bot, DROP_CELL, "air", mode="destroy")
        second = [read_spawn(b) for b in all_of(window, CB_SPAWN_ENTITY)
                  if read_spawn(b)["type"] == ET_ITEM]
        assert len(second) == 1, f"the second destroy spawned {len(second)} items"
        second_id = second[0]["entity"]
        assert second_id != drop["entity"], "the two drops share an entity id"

        # The merge: the survivor's DATA_ITEM goes to 2 and the absorbed id is
        # despawned. Both packets come from `ItemEntity.merge`, and neither
        # existed before this change.
        seen += wait_for(
            bot,
            lambda pk: any(read_item_data(b)["count"] == 2
                           for b in all_of(pk, CB_SET_ENTITY_DATA))
            and any(len(read_remove_entities(b)) >= 1 for b in all_of(pk, CB_REMOVE_ENTITIES)),
            timeout=8.0)
        merged = [read_item_data(b) for b in all_of(seen, CB_SET_ENTITY_DATA)
                  if read_item_data(b)["count"] == 2]
        assert merged, "the two drops never merged (no count-2 item data)"
        survivor = merged[0]["entity"]
        assert survivor in (drop["entity"], second_id)
        gone = [eid for b in all_of(seen, CB_REMOVE_ENTITIES) for eid in read_remove_entities(b)]
        absorbed = second_id if survivor == drop["entity"] else drop["entity"]
        assert absorbed in gone, f"the absorbed drop {absorbed} was never despawned: {gone}"
        step(f"  OK the pair merged into {survivor} (count 2) and {absorbed} was despawned")

        # --- 4. the negative control: nothing is taken from three away -----
        # Everything seen so far is the whole session, and the pickup is the
        # one thing that must not have happened yet: the bot's centre is 4.24
        # blocks from the drop's, and `Player.aiStep`'s touch box reaches 1.3
        # per axis (a drop 1.41 away on x and z is *not* reachable).
        takes = [read_take_item(b) for b in all_of(seen, CB_TAKE_ITEM_ENTITY)]
        assert not takes, f"a drop was collected from far away: {takes}"
        step("  OK nothing was collected while the bot stood three blocks away")

        # --- 5. walking into the drop collects it ------------------------------
        bot.move_to(*BOT_AT_DROP)
        seen = tick_until(
            bot,
            lambda pk: any(item in (drop["entity"], second_id)
                           for item, _, _ in [read_take_item(b) for b in all_of(pk, CB_TAKE_ITEM_ENTITY)]),
            timeout=10.0)
        takes = [read_take_item(b) for b in all_of(seen, CB_TAKE_ITEM_ENTITY)]
        assert takes, "walking into the drop collected nothing"
        item, player, amount = takes[0]
        assert player == bot.entity_id, f"the take names player {player}, not the bot"
        assert item in (drop["entity"], second_id), f"the take names a stranger: {item}"
        assert amount == 2, f"the take amount is {amount}, not the merged stack's 2"
        step(f"  OK take_item_entity: item {item} taken by {player}, amount {amount}")

        # ...and the stack is in the bot's own menu: `Inventory.add` takes the
        # selected hotbar slot when it holds the item and the first empty slot
        # otherwise (here hotbar 0, menu slot 36).
        #
        # The take packet and the menu resync are two frames the server writes in
        # the same tick, but they are *two* frames and a drain window is a fixed
        # slice of wall time: the window that caught the take can close between
        # them, and on a 2-vCPU guest it does about half the time (on the host it
        # never does). Waiting for the resync is the difference between asserting
        # what the server sends and asserting which read window it landed in, so
        # keep ticking — the same one `flying` per iteration a real client sends
        # — until it arrives. The assertion below is unchanged.
        seen += tick_until(
            bot,
            lambda pk: any(window == 0 and len(slots) == PLAYER_MENU_SIZE
                           for window, slots
                           in [read_content(b) for b in all_of(pk, CB_CONTAINER_SET_CONTENT)]),
            timeout=10.0)
        content = [read_content(b) for b in all_of(seen, CB_CONTAINER_SET_CONTENT)]
        tables = [slots for window, slots in content if window == 0 and len(slots) == PLAYER_MENU_SIZE]
        assert tables, "no player-menu container_set_content arrived after the take"
        assert tables[-1][MENU_HOTBAR_FIRST] == (ITEM_DIRT, 2), (
            f"hotbar 0 holds {tables[-1][MENU_HOTBAR_FIRST]}, not two dirt")
        step("  OK the dirt is in the bot's inventory (hotbar 0, count 2)")

        # The take packet *is* the removal: the server must not also despawn the
        # entity, or the client would replay an animation for an id it already
        # dropped.
        final = drain(bot, 0.5)
        despawns = [read_remove_entities(b) for b in all_of(final, CB_REMOVE_ENTITIES)]
        assert not any(item in ids for ids in despawns), (
            f"the taken drop was despawned as well: {despawns}")
        step("  OK the taken drop was removed by the take packet alone")

        # --- 6. a creative drop throws a real entity --------------------------
        # Slot -1 is the creative screen's "throw this stack out". Vanilla's
        # `handleSetCreativeModeSlot` calls `player.drop(stack, true)` inside the
        # `hasInfiniteMaterials` gate, so the drop starts at the eye height less
        # 0.3 and carries the stack it was handed — three dirt, not a block's
        # one — while the negative slot writes nothing to the inventory.
        drop_at = (BOT_AT_DROP[0], BOT_AT_DROP[1] + 1.62 - 0.3, BOT_AT_DROP[2])
        thrown = (varint(3) + varint(ITEM_DIRT) + b"\x00\x00")
        bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", -1) + thrown)
        window = drain(bot, 1.0)
        spawns = [read_spawn(b) for b in all_of(window, CB_SPAWN_ENTITY)]
        drops = [s for s in spawns if s["type"] == ET_ITEM]
        assert len(drops) == 1, (
            f"the creative drop spawned {len(drops)} item entities, not one: "
            f"{[(s['type'], s['x'], s['y'], s['z']) for s in spawns]}")
        creative_drop = drops[0]
        assert abs(creative_drop["x"] - drop_at[0]) < 1e-9 and \
               abs(creative_drop["y"] - drop_at[1]) < 1e-9 and \
               abs(creative_drop["z"] - drop_at[2]) < 1e-9, (
            f"the creative drop must spawn at the eye height less 0.3 "
            f"({drop_at}), got ({creative_drop['x']}, {creative_drop['y']}, "
            f"{creative_drop['z']})")
        data = [read_item_data(b) for b in all_of(window, CB_SET_ENTITY_DATA)
                if rd_varint(b, 0)[0] == creative_drop["entity"]]
        assert data, "the creative drop's Set Entity Data never arrived"
        assert data[0]["item"] == ITEM_DIRT and data[0]["count"] == 3, (
            f"the creative drop is {data[0]}, not three dirt")
        assert not all_of(window, CB_CONTAINER_SET_SLOT), (
            "a negative creative slot wrote the inventory: container_set_slot "
            "must not follow a throw")
        step(f"  OK set_creative_slot -1 spawned item {creative_drop['entity']} "
             "at the eye height, carrying all three dirt, and wrote no slot")

        # It falls to the platform like any other drop. Step away first: the
        # pickup delay is 40 ticks and the bot is standing where it spawned.
        #
        # The fall can finish inside the window above (it is 1.32 blocks of
        # travel), so the landed sync has to be looked for in *that* window too —
        # a fresh one would only see a drop already at rest, which sends nothing.
        bot.move_to(*BOT_HOME)
        seen = list(window)
        syncs = []
        deadline = time.time() + 8.0
        while time.time() < deadline:
            syncs = [read_position_sync(b) for b in all_of(seen, CB_ENTITY_POSITION_SYNC)
                     if read_position_sync(b)["entity"] == creative_drop["entity"]]
            if syncs and syncs[-1]["ground"]:
                break
            seen += drain(bot, 0.25)
        assert syncs, "the creative drop never got a position sync (it is not being ticked)"
        assert syncs[-1]["ground"], "the creative drop never landed"
        assert abs(syncs[-1]["y"] - STAND_Y) < 1e-9, (
            f"the creative drop rests at y={syncs[-1]['y']}, not on the platform "
            f"({STAND_Y})")
        step(f"  OK it fell and rests on the floor at y={syncs[-1]['y']}")

        bot.close()
        print("item pickup: all steps passed")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
