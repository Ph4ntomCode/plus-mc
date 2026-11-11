#!/usr/bin/env python3
"""Integration test: weather, rain and precipitation over the wire.

Weather exists so three things that were unreachable become reachable:

  * `Level.isRaining` itself, and the two `Game Event` packets a client needs
    to render it (`START_RAINING` / `STOP_RAINING` plus the rain and thunder
    *levels* that ramp 0.01 a tick, which is why a flip takes a second to show);
  * `FireBlock.tick`'s two rain branches — the `isNearRain` douse, and the gate
    that keeps a spread from starting in a cell it is raining on;
  * `CauldronBlock.handlePrecipitation`, which `tickChunk`'s one-in-48 sample
    reaches.

The world is the committed flat fixture, so the spawn chunk really is plains
(the in-memory world advertises `minecraft:the_void`, a biome that does not
precipitate, and rain there would prove nothing).

What the test does, and what each step can only be explained by:

  1. joins into clear weather and sees no game-event packet at all (vanilla
     sends none when the sky is clear);
  2. lays two identical 3x3 hay platforms, lights all nine top cells of the
     first, steps clear, and watches fire appear in a cell it never lit — the
     spread, which is the *control* for step 5: it proves the block-tick loop is
     running and that this layout spreads in this world;
  3. standing in a soul fire of its own in survival, the player catches fire
     (the shared flags' on-fire bit, which is what the client draws);
  4. `/weather rain` -> the level events and `START_RAINING`, and the burning
     player is put out (`isInRain` -> `clearFire`) a handful of packets after the
     command, far sooner than the 160-tick burn the same flags would run out on
     their own;
  5. lights the second platform in the rain, and watches for as long as step 2
     took: no cell it did not light ever holds fire, and the nine it did light
     are put out. Only the rain separates the two platforms.
  6. `/weather clear` -> `STOP_RAINING`;
  7. `/weather rain` again, lays a 3x3 field of empty cauldrons, and watches one
     of them become `water_cauldron[level=1]` — the precipitation sample landing
     on the block below the column's motion-blocking height;
  8. a second client joins into the storm and is told so during its join.

Two honest notes. The removal in step 5 is consistent with the douse but the
fuel burning away can also remove a fire, so the *branch* that only rain can
explain is the spread gate (step 2 against step 5); the douse's own draw is what
the unit test pins exactly. And `PLUSMC_RANDOM_TICK_SPEED` is set well above
vanilla's 3 because vanilla's one-in-48 sample means a cauldron takes about an
hour of in-game rain to fill — the rules are untouched, only the sample rate.
`PLUSMC_TICK_SCALE` compresses the clock the same way, so a fire's 30..40-tick
delay and the hundred-tick level ramp pass in a couple of real ticks.

This is a *world*-clock test, and the server additionally counts packets for the
player's survival tick (`PLUSMC_SURVIVAL_CLOCK=packet`) so that the burning
countdown cannot drift with wall time behind the state claims below. The claims
are states — alight, put out, still alight — read from everything the client has
been told rather than from one window, and the loops are capped-until rather
than fixed-count on purpose (see step 3's note): what a barrier bounds is a
survival-tick count, and the durations this test measures (the rain ramp, a
fire's spread delay, the burn's 160 ticks) are the world's.

Run: weather_test.py /path/to/plusmc
"""
import os, re, shutil, socket, struct, subprocess, sys, tempfile, threading, time
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

# Item ids (src/world/item_registry.h).
ITEM_SOUL_SOIL = 435
ITEM_HAY_BLOCK = 579
ITEM_FLINT_AND_STEEL = 1005
ITEM_CAULDRON = 1277

# Block state ids (src/world/block_states.h).
STATE_AIR = 0
# `minecraft:fire` is 512 states, age=15 down to age=0 (4931..5442); any of them
# can be a spread's result depending on which neighbours burn where it lands.
FIRE_STATE_MIN = 4931
FIRE_STATE_MAX = 5442
STATE_CAULDRON = 11194
STATE_WATER_CAULDRON_L1 = 11195  # `water_cauldron[level=1]`
# `minecraft:soul_fire` is a singleton state: a fire with no tick of its own, so
# it neither ages out nor is doused — which is what makes the player's own rain
# extinguish (below) the only thing that can put them out.
STATE_SOUL_FIRE = 5443
STATE_SOUL_SOIL = 8669

# Game events (src/protocol/ids.h).
GE_START_RAINING = 1
GE_STOP_RAINING = 2
GE_RAIN_LEVEL_CHANGE = 7
GE_THUNDER_LEVEL_CHANGE = 8

