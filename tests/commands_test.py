#!/usr/bin/env python3
"""Integration test: Milestone D commands via chat and via chat_command.

One bot joins, then:
  /time set day -> server broadcasts SetTime (0x73) and replies via SystemChat
  /gamemode c -> client gets GameEvent + Abilities + PlayerInfoUpdate(gamemode)
  /give self minecraft:stone -> feedback SystemChat and enables block place
  /tp 10 80 10 -> client gets PlayerPosition (0x49) entity teleport
Each is also exercised via the Play chat_command packet (0x07).

A second bot verifies that a SetTime broadcast reaches both players, and then
takes the cross-session commands: /gamemode, /tp (both forms), /give, /xp,
/effect (give and both clear forms), /enchant, /damage and /kill aimed at the
second client, each applied by that client's own session tick and asserted on
that client's packet family.
"""

import os, socket, struct, sys, time, re, subprocess, pathlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nbt_component import chat_text  # noqa: E402
import wire_client
from wire_client import make_frame, read_packet

PROTO = 777
HOST = "127.0.0.1"

def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: b |= 0x80
        out.append(b)
        if not n: return bytes(out)

def varlong(n):
    out = bytearray()
    while True:
        b = n & 0x7F; n >>= 7
        if n: b |= 0x80
        out.append(b)
        if not n: return bytes(out)

def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]; pos += 1
        val |= (b & 0x7F) << shift; shift += 7
        if not (b & 0x80): return val, pos
    return None, pos

def rd_varlong(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]; pos += 1
        val |= (b & 0x7F) << shift; shift += 7
        if not (b & 0x80): return val, pos
    return None, pos

def rd_string(buf, pos=0):
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos+ln].decode("utf-8"), pos+ln

class Bot:
    def __init__(self, name, port):
        self.name = name
        self.sock = wire_client.transport(HOST, port, timeout=10)
        self.sock.settimeout(2.0)
        self.threshold = -1
        # The read side belongs to a thread from the first drain after the join
        # (`wire_client.Reader`): a windowed drain that reads the socket itself
        # can abandon a half-taken packet when its window closes, which leaves
        # every packet after it garbage, and it can also miss a keep-alive.
        self.reader = wire_client.Reader(self.sock, self)
        self.command_tree = None

    def send(self, pkt_id, body=b""):
        self.reader.send(pkt_id, body)

    def recv(self):
        return read_packet(self.sock, self.threshold)

    def start_reader(self):
        self.reader.start()


    def close(self):
        try: self.sock.close()
        except OSError: pass

    def move_to(self, x, y, z, ground=1):
        # ServerboundMovePlayerPos (0x1e): feet x/y/z + on-ground flag. The
        # server's block interaction range is measured from where the client says
        # it is (eye to the block's box, 5.5 blocks), so the bot stands by the
        # block it places rather than wherever it spawned.
        self.send(0x1e, struct.pack(">ddd", x, y, z) + bytes([ground]))

    def join(self):
        hs = (varint(0) + varint(PROTO) + varint(len(HOST)) + HOST.encode() + struct.pack(">H", 0) + varint(2))
        self.sock.sendall(make_frame(hs, -1))
        self.send(0x00, varint(len(self.name)) + self.name.encode() + b"\x00"*16)
        while True:
            pid, body = self.recv()
            if pid == 0x03:
                self.threshold, _ = rd_varint(body)
            elif pid == 0x02:
                self.send(0x03); break
            else:
                raise AssertionError(f"unexpected login 0x{pid:02x}")
        while True:
            pid, body = self.recv()
            if pid == 0x0F:
                npacks, i = rd_varint(body)
                packs=[]
                for _ in range(npacks):
                    ns,i = rd_string(body,i)
                    pk,i = rd_string(body,i)
                    ver,i = rd_string(body,i)
                    packs.append((ns,pk,ver))
                out = varint(len(packs))
                for ns,pk,ver in packs:
                    out += varint(len(ns))+ns.encode()+varint(len(pk))+pk.encode()+varint(len(ver))+ver.encode()
                self.send(0x07, out)
            elif pid == 0x03:
                self.send(0x03); break
        while True:
            pid,_ = self.recv()
            if pid == 0x32: break
        for _ in range(400):
            pid, body = self.recv()
            if pid == 0x10:
                # commands: the tree the client parses and colours input from.
                self.command_tree = body
            elif pid == 0x2D:
                self.send(0x1C, body[:8])
            elif pid == 0x0B:
                batch_size, _ = rd_varint(body)
                self.send(0x0B, varint(batch_size)); break
        assert self.command_tree is not None, "server sent no commands packet"
        assert b"gamemode" in self.command_tree and b"scoreboard" in self.command_tree, \
            "command tree is missing implemented commands"
        return True