# Clientbound packet ids.
CB_BLOCK_UPDATE = 0x08
CB_GAME_EVENT = 0x27
CB_SET_ENTITY_DATA = 0x65
CB_KEEP_ALIVE = 0x2D
CB_PLAYER_POSITION = 0x49

# Serverbound packet ids.
SB_CHAT_COMMAND = 0x07
SB_FLYING = 0x21
SB_POSITION = 0x1E
SB_SET_CARRIED_ITEM = 0x36
SB_SET_CREATIVE_SLOT = 0x39
SB_USE_ITEM_ON = 0x42
SB_KEEP_ALIVE = 0x1C

# Entity data: the shared-flags accessor and its on-fire bit (entity_packets.h).
SHARED_FLAGS_INDEX = 0
FLAG_ON_FIRE = 0x01
SER_BYTE = 0
SER_FLOAT = 3
SER_PARTICLES = 17
# minecraft:entity_effect, the one particle type an effect's particle list
# carries a payload for (the ARGB colour).
PARTICLE_ENTITY_EFFECT = 28

# The fixture world's flat floor: bedrock at -64, then dirt and grass, so the
# surface is grass at -61. The platforms go on top of it, one block of hay at
# -60 and the fires in the cells above at -59.
GROUND_Y = -61
HAY_Y = -60
FIRE_Y = -59
# Both platforms and the cauldron field live inside chunk (0,0), which is the one
# chunk the fixture carries — and the only one whose biome is really plains.
PLATFORM_A = [(x, z) for x in range(4, 7) for z in range(4, 7)]
PLATFORM_B = [(x, z) for x in range(10, 13) for z in range(4, 7)]
CAULDRONS = [(x, z) for x in range(5, 8) for z in range(9, 12)]
A_CENTER = (5.5, float(FIRE_Y), 5.5)
B_CENTER = (11.5, float(FIRE_Y), 5.5)
CAULDRON_CENTER = (6.5, float(FIRE_Y), 10.5)
# A cell of its own for the soul fire the player burns in, clear of both
# platforms and of the cauldron field (so nothing else can ever be in it but the
# soul soil and the soul fire).
SOLO = (2, 2)
SOLO_CENTER = (2.5, float(FIRE_Y), 2.5)
# One cell east of the soul fire: where the player stands once it has caught, so
# the burn they are observed carrying is the countdown the catch started rather
# than a soul fire's 2.0 a hit (which would kill a 20-health player inside the
# few seconds the rain step spans).
SOLO_STAND = (3.5, float(FIRE_Y), 2.5)

# Menu slots: hotbar slot 0 is menu slot 36.
SLOT_MENU_HOTBAR_FIRST = 36
SLOT_HAY = SLOT_MENU_HOTBAR_FIRST
SLOT_FLINT = SLOT_MENU_HOTBAR_FIRST + 1
SLOT_CAULDRON = SLOT_MENU_HOTBAR_FIRST + 2
SLOT_SOUL_SOIL = SLOT_MENU_HOTBAR_FIRST + 3

# `randomTickSpeed`: 192 draws a tick make the one-in-48 sample land on a
# cauldron often enough to watch (see the docstring).
RANDOM_TICK_SPEED = 192
# 20 game ticks per real tick, so a 30..40-tick fire delay passes in ~100 ms.
TICK_SCALE = 20
# How long to watch the control platform spread, and then the rained-on one for
# the same time.
SPREAD_SECONDS = 8.0
CAULDRON_SECONDS = 20.0
TICK_MS = 0.055


# --- framing ----------------------------------------------------------------

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


def position(x, y, z):
    packed = (((x & 0x3FFFFFF) << 38) | ((z & 0x3FFFFFF) << 12) | (y & 0xFFF)) & 0xFFFFFFFFFFFFFFFF
    return struct.pack(">Q", packed)


def unpack_position(raw):
    x = raw >> 38
    z = (raw >> 12) & 0x3FFFFFF
    y = raw & 0xFFF
    if x >= 0x2000000:
        x -= 0x4000000
    if z >= 0x2000000:
        z -= 0x4000000
    if y >= 0x800:
        y -= 0x1000
    return x, y, z


# --- readers ----------------------------------------------------------------

def read_block_update(body):
    """block_update: [i64 packed position][varint state id]."""
    raw = struct.unpack(">q", body[0:8])[0]
    state, pos = rd_varint(body, 8)
    assert pos == len(body), f"block_update: {len(body) - pos} trailing byte(s)"
    return unpack_position(raw), state


def read_game_event(body):
    """game_event: [u8 event][f32 value]."""
    assert len(body) == 5, f"game_event: {len(body)} byte(s)"
    return body[0], round(struct.unpack(">f", body[1:5])[0], 4)


def read_shared_flags(body, wanted):
    """Set Entity Data for *one* entity: the BYTE accessors this test watches.
    Another entity's packet returns None before anything is read, so the
    packet's other serializers never reach this decoder — and the FLOAT arm
    below is skipped rather than recorded, because the model's own stream also
    carries its absorption word (`Player.DATA_PLAYER_ABSORPTION_ID`, index 17;
    this test grants itself an Absorption effect to survive the fire) and the
    particle list that effect is drawn from
    (`LivingEntity.DATA_EFFECT_PARTICLES`, index 10)."""
    eid, pos = rd_varint(body, 0)
    if eid != wanted:
        return None
    values = {}
    while True:
        index = body[pos]
        pos += 1
        if index == 0xFF:
            break
        serializer, pos = rd_varint(body, pos)
        if serializer == SER_BYTE:
            values[index] = body[pos]
            pos += 1
        elif serializer == SER_FLOAT:
            pos += 4
        elif serializer == SER_PARTICLES:
            count, pos = rd_varint(body, pos)
            for _ in range(count):
                ptype, pos = rd_varint(body, pos)
                if ptype == PARTICLE_ENTITY_EFFECT:
                    pos += 4
        else:
            raise AssertionError(f"entity data index {index}: serializer {serializer}")
    assert pos == len(body), f"set_entity_data: {len(body) - pos} trailing byte(s)"
    return values


# --- the client -------------------------------------------------------------

class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(10)
        self.threshold = -1
        self.entity_id = None
        self.position = None
        self.join_packets = []
        # Once the join is done a background thread owns the socket's read side
        # and `drain` collects from its queue. Reading only inside a drain window
        # *loses* packets: this server runs the clock twenty times over
        # (`PLUSMC_TICK_SCALE`) with a burning platform under it, so a window can
        # land entirely between two bursts and the packets that came back before
        # it were dropped on the floor. The reader also answers keep-alives from
        # wherever they arrive, which a windowed drain can miss the same way.
        # See tests/wire_client.py for why it reads whole frames and nothing else.
        self.reader = wire_client.Reader(self.sock, self)

    def send(self, pid, body=b""):
        self.reader.send(pid, body)

    def recv(self):
        pid, body = read_packet(self.sock, self.threshold)
        if pid == CB_PLAYER_POSITION:
            self.position = struct.unpack(">ddd", body[0:24])
        return pid, body

    def start_reader(self):
        """Hand the read side to a thread, once the join's own reads are done."""
        self.reader.start()

    def close(self):
        self.reader.stop()
        try:
            self.sock.close()
        except OSError:
            pass

    def join(self):
        """Login, configuration and the spawn burst, keeping every packet seen
        (the join is where a storm is announced to a fresh client)."""
        seen = self.join_packets
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
            seen.append((pid, body))
            if pid == CB_KEEP_ALIVE:
                self.send(SB_KEEP_ALIVE, body[:8])
            elif pid == 0x0B:
                batch, _ = rd_varint(body)
                self.send(0x0B, varint(batch))
                break
        return seen

    def move_to(self, x, y, z, ground=1):
        self.send(SB_POSITION, struct.pack(">ddd", x, y, z) + bytes([ground]))


def drain(bot, timeout=1.0):
    """Every packet the reader thread has queued in the next `timeout` seconds.
    Before the reader starts (the join itself) the socket is read directly."""
    # A reply window ends when the server has been quiet for `quiet` seconds
    # rather than always burning the whole window; `timeout` stays the bound a
    # missing reply is judged against. Requires the reader (started after each
    # join below).
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)


def all_of(packets, pid):
    return [b for p, b in packets if p == pid]


def game_events(packets):
    return [read_game_event(b) for b in all_of(packets, CB_GAME_EVENT)]


def block_updates(packets):
    return [read_block_update(b) for b in all_of(packets, CB_BLOCK_UPDATE)]