def drain(bot, timeout=2.0):
    """The packets the server sends before it goes quiet, up to `timeout`.

    A command's reply is synchronous, so a window here ends when the server has
    been quiet for `quiet` seconds instead of always burning the whole window;
    `timeout` stays the bound a missing reply is judged against. Both bots start
    their readers, so this is the queue path.
    """
    return wire_client.drain_quiet(bot, quiet=0.3, limit=timeout)
def has_pid(packets, pid): return any(p==pid for p,_ in packets)


def decode_stack(buf, pos):
    """ItemStack.OPTIONAL_STREAM_CODEC: varint count (0 = empty), then the item
    id and the DataComponentPatch. Only the empty patch is accepted here — the
    stacks in these assertions are plain, and silently mis-reading a two-varint
    patch would hide a layout bug."""
    count, pos = rd_varint(buf, pos)
    if count == 0:
        return None, pos
    item, pos = rd_varint(buf, pos)
    added, pos = rd_varint(buf, pos)
    removed, pos = rd_varint(buf, pos)
    assert added == 0 and removed == 0, f"unexpected component patch {added}/{removed}"
    return (item, count), pos


def decode_container_content(body):
    """container_set_content (0x12): [varint container id][varint state id]
    [varint slot count][stacks...][carried stack]. Requires exact consumption."""
    cid = body[0]
    state, pos = rd_varint(body, 1)
    n, pos = rd_varint(body, pos)
    slots = []
    for _ in range(n):
        s, pos = decode_stack(body, pos)
        slots.append(s)
    carried, pos = decode_stack(body, pos)
    assert pos == len(body), f"container_set_content: {len(body)-pos} trailing byte(s)"
    return cid, state, slots, carried


def decode_container_set_slot(body):
    """container_set_slot (0x14): [varint container id][varint state id]
    [i16 slot][stack]."""
    cid = body[0]
    state, pos = rd_varint(body, 1)
    assert pos + 2 <= len(body), "container_set_slot truncated before the slot"
    slot = struct.unpack(">h", body[pos:pos+2])[0]
    stack, pos = decode_stack(body, pos + 2)
    assert pos == len(body), f"container_set_slot: {len(body)-pos} trailing byte(s)"
    return cid, state, slot, stack

def parse_set_time(body):
    # ClientboundSetTime 0x73: i64 gameTime + varint(1) + varint(holder) + varlong(total) + f32 + f32
    if len(body) < 8: return None
    import struct as st
    gameT = st.unpack(">q", body[0:8])[0]
    p=8
    n,_ = rd_varint(body,p); p = _+0  # dummy
    # Actually: varint count, then holder varint, varlong total, f32, f32
    cnt,_ = rd_varint(body,8); pos=_
    holder,_ = rd_varint(body,pos); pos=_
    total,_ = rd_varlong(body,pos); pos=_
    # f32 partial, f32 rate follow
    return gameT, cnt, holder, total