def read_damage_event(body):
    """damage_event: [varint entity][varint type][varint cause+1]
    [varint direct+1][bool has position][3 doubles]."""
    pos = 0
    eid, pos = rd_varint(body, pos)
    type_id, pos = rd_varint(body, pos)
    cause, pos = rd_varint(body, pos)
    direct, pos = rd_varint(body, pos)
    has_position = body[pos]
    pos += 1
    if has_position:
        pos += 24
    assert pos == len(body), f"damage_event: {len(body) - pos} trailing byte(s)"
    return {"entity": eid, "type": type_id, "cause": cause - 1, "direct": direct - 1}


# `minecraft:in_fire` and `minecraft:on_fire` in kDamageTypes (world/damage_table.h).
DMG_IN_FIRE = 21
DMG_ON_FIRE = 31


# Clientbound damage_event.
CB_DAMAGE_EVENT = 0x19


def on_fire_bits(packets, entity_id):
    """Every shared-flags byte this batch carried for `entity_id`."""
    bits = []
    for body in all_of(packets, CB_SET_ENTITY_DATA):
        read = read_shared_flags(body, entity_id)
        if read is not None and SHARED_FLAGS_INDEX in read:
            bits.append(read[SHARED_FLAGS_INDEX])
    return bits


def use_item_on(bot, x, y, z, face, seq, cursor=(0.5, 0.5, 0.5)):
    """ServerboundUseItemOn: [varint hand][i64 position][varint face][3 f32
    cursor][bool inside][bool border][varint sequence]. The cursor is checked
    against the block's *centre*, so a half-block offset is on it."""
    bot.send(SB_USE_ITEM_ON,
             varint(0) + position(x, y, z) + varint(face)
             + struct.pack(">fff", *cursor) + bytes([0, 0]) + varint(seq))


def select(bot, menu_slot):
    bot.send(SB_SET_CARRIED_ITEM, struct.pack(">h", menu_slot - SLOT_MENU_HOTBAR_FIRST))
    drain(bot, 0.2)


def creative_item(bot, menu_slot, item_id, count=64):
    bot.send(SB_SET_CREATIVE_SLOT, struct.pack(">h", menu_slot) + varint(count)
             + varint(item_id) + b"\x00\x00")
    drain(bot, 0.2)


def send_chat_command(bot, command):
    bot.send(SB_CHAT_COMMAND, varint(len(command)) + command.encode())


def tick(bot, count):
    """Advance the survival clock by exactly `count` game ticks (one `flying`
    packet each) and give the world clock `count` packets' worth of real time.

    `PLUSMC_SURVIVAL_CLOCK=packet` is what makes the first half true: the tick
    the burning countdown rides counts packets, not banked wall time. The sleep
    is for the second half, which no packet can drive — the weather's level
    ramp, the random-tick pass that spreads a fire and fills a cauldron, and the
    block-tick loop all run on the server's own 50 ms clock with
    `PLUSMC_TICK_SCALE` multiplying it.
    """
    for _ in range(count):
        bot.send(SB_FLYING, bytes([1]))
        time.sleep(TICK_MS)


def is_fire(state):
    return FIRE_STATE_MIN <= state <= FIRE_STATE_MAX


def ring_of(platform):
    """The cells at the platform's own level around it: the only cells a spread
    can land in (an empty cell whose own neighbour is flammable)."""
    xs = [x for x, _ in platform]
    zs = [z for _, z in platform]
    return {(x, HAY_Y, z)
            for x in range(min(xs) - 1, max(xs) + 2)
            for z in range(min(zs) - 1, max(zs) + 2)} - {(x, HAY_Y, z) for x, z in platform}


def lit_cells(platform):
    return {(x, FIRE_Y, z) for x, z in platform}


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


class Session:
    """A bot plus the running record of what the client has seen, so the two
    phases can be compared cell by cell."""

    def __init__(self, bot):
        self.bot = bot
        self.hay_state = None
        self.last_state = {}     # cell -> the last state the client was sent
        self.fire_cells = set()  # every cell that has ever held fire
        self.fire_bits = []      # every on-fire shared-flags byte, in order
        self.seq = 1

    def observe(self, packets):
        # The player's on-fire shared flag is a *state*, published only when it
        # changes, so a step that wants to assert on it has to have been reading
        # for the whole story rather than from the moment it started looking:
        # every byte the client was told is kept here, in order, and a step reads
        # the tail of it.
        eid = getattr(self.bot, "entity_id", None)
        if eid is not None:
            self.fire_bits += on_fire_bits(packets, eid)
        return self.observe_updates(block_updates(packets))

    def observe_updates(self, updates):
        for at, state in updates:
            self.last_state[at] = state
            if is_fire(state):
                self.fire_cells.add(at)
        return updates

    def command(self, text, timeout=1.2):
        """The command's own packets, with the block updates folded into the
        session's bookkeeping (`observe` returns the *updates*, not the raw
        packets, so a caller that wants the game events has to keep them)."""
        step(f"-> /{text}")
        send_chat_command(self.bot, text)
        packets = drain(self.bot, timeout)
        self.observe(packets)
        return packets

    def move_to(self, xyz):
        self.bot.move_to(*xyz)
        time.sleep(0.2)

    def lay_cells(self, cells, click_y):
        """Click each cell's up face at `click_y`, which places one block in
        the cell above it."""
        for x, z in cells:
            use_item_on(self.bot, x, click_y, z, 1, self.seq)
            self.seq += 1

    def lay(self, platform, item_slot):
        """A 3x3 hay platform on the ground: every click lands one block above
        the floor cell it clicks."""
        select(self.bot, item_slot)
        self.lay_cells(platform, GROUND_Y)
        updates = self.observe_updates(block_updates(drain(self.bot, 1.5)))
        ats = {at for at, _ in updates}
        expect = {(x, HAY_Y, z) for x, z in platform}
        assert ats == expect, f"the platform did not land: {sorted(ats ^ expect)}"
        states = {state for _, state in updates}
        assert len(states) == 1, f"the platform is not one state: {states}"
        self.hay_state = states.pop()
        return updates

    def light(self, platform):
        """Flint and steel on every hay cell: fire in the cell above each."""
        select(self.bot, SLOT_FLINT)
        self.lay_cells(platform, HAY_Y)
        updates = self.observe_updates(block_updates(drain(self.bot, 1.5)))
        lit = lit_cells(platform)
        saw = {at for at, state in updates if at in lit and is_fire(state)}
        assert saw == lit, f"not every cell was lit: {sorted(saw)} of {sorted(lit)}"
        return updates


def watch_for(session, seconds, done, slice_seconds=0.3):
    """Drain until `done()` or the deadline; returns everything seen."""
    seen = []
    deadline = time.time() + seconds
    while time.time() < deadline and not done():
        packet_slice = drain(session.bot, slice_seconds)
        session.observe(packet_slice)
        seen += packet_slice
    return seen