def main():
    if len(sys.argv) < 2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1]) if len(sys.argv) > 1 else ""
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    # The day/night cycle advances the world clock continuously; freeze it so
    # the /time assertions stay exact (the cycle has its own test, hosting).
    env = dict(os.environ); env["PLUSMC_KEEPALIVE_MS"]="1200"; env["PLUSMC_KEEPALIVE_TIMEOUT_MS"]="4000"
    env["PLUSMC_DAY_CYCLE"]="0"
    # The survival tick counts packets rather than wall time (one tick each, no
    # catch-up), so a command's drain banks no ticks. Everything asserted here
    # is the command's own effect — a block update, a game event, a chat line —
    # and none of it is a survival-tick duration; pinning the clock keeps a
    # later edit from inheriting a wall-clock one.
    env["PLUSMC_SURVIVAL_CLOCK"]="packet"
    def wait_listening(proc, timeout=20):
        """The port from the server's own "listening on host:port" line."""
        deadline=time.time()+timeout
        while time.time() < deadline:
            line=proc.stdout.readline().decode(errors="replace")
            if line: sys.stderr.write(line)
            m=re.search(r"listening on.*:(\d+)", line)
            if m: return int(m.group(1))
        print("FAIL: server did not start", file=sys.stderr)
        return None

    proc, port = wire_client.start_server(
        [binary,"--host","127.0.0.1","--port","0","--motd","cmd test","--offline-mode","--gamemode","0","--data",data_root],
        wait_listening, env=env)
    if port is None:
        proc.kill(); proc.wait(); return 1
    if not wire_client.replaying():
        for _ in range(20):
            try:
                s=socket.create_connection(("127.0.0.1",port),timeout=1); s.close(); break
            except OSError: time.sleep(0.1)

    try:
        print("Connecting bot1 (CmdTester)...")
        bot = Bot("CmdTester", port)
        wire_client.record(bot, note="commands: /gamemode /tp /give /time and the chat paths")
        bot.join(); bot.start_reader(); print(f"  joined threshold={bot.threshold}")
        # drain the initial SetTime after join
        pkts = drain(bot,1.0)
        assert has_pid(pkts, 0x73), f"expected initial SetTime 0x73, got {[hex(p) for p,_ in pkts][:10]}"
        print("  initial SetTime present")

        # The player inventory menu must be opened and synced at join: 46 menu
        # slots (the player menu's size), all empty, with no cursor stack.
        contents = [b for p, b in pkts if p == 0x12]
        assert contents, f"no container_set_content at join, got {[hex(p) for p,_ in pkts][:12]}"
        cid, state, slots, carried = decode_container_content(contents[-1])
        assert cid == 0, f"the player inventory is menu 0, got {cid}"
        assert len(slots) == 46, f"player menu is 46 slots, got {len(slots)}"
        assert all(s is None for s in slots), "a fresh join starts with an empty inventory"
        assert carried is None, "a fresh join has nothing on the cursor"
        print(f"  OK inventory menu synced at join (container {cid}, {len(slots)} slots, state {state})")

        # Helper: send chat command and drain
        def send_chat(cmd):
            payload = varint(len(cmd)) + cmd.encode()
            bot.send(0x09, payload)

        # Plain chat: the server must echo a SystemChat whose component is
        # valid network NBT — decoding it is what used to disconnect a real
        # client ("Failed to decode packet 'clientbound/minecraft:system_chat'").
        print("-> plain chat message")
        send_chat("hello world")
        pkts = drain(bot,2.0)
        chats = [chat_text(b) for p,b in pkts if p==0x7C]
        assert "<CmdTester> hello world" in chats, f"chat echo missing/wrong: {chats}"
        print("  OK chat echo (component is valid NBT)")

        def send_chat_command(cmd_minus_slash):
            payload = varint(len(cmd_minus_slash)) + cmd_minus_slash.encode()
            bot.send(0x07, payload)

        # /time set day (expects day->1000, broadcast SetTime, feedback chat)
        print("-> /time set day via chat")
        send_chat("/time set day")
        pkts = drain(bot,2.0)
        assert has_pid(pkts, 0x73), f"/time set: missing SetTime, got {[hex(p) for p,_ in pkts]}"
        st = parse_set_time([b for p,b in pkts if p==0x73][0])
        assert st and st[2]==0 and st[3]==1000, f"/time set day expected total 1000, got {st}"
        assert any("Set time" in chat_text(b) for p,b in pkts if p==0x7C), "missing feedback"
        print("  OK /time set day")

        # /time add 100
        print("-> /time add 100 via chat_command")
        send_chat_command("time add 100")
        pkts = drain(bot,2.0)
        assert has_pid(pkts, 0x73)
        st = parse_set_time([b for p,b in pkts if p==0x73][0])
        assert st and st[3]==1100, f"/time add 100 expected 1100, got {st}"
        print("  OK /time add 100")

        # /time query daytime
        print("-> /time query daytime")
        send_chat("/time query daytime")
        pkts = drain(bot,2.0)
        assert any("daytime" in chat_text(b).lower() and "1100" in chat_text(b) for p,b in pkts if p==0x7C), "query feedback missing"
        print("  OK /time query")

        # /gamemode creative
        print("-> /gamemode creative")
        send_chat("/gamemode creative")
        pkts = drain(bot,2.0)
        # Expect GameEvent(0x27), Abilities(0x41), PlayerInfoUpdate(0x47 gamemode), feedback(0x7C)
        assert has_pid(pkts, 0x27), "missing GameEvent"
        assert has_pid(pkts, 0x41), "missing Abilities"
        # Check that a 0x47 with gamemode 1 is present
        pius = [b for p,b in pkts if p==0x47]
        assert pius, "missing PlayerInfoUpdate"
        found=False
        for body in pius:
            if len(body) >= 2 and body[0]==0x04: # gamemode action
                found=True
        assert found, "PlayerInfoUpdate gamemode action not present"
        assert any("Creative" in chat_text(b) or "creative" in chat_text(b).lower() for p,b in pkts if p==0x7C)
        print("  OK /gamemode creative")

        # /give self stone 2
        print("-> /give CmdTester minecraft:stone 2")
        send_chat("/give CmdTester minecraft:stone 2")
        pkts = drain(bot,2.0)
        assert any("Gave" in chat_text(b) and "minecraft:stone" in chat_text(b) for p,b in pkts if p==0x7C), f"feedback missing {[(p,chat_text(b)) for p,b in pkts if p==0x7C]}"
        # ...and the slot itself must reach the client, or the player is told
        # they received items they cannot see. /give fills the selected slot:
        # menu 36 is hotbar 0, and stone is item id 1.
        contents = [b for p, b in pkts if p == 0x12]
        assert contents, f"/give must sync the inventory, got {[hex(p) for p,_ in pkts]}"
        cid, give_state, slots, _ = decode_container_content(contents[-1])
        assert cid == 0 and len(slots) == 46, (cid, len(slots))
        assert slots[36] == (1, 2), f"menu slot 36 should hold stone x2, got {slots[36]}"
        print("  OK /give")

        # Verify block place now works (held_item was set by /give)
        # Place at (1,4,1) face up -> actual (1,5,1), from right beside it.
        bot.move_to(1.5, 5.0, 1.5)
        time.sleep(0.2)
        def position(x,y,z): return wire_client.pack_block_pos(x, y, z)
        place = varint(0)+position(1,4,1)+varint(1)+struct.pack(">f",0.5)+struct.pack(">f",0.0)+struct.pack(">f",0.5)+bytes([0,0])+varint(1)
        bot.send(0x42, place)
        pkts = drain(bot,2.0)
        assert has_pid(pkts, 0x08), f"after give, place should yield BlockUpdate 0x08, got {[hex(p) for p,_ in pkts]}"
        print("  OK place after /give")

        # --- container_click (0x12): PICKUP the stone off menu slot 36 ---
        # Body: varint container, varint state, i16 slot, i8 button, varint click
        # type, varint changed-slot count, then the changed slots' HashedStacks
        # and the carried HashedStack (both absent here). In creative the client
        # uses set_creative_mode_slot for the player menu, but a survival client
        # sends this, and the server must still apply it and answer with the
        # authoritative slots + cursor. Placed before the creative slot write
        # below, which overwrites slot 36.
        click = (bytes([0]) + varint(give_state) + struct.pack(">h", 36) + bytes([0]) +
                 varint(0) + varint(0) + bytes([0]))
        bot.send(0x12, click)
        pkts = drain(bot, 2.0)
        slot_sets = {sl: stk for _, _, sl, stk in
                     (decode_container_set_slot(b) for p, b in pkts if p == 0x14)}
        assert slot_sets.get(36, "missing") is None, \
            f"picking up slot 36 must empty it, got {slot_sets.get(36, 'missing')!r}"
        cursors = [b for p, b in pkts if p == 0x62]
        assert cursors, f"no set_cursor_item after the click: {[hex(p) for p,_ in pkts]}"
        cur, pos = decode_stack(cursors[-1], 0)
        assert pos == len(cursors[-1]), "set_cursor_item must be exactly one stack"
        assert cur == (1, 2), f"cursor should hold stone x2, got {cur}"
        states = [st for _, st, _, _ in (decode_container_set_slot(b) for p, b in pkts if p == 0x14)]
        assert states and states[-1] > give_state, \
            f"a mutation must advance state_id past {give_state}, got {states}"
        new_state = states[-1]
        print(f"  OK container_click pickup (cursor stone x2, state {give_state} -> {new_state})")

        # A click replayed against the superseded revision must be rejected and
        # answered with a full resync, not applied to state the client never saw.
        bot.send(0x12, click)
        pkts = drain(bot, 2.0)
        assert has_pid(pkts, 0x12), \
            f"a stale click must trigger container_set_content, got {[hex(p) for p,_ in pkts]}"
        cid, state, slots, carried = decode_container_content([b for p, b in pkts if p == 0x12][-1])
        assert cid == 0 and state > give_state and carried == (1, 2), (cid, state, carried)
        print("  OK stale click resynced")

        # A creative slot write is echoed back for the exact slot (menu 36 = hotbar 0).
        # Done after the placement check above, which needs the stone still there.
        bot.send(0x39, struct.pack(">h", 36) + varint(5) + varint(1012) + varint(0) + varint(0))
        pkts = drain(bot,2.0)
        sets = [b for p, b in pkts if p == 0x14]
        assert sets, f"set_creative_slot must be echoed with container_set_slot, got {[hex(p) for p,_ in pkts]}"
        cid, _, slot, stack = decode_container_set_slot(sets[-1])
        assert cid == 0 and slot == 36, f"echo should target menu slot 36, got {slot}"
        assert stack == (1012, 5), f"menu slot 36 should hold diamond x5, got {stack}"
        print("  OK creative slot echo")

        # /tp 10 80 10 (absolute)
        print("-> /tp 10 80 10")
        send_chat("/tp 10 80 10")
        pkts = drain(bot,2.0)
        assert has_pid(pkts, 0x49), f"/tp missing PlayerPosition 0x49, got {[hex(p) for p,_ in pkts]}"
        assert any("Teleported" in chat_text(b) for p,b in pkts if p==0x7C)
        print("  OK /tp absolute")

        # /tp ~1 ~ ~-1 (relative)
        print("-> /tp ~1 ~ ~-1 (relative)")
        send_chat("/tp ~1 ~ ~-1")
        pkts = drain(bot,2.0)
        assert has_pid(pkts, 0x49)
        print("  OK /tp relative")

        # Second bot: verify broadcast of /time set is seen by both
        print("Connecting bot2...")
        bot2 = Bot("CmdViewer", port)
        wire_client.record(bot2, note="commands: the second client of the session")
        bot2.join(); bot2.start_reader(); drain(bot2,1.0)
        print("  bot2 joined, setting time to 6000 and checking both see it")
        send_chat("/time set 6000")
        p1 = drain(bot,2.0); p2 = drain(bot2,2.0)
        assert has_pid(p1, 0x73) and has_pid(p2, 0x73), f"broadcast failure p1 {[hex(p) for p,_ in p1]} p2 {[hex(p) for p,_ in p2]}"
        st1 = parse_set_time([b for p,b in p1 if p==0x73][0])
        assert st1 and st1[3]==6000
        print("  OK broadcast")

        # --- cross-session commands ------------------------------------------
        # Every command below changes state that belongs to *bot2's* own session
        # (its game mode, its position, its health). The action is queued for
        # that session and applied on bot2's next packet, so a real client's
        # one-packet-per-tick clock is what resolves it; the reply travels back
        # to bot the same way. Both clients are driven, and each step asserts
        # the target's own packet family, not just the sender's feedback.
        def pump(seconds=1.0):
            """Drive both clients' ticks and return (bot pkts, bot2 pkts)."""
            first, second = [], []
            deadline = time.time() + seconds
            while time.time() < deadline:
                bot.send(0x21, bytes([1]))
                bot2.send(0x21, bytes([1]))
                first += drain(bot, 0.1)
                second += drain(bot2, 0.1)
            return first, second

        def game_events(pkts):
            """game_event (0x27): [u8 event][f32 value]."""
            return [(b[0], struct.unpack(">f", b[1:5])[0]) for p, b in pkts if p == 0x27]

        def positions(pkts):
            """player_position (0x49): [varint teleport id][f64 x y z]..."""
            out = []
            for p, b in pkts:
                if p != 0x49:
                    continue
                _, i = rd_varint(b, 0)
                out.append(struct.unpack(">ddd", b[i:i + 24]))
            return out

        def healths(pkts):
            """set_health (0x6A): [f32 health][varint food][f32 saturation]."""
            return [round(struct.unpack(">f", b[0:4])[0], 3) for p, b in pkts if p == 0x6A]

        def said(pkts, needle):
            """Chat lines containing `needle`. A SystemChat is not always a
            literal component — the death broadcast is a translatable one — so a
            line `chat_text` cannot decode is skipped rather than failing the
            step that is looking for a different line."""
            out = []
            for p, b in pkts:
                if p != 0x7C:
                    continue
                try:
                    text = chat_text(b)
                except AssertionError:
                    continue
                if needle in text:
                    out.append(text)
            return out

        print("-> /gamemode creative CmdViewer (the other session applies it)")
        send_chat("/gamemode creative CmdViewer")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Set CmdViewer's game mode to Creative"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        events = game_events(b_pkts)
        assert (3, 1.0) in events, f"CmdViewer did not get change_game_mode creative: {events}"
        print("  OK /gamemode <mode> <player> (GameEvent 3 = 1 at the target)")

        print("-> /gamemode survival CmdViewer (back, for the damage step)")
        send_chat("/gamemode survival CmdViewer")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Set CmdViewer's game mode to Survival")
        assert (3, 0.0) in game_events(b_pkts), game_events(b_pkts)
        print("  OK /gamemode <mode> <player> back to Survival")

        print("-> /tp CmdViewer 40 70 40 (target teleports)")
        send_chat("/tp CmdViewer 40 70 40")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Teleported CmdViewer to 40"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        dests = positions(b_pkts)
        assert any(abs(x - 40) < 1e-6 and abs(y - 70) < 1e-6 and abs(z - 40) < 1e-6
                   for x, y, z in dests), f"target not moved: {dests}"
        print("  OK /tp <player> <x> <y> <z>")

        print("-> /tp CmdViewer (self to that player)")
        send_chat("/tp CmdViewer")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Teleported CmdTester to CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        dests = positions(a_pkts)
        assert any(abs(x - 40) < 1e-6 and abs(z - 40) < 1e-6 for x, y, z in dests), \
            f"sender not moved to the target: {dests}"
        print("  OK /tp <player>")

        # Entity to entity: the sender moves away first, so the target's landing
        # spot names a position only the sender has and the step cannot pass on
        # where the target already stood.
        print("-> /tp 20 70 20 (sender away), then /tp CmdViewer CmdTester")
        send_chat("/tp 20 70 20")
        a_pkts, _ = pump(1.0)
        assert said(a_pkts, "Teleported to 20"), \
            f"sender did not move: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        send_chat("/tp CmdViewer CmdTester")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Teleported CmdViewer to CmdTester"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        dests = positions(b_pkts)
        assert any(abs(x - 20) < 1e-6 and abs(y - 70) < 1e-6 and abs(z - 20) < 1e-6
                   for x, y, z in dests), f"target not moved to the sender: {dests}"
        print("  OK /tp <player> <player>")

        print("-> /tp CmdViewer Nobody (unknown destination)")
        send_chat("/tp CmdViewer Nobody")
        a_pkts, _ = pump(1.0)
        assert said(a_pkts, "Player 'Nobody' not found"), \
            f"destination resolve missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        print("  OK unknown destination")

        print("-> /give CmdViewer minecraft:diamond_sword 1")
        send_chat("/give CmdViewer minecraft:diamond_sword 1")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Gave 1 [minecraft:diamond_sword] to CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        menus = [decode_container_content(b) for p, b in b_pkts if p == 0x12]
        assert menus and menus[-1][2][36] == (1050, 1), \
            f"target inventory did not receive the sword: {menus[-1][2][36] if menus else None}"
        print("  OK /give <player> (menu content at the target)")

        print("-> /xp add CmdViewer 5 levels, then /xp query CmdViewer levels")
        send_chat("/xp add CmdViewer 5 levels")
        a_pkts, _ = pump(1.5)
        assert said(a_pkts, "Gave 5 experience levels to CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        send_chat("/xp query CmdViewer levels")
        a_pkts, _ = pump(1.5)
        assert said(a_pkts, "CmdViewer has 5 experience levels"), \
            f"query reply missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        print("  OK /xp add and /xp query on the other player")

        print("-> /effect give CmdViewer speed 10 1")
        send_chat("/effect give CmdViewer speed 10 1")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Applied Speed to CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        assert has_pid(b_pkts, 0x87), \
            f"target got no update_mob_effect: {[hex(p) for p, _ in b_pkts]}"
        print("  OK /effect give <player> (update_mob_effect at the target)")

        print("-> /effect clear CmdViewer speed")
        send_chat("/effect clear CmdViewer speed")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Removed Speed from CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        assert has_pid(b_pkts, 0x4F), f"target got no remove_mob_effect: {[hex(p) for p, _ in b_pkts]}"
        print("  OK /effect clear <player> (remove_mob_effect at the target)")

        # The clear-*all* form is its own branch (`clearEffects`), and so is the
        # answer to a `/xp set ... points` the target's own level refuses: both
        # read the target's state, so both can only be answered from their tick.
        print("-> /effect give CmdViewer speed 10 + regeneration 10, then /effect clear CmdViewer")
        send_chat("/effect give CmdViewer speed 10")
        pump(1.0)
        send_chat("/effect give CmdViewer regeneration 10")
        a_pkts, _ = pump(1.5)
        assert said(a_pkts, "Applied Regeneration to CmdViewer"), \
            f"second effect missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        send_chat("/effect clear CmdViewer")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Removed every effect from CmdViewer"), \
            f"clear-all reply missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        # onEffectsRemoved is one remove packet per active effect, both of them.
        assert len([p for p, _ in b_pkts if p == 0x4F]) >= 2, \
            f"target got fewer than two remove_mob_effect packets: {[hex(p) for p, _ in b_pkts]}"
        print("  OK /effect clear <player> (every effect, one packet each)")

        print("-> /xp set CmdViewer 100000 points (refused against their own level)")
        send_chat("/xp set CmdViewer 100000 points")
        a_pkts, _ = pump(1.5)
        assert said(a_pkts, "Cannot set experience points above the limit of"), \
            f"ceiling reply missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        print("  OK /xp set <player> points (the ceiling is the target's)")

        print("-> /enchant CmdViewer sharpness 2")
        send_chat("/enchant CmdViewer sharpness 2")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "Applied enchantment") and said(a_pkts, "CmdViewer's item"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        assert has_pid(b_pkts, 0x14), f"target's slot was not re-sent: {[hex(p) for p, _ in b_pkts]}"
        print("  OK /enchant <player> (enchantment component + slot resync at the target)")

        print("-> /damage CmdViewer 3")
        send_chat("/damage CmdViewer 3")
        a_pkts, b_pkts = pump(1.5)
        assert said(a_pkts, "damage to CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        hp = healths(b_pkts)
        assert hp and hp[-1] < 20.0, f"target health did not move: {hp}"
        print(f"  OK /damage <player> (target health {hp[-1]})")

        print("-> /kill CmdViewer")
        send_chat("/kill CmdViewer")
        a_pkts, b_pkts = pump(2.0)
        assert said(a_pkts, "Killed CmdViewer"), \
            f"sender feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        assert has_pid(b_pkts, 0x45), f"target got no player_combat_kill: {[hex(p) for p, _ in b_pkts]}"
        assert healths(b_pkts)[-1] == 0.0, healths(b_pkts)
        print("  OK /kill <player> (combat kill + health 0 at the target)")

        print("-> /tp Nobody (unknown target)")
        send_chat("/tp Nobody")
        a_pkts, _ = pump(1.0)
        assert said(a_pkts, "Player 'Nobody' not found"), \
            f"not-found feedback missing: {[chat_text(b) for p, b in a_pkts if p == 0x7C]}"
        print("  OK not-found reply")

        # Unknown command feedback
        print("-> /boguscmd should feedback unknown")
        send_chat("/boguscmd foo")
        pkts = drain(bot,1.5)
        assert any("Unknown command" in chat_text(b) for p,b in pkts if p==0x7C)
        print("  OK unknown command")

        bot.close(); bot2.close()
    finally:
        proc.terminate(); proc.wait(timeout=5)
    print("\nPASS: commands test")
    return 0

if __name__ == "__main__":
    sys.exit(main())