def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    fixtures = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")
    world = tempfile.mkdtemp(prefix="plusmc-weather-")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_TICK_SCALE"] = str(TICK_SCALE)
    env["PLUSMC_RANDOM_TICK_SPEED"] = str(RANDOM_TICK_SPEED)
    # The weather, the fire spread and the cauldron fills are the world's clock,
    # which this leaves alone; pinning the player's own tick to packets is what
    # keeps the burning countdown out of the load's hands.
    env["PLUSMC_SURVIVAL_CLOCK"] = "packet"

    proc = None
    try:
        # The flat fixture, so the spawn chunk is plains with a real surface for
        # the platforms to stand on (`world_join_test`'s own layout).
        region_dir = os.path.join(world, "dimensions", "minecraft", "overworld", "region")
        os.makedirs(region_dir, exist_ok=True)
        shutil.copyfile(os.path.join(fixtures, "r.0.0.mca"),
                        os.path.join(region_dir, "r.0.0.mca"))
        step(f"world dir: {world} (flat fixture: chunk (0,0) is plains)")

        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "weather test",
             "--offline-mode", "--gamemode", "1", "--data", data_root, "--world", world],
            wait_listening, env=env)

        bot = Bot("WeatherBot", port)
        wire_client.record(bot, note="weather-wire: /weather, the game events, rain on fire")
        join_packets = bot.join()
        bot.start_reader()
        join_packets += drain(bot, 1.2)
        # The join's own game events (the game-mode change and the level-chunk
        # load start) are not weather: a clear sky sends none of the four.
        weather_events = [e for e in game_events(join_packets)
                          if e[0] in (GE_START_RAINING, GE_STOP_RAINING, GE_RAIN_LEVEL_CHANGE,
                                      GE_THUNDER_LEVEL_CHANGE)]
        assert not weather_events, f"a clear sky sends no rain event: {weather_events}"
        session = Session(bot)
        attrs = all_of(join_packets, 0x86)  # update_attributes
        assert attrs, "the join must push the attribute set (which carries the entity id)"
        bot.entity_id, _ = rd_varint(attrs[-1], 0)
        step("  OK joined into clear weather (not a single game-event packet)")

        # --- 1. two identical platforms, laid in creative ---------------------
        creative_item(bot, SLOT_HAY, ITEM_HAY_BLOCK)
        creative_item(bot, SLOT_FLINT, ITEM_FLINT_AND_STEEL, count=1)
        creative_item(bot, SLOT_CAULDRON, ITEM_CAULDRON)
        creative_item(bot, SLOT_SOUL_SOIL, ITEM_SOUL_SOIL)
        session.move_to(A_CENTER)
        session.lay(PLATFORM_A, SLOT_HAY)
        assert session.hay_state is not None, "the hay state must be readable"
        hay_state = session.hay_state
        session.move_to(B_CENTER)
        session.lay(PLATFORM_B, SLOT_HAY)
        step(f"  OK two 3x3 hay platforms at y={HAY_Y} (state {hay_state})")

        # Survival, so the fire can actually burn the player (a creative player
        # is invulnerable and would never catch).
        session.command("gamemode survival WeatherBot")

        # --- 2. the control: a lit platform spreads while the sky is clear ----
        session.move_to(A_CENTER)
        session.light(PLATFORM_A)
        ring_a = ring_of(PLATFORM_A)
        # Step away before the watch: the bot lit the cell it was standing in (and
        # the spread reaches the ring), and this is a *survival* player — left
        # there it would burn through the control stretch and, some seconds in,
        # die, after which the server announces the death and drops every later
        # damage. The burn this test is about belongs in the soul fire of step 3.
        session.move_to(SOLO_CENTER)
        step(f"  OK lit all 9 cells of platform A; watching {len(ring_a)} ring cells")

        seen = watch_for(session, SPREAD_SECONDS, lambda: bool(session.fire_cells & ring_a))
        spread = session.fire_cells & ring_a
        assert spread, (
            f"the control platform never spread in {SPREAD_SECONDS}s: "
            f"fire only at {sorted(session.fire_cells)}")
        step(f"  OK (clear) the fire spread into {sorted(spread)} — the block-tick loop runs")

        # --- 3. the player standing in it catches fire ------------------------
        # A soul fire in a cell of its own (`SOLO`), away from both platforms: lit
        # on soul soil, so `fire_placement_state` picks soul fire — a block with no
        # tick of its own, which neither ages out (that is what would end an
        # ordinary fire in a fraction of a second at this clock scale) nor is
        # doused by the rain later. Whatever ends the player's burn, then, is not
        # the block going anywhere. A cell of its own because the platform's
        # middle cell is already burning — `BaseFireBlock.canBePlacedAt` wants air,
        # so fire cannot be re-lit where fire already is.
        #
        # Absorption first. A soul fire deals `BaseFireBlock.fireDamage` — 2.0 —
        # on *every* tick the player's box is inside it
        # (`BaseFireBlock.entityInside` -> `runAfter(FIRE_IGNITE, hurt(inFire,
        # fireDamage))`, and a standing player flushes one step a tick), and the
        # catch this step observes needs the 20-tick immunity climb before the
        # flag is even set. Soul fire is here because it never goes out on its
        # own, not because it is survivable: the window is already worth more
        # than a 20-health player. The absorption hearts soak the blows *in
        # front of* the bar, so the bot lives long enough to be observed without
        # changing what is asserted — the `in_fire` events still fire and the
        # shared flag is still set by the same catch.
        session.command("effect give WeatherBot absorption 100000 9")
        session.move_to(SOLO_CENTER)
        select(bot, SLOT_SOUL_SOIL)
        use_item_on(bot, SOLO[0], GROUND_Y, SOLO[1], 1, session.seq)
        session.seq += 1
        updates = session.observe_updates(block_updates(drain(bot, 1.0)))
        assert ((SOLO[0], HAY_Y, SOLO[1]), STATE_SOUL_SOIL) in updates, updates
        select(bot, SLOT_FLINT)
        use_item_on(bot, SOLO[0], HAY_Y, SOLO[1], 1, session.seq)
        session.seq += 1
        lit_here = session.observe(drain(bot, 1.5))
        assert ((SOLO[0], FIRE_Y, SOLO[1]), STATE_SOUL_FIRE) in lit_here, lit_here
        step("  OK the bot stands in soul fire (which never goes out on its own)")

        # The survival tick (and so the burn countdown) only advances on a
        # serverbound packet: `Player`'s countdown idles at -20 and climbs one a
        # tick while the player is inside fire, so catching takes a second.
        # The claim is the *state* at the end of the step, and the state is read
        # from everything the client has been told, not just from this loop's
        # slice of it. The flag is published on change only, and the control
        # platform behind us can already have caught the player — a burn is a
        # 160-tick countdown that stepping out of the flame does not end — so
        # the transition is not reliably *in* this window even when the state is
        # right. What the flame actually does (the -20 idle countdown, the one to
        # three ticks a tick, the catch at zero) is pinned by the unit test in
        # tests/fire_test.cpp (`test_burning_countdown`); what this asserts is the
        # wire. The loop is still "until the bit is on", capped rather than
        # fixed, so a slower build is not judged on how many packets it managed.
        bits = list(session.fire_bits)
        events = []
        for _ in range(24):
            tick(bot, 12)
            slice_packets = drain(bot, 0.2)
            session.observe(slice_packets)  # keeps the block-update bookkeeping
            bits += on_fire_bits(slice_packets, bot.entity_id)
            events += [read_damage_event(b) for b in all_of(slice_packets, CB_DAMAGE_EVENT)]
            if bits and bits[-1] & FLAG_ON_FIRE:
                break
        assert any(e["type"] == DMG_IN_FIRE and e["entity"] == bot.entity_id for e in events), (
            f"standing in fire must hurt: {events}")
        assert bits and bits[-1] & FLAG_ON_FIRE, bits
        # Step out of the flame now that it has caught. What is left burning is
        # the 8-second countdown (160 ticks) the catch started, which is what the
        # rain has to end — and the soul fire itself stays exactly where it is,
        # since a soul fire neither spreads nor goes out on its own. The cell the
        # player moves to is the same open sky one cell east.
        session.move_to(SOLO_STAND)
        step("  OK the burning player's shared flags carry the on-fire bit")

        # --- 4. /weather rain puts the player out -----------------------------
        packets = session.command("weather rain 100000")
        events = game_events(packets)
        assert events, "the rain ramp must be broadcast"
        assert all(e in (GE_RAIN_LEVEL_CHANGE, GE_THUNDER_LEVEL_CHANGE, GE_START_RAINING)
                   for e, _ in events), events
        assert events[0][0] == GE_RAIN_LEVEL_CHANGE, events[:3]
        assert any(e == GE_START_RAINING for e, _ in events), events
        levels = [v for e, v in events if e == GE_RAIN_LEVEL_CHANGE]
        # `advanceWeatherCycle`'s 0.01-a-tick ramp: with the clock scaled twenty
        # times, the whole climb to 1.0 lands inside this one drain.
        assert levels == sorted(levels) and levels[0] == 0.01, levels
        assert max(levels) > 0.2, levels
        step(f"  OK /weather rain: {len(events)} events, rain level {levels[0]}..{levels[-1]}")

        # `Entity.isInRain` -> `clearFire`: the flames go out within a tick or two
        # of the rain starting, far short of the 160-tick burn — and the soul fire
        # the player lit is still there, so the block can have had no hand in it.
        # The command's *own* packets count: at this clock scale the
        # whole level ramp lands inside that one drain, so `isInRain` is already
        # true by the time it returns and the clear can arrive with it.
        before = len(bits)
        bits += on_fire_bits(packets, bot.entity_id)
        for _ in range(6):
            if bits and not (bits[-1] & FLAG_ON_FIRE):
                break
            tick(bot, 4)
            slice_packets = drain(bot, 0.2)
            session.observe(slice_packets)
            bits += on_fire_bits(slice_packets, bot.entity_id)
        assert bits and not (bits[-1] & FLAG_ON_FIRE), (
            f"the rain must put the player out: {bits[-4:]}")
        cleared = len(bits) - before
        assert session.last_state.get((SOLO[0], FIRE_Y, SOLO[1])) == STATE_SOUL_FIRE, \
            "the soul fire stays"
        step(f"  OK the rain put the burning player out after {cleared} flag packets, "
             "while the fire block they stand in is untouched")

        # --- 5. the same fire, in the rain, does not spread --------------------
        # Platform B is fresh hay and is lit exactly as A was. From here A and B
        # differ in one thing only: whether rain reaches their cells.
        session.move_to(B_CENTER)
        session.light(PLATFORM_B)
        ring_b = ring_of(PLATFORM_B)
        lit_b = lit_cells(PLATFORM_B)
        step(f"  OK lit all 9 cells of platform B in the rain")

        # The douse is the rain's work and the clock it runs on is game time, so
        # the window is spent and then, if a cell is somehow still alight, spent
        # again: a build under a sanitizer turns over fewer ticks in the same
        # wall-clock second and can need longer than one `SPREAD_SECONDS` to
        # finish the platform off. The no-spread claim only gets *harder* with
        # the extra time — the ring is re-read on every pass.
        rained_spread = set()
        still_burning = set(lit_b)
        for _ in range(3):
            watch_for(session, SPREAD_SECONDS, lambda: False)
            rained_spread |= session.fire_cells & ring_b
            still_burning = {at for at in lit_b
                             if is_fire(session.last_state.get(at, STATE_AIR))}
            if not still_burning:
                break
        assert not rained_spread, (
            f"rain must keep the spread from starting: fire reached {sorted(rained_spread)}")
        assert not still_burning, (
            f"every lit cell of the rained-on platform is put out: {sorted(still_burning)}")
        step(f"  OK (rain) no spread in {SPREAD_SECONDS}s and all 9 lit cells are out")

        # --- 6. /weather clear ------------------------------------------------
        # The ramp down is broadcast by the weather's own clock, so the drain has
        # to outlast it: 1.2 s is a Release build's worth, and a sanitizer build
        # needs more than that to produce even the first level change.
        packets = session.command("weather clear 100000", timeout=4.0)
        events = game_events(packets)
        # The level ramp is game time, and a loaded machine (or a sanitizer build)
        # turns over far fewer ticks per wall-clock second than a release one: the
        # first window can be spent before the level has moved at all. The reader
        # keeps queueing between drains, so a second window sees whatever the
        # first missed — and in the common case the assertion is already met and
        # no extra window is spent.
        deadline = time.time() + 20.0
        while not any(e == GE_STOP_RAINING for e, _ in events) and time.time() < deadline:
            more = drain(session.bot, 2.0)
            session.observe(more)
            events += game_events(more)
        assert any(e == GE_STOP_RAINING for e, _ in events), events
        levels = [v for e, v in events if e == GE_RAIN_LEVEL_CHANGE]
        assert levels == sorted(levels, reverse=True), levels
        step(f"  OK /weather clear: STOP_RAINING and the levels ramp down to {levels[-1]}")

        # --- 7. rain fills a cauldron ----------------------------------------
        # `tickPrecipitation` samples the block below the column's
        # motion-blocking height and asks *it* for a precipitation handler, so a
        # cauldron standing at the top of its own column is exactly what the
        # sample reaches. Nine of them, so the one-in-48 sample lands often.
        session.command("weather rain 100000")
        session.move_to(CAULDRON_CENTER)
        select(bot, SLOT_CAULDRON)
        session.lay_cells(CAULDRONS, GROUND_Y)
        updates = block_updates(drain(bot, 1.5))
        ats = {at for at, _ in updates}
        assert ats == {(x, HAY_Y, z) for x, z in CAULDRONS}, sorted(ats)
        # Each cell's *first* update is the placement; a later one is the rain
        # already filling that cauldron in this same window, which is the next
        # assertion's subject.
        first = {}
        for at, state in updates:
            first.setdefault(at, state)
        assert set(first.values()) == {STATE_CAULDRON}, updates
        step(f"  OK laid {len(CAULDRONS)} empty cauldrons")

        def filled():
            return [at for at, state in session.last_state.items()
                    if at in {(x, HAY_Y, z) for x, z in CAULDRONS}
                    and state == STATE_WATER_CAULDRON_L1]

        watch_for(session, CAULDRON_SECONDS, lambda: bool(filled()))
        got = filled()
        assert got, ("the rain never filled a cauldron in "
                     f"{CAULDRON_SECONDS}s: {sorted(session.last_state.items())[:4]}")
        step(f"  OK rain filled {len(got)} cauldron(s): {sorted(got)[:3]} -> water_cauldron[level=1]")

        # --- 8. a client that joins into the storm is told -------------------
        storm = Bot("StormBot", port)
        wire_client.record(storm, note="weather-wire: the client that joins into the storm")
        storm_join = storm.join()
        storm.start_reader()
        storm_join += drain(storm, 1.2)
        events = game_events(storm_join)
        assert GE_START_RAINING in [e for e, _ in events], events
        assert GE_RAIN_LEVEL_CHANGE in [e for e, _ in events], events
        assert GE_THUNDER_LEVEL_CHANGE in [e for e, _ in events], events
        levels = [v for e, v in events if e == GE_RAIN_LEVEL_CHANGE]
        assert levels and levels[0] == 1.0, levels
        storm.close()
        step(f"  OK a joining client is told it is raining ({len(events)} events)")

        bot.close()
        print("weather: all steps passed")
        return 0
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(world, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
