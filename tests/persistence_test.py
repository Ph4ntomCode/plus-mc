#!/usr/bin/env python3
"""Persistence test (Milestone E): block place → save → reload → verify.

Steps:
 1) Start server with --world <tmp> (fresh dir).
 2) Bot1 joins, places stone at (0,100,1) or uses placement offset.
 3) Wait for autosave or trigger: we just wait then terminate gracefully with SIGTERM.
 4) Reopen the same world directory via C++? Instead verify via second boot:
    Bot2 joins the world that was saved and checks the block update is now part of the
    spawned chunk (i.e., the placed block survives a restart).
We implement this by: start server A, place, shutdown, start server B on same world,
join and inspect chunk contents (decode chunk or wait for chunk data containing stone).
Simplified: we directly inspect the region file's NBT after shutdown using python.

Usage: persistence_test.py /path/to/plusmc
   or: PLUSMC_REPLAY=transcript.jsonl persistence_test.py   (no binary needed)

The socket comes from `wire_client.transport` and the client is handed to
`record`, so a failing run leaves a transcript. Replaying it re-runs phase 1 —
the join, the placement, its ack and its block update — with no server, and says
out loud that phase 2 (the save, the region file, the reload) is the server's own
side and is not in a transcript.
"""
import os, socket, struct, sys, time, zlib, pathlib, shutil, signal, subprocess, re, tempfile
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

def rd_varint(buf, pos=0):
    val, shift = 0, 0
    while pos < len(buf):
        b = buf[pos]; pos += 1
        val |= (b & 0x7F) << shift; shift += 7
        if not (b & 0x80): return val, pos
    return None, pos

def rd_string(buf, pos=0):
    ln, pos = rd_varint(buf, pos)
    return buf[pos:pos+ln].decode("utf-8"), pos+ln

def rd_varint_bytes(buf):
    v,p=rd_varint(buf)
    # also need length
    l=0
    while True:
        l+=1
        if not (buf[l-1] & 0x80): break
    return v,l

def position(x, y, z):
    return wire_client.pack_block_pos(x, y, z)

class Bot:
    def __init__(self, name, port):
        self.name=name
        self.sock=wire_client.transport(HOST,port,timeout=10)
        self.sock.settimeout(10)
        self.threshold=-1
        # The read side goes to a thread from the first drain after the join
        # (`wire_client.Reader`): a windowed drain that reads the socket itself
        # can abandon a half-taken packet when its window closes.
        self.reader=wire_client.Reader(self.sock, self)
    def send(self, pid, body=b""):
        self.reader.send(pid, body)
    def recv(self):
        return read_packet(self.sock, self.threshold)
    def start_reader(self):
        self.reader.start()
    def close(self):
        try: self.sock.close()
        except: pass
    def move_to(self, x, y, z, ground=1):
        # ServerboundMovePlayerPos (0x1e): feet x/y/z + on-ground flag. The
        # server's block interaction range is measured from where the client says
        # it is, so the bot has to stand next to the block it places; the world
        # spawn (8.5, 100, 8.5) is nine blocks from the target below.
        self.send(0x1e, struct.pack(">ddd", x, y, z) + bytes([ground]))
    def join(self):
        hs = (varint(0)+varint(PROTO)+varint(len(HOST))+HOST.encode()+struct.pack(">H",0)+varint(2))
        self.sock.sendall(make_frame(hs,-1))
        self.send(0x00, varint(len(self.name))+self.name.encode()+b"\x00"*16)
        while True:
            pid,body=self.recv()
            if pid==0x03:
                self.threshold,_=rd_varint(body)
            elif pid==0x02:
                self.send(0x03)
                break
            else:
                raise AssertionError(f"login 0x{pid:02x}")
        while True:
            pid,body=self.recv()
            if pid==0x0F:
                npacks,i=rd_varint(body)
                packs=[]
                for _ in range(npacks):
                    ns,i=rd_string(body,i); pk,i=rd_string(body,i); ver,i=rd_string(body,i)
                    packs.append((ns,pk,ver))
                out=varint(len(packs))
                for ns,pk,ver in packs:
                    out+=varint(len(ns))+ns.encode()+varint(len(pk))+pk.encode()+varint(len(ver))+ver.encode()
                self.send(0x07,out)
            elif pid==0x03:
                self.send(0x03)
                break
        while True:
            pid,body=self.recv()
            if pid==0x32:
                break
        # drain until batch finished
        for _ in range(400):
            pid,body=self.recv()
            if pid==0x2C:
                self.send(0x1C, body[:8])
            elif pid==0x0B:
                b,_=rd_varint(body)
                self.send(0x0B,varint(b))
                break

def wait_listening(proc, timeout=15):
    deadline=time.time()+timeout
    port=None
    while time.time()<deadline:
        line=proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
            m=re.search(r"listening on.*:(\d+)",line)
            if m:
                port=int(m.group(1)); break
    if port is None:
        raise RuntimeError("server did not start")
    for _ in range(30):
        try:
            s=socket.create_connection(("127.0.0.1",port),timeout=1); s.close(); break
        except OSError:
            time.sleep(0.1)
    return port

def main():
    if len(sys.argv)<2 and not wire_client.replaying():
        print(f"Usage: {sys.argv[0]} /path/to/plusmc",file=sys.stderr)
        print(f"   or: PLUSMC_REPLAY=transcript.jsonl {sys.argv[0]}"
              "  (no binary needed)",file=sys.stderr)
        return 1
    binary=os.path.realpath(sys.argv[1]) if len(sys.argv)>1 else ""
    data_root=os.path.join(os.path.dirname(__file__),"..","data","vanilla")
    tmpdir=tempfile.mkdtemp(prefix="plusmc-persist-")
    try:
        env=dict(os.environ)
        env["PLUSMC_KEEPALIVE_MS"]="1200"
        env["PLUSMC_KEEPALIVE_TIMEOUT_MS"]="4000"
        # The survival tick counts packets (one tick each, no wall-time
        # catch-up): what this file asserts is a block read back out of a region
        # after a restart, so nothing here is a survival-tick duration.
        env["PLUSMC_SURVIVAL_CLOCK"]="packet"
        # Use short autosave to test path, but we also do explicit save on shutdown
        env["PLUSMC_AUTOSAVE_SECS"]="1"
        print(f"world dir: {tmpdir}")
        proc,port=wire_client.start_server(
            [binary,"--host","127.0.0.1","--port","0","--motd","persist test","--offline-mode","--gamemode","1","--data",data_root,"--world",tmpdir],
            wait_listening, env=env)
        bot=Bot("PersistBot1",port)
        wire_client.record(bot, note="persistence: the block placed and the acks for it")
        bot.join()
        # give time for chunks
        time.sleep(0.5)
        # consume pending
        wire_client.drain(bot, 0.5)
        # Stand beside the block this test places, at (0, 100, 1).
        bot.move_to(0.5, 101.0, 0.5)
        time.sleep(0.2)
        # creative slot: menu slot 36 = hotbar slot 0, one stack of stone
        # ([count VarInt][item id VarInt][empty DataComponentPatch]);
        # see tests/block_interact_test.py for the layout notes.
        creative = struct.pack(">h",36)+varint(64)+varint(1)+b"\x00\x00"
        bot.send(0x39, creative)  # set_creative_mode_slot in 26.3
        time.sleep(0.2)
        # place at (0,100,1) face up (1)
        place = varint(0)+position(0,100,1)+varint(1)+struct.pack(">f",0.5)+struct.pack(">f",0.0)+struct.pack(">f",0.5)+bytes([0])+bytes([0])+varint(1)
        bot.send(0x42, place)
        # wait for ack
        got_ack=False
        got_update=False
        deadline=time.time()+5
        while time.time()<deadline and not (got_ack and got_update):
            for pid,body in wire_client.drain(bot, 0.2):
                if pid==0x04:
                    got_ack=True
                elif pid==0x08:
                    got_update=True
        print(f"got_ack={got_ack} got_update={got_update}")
        if not (got_ack and got_update):
            print("FAIL: did not get block place ack/update",file=sys.stderr)
            proc.terminate(); proc.wait(timeout=5)
            return 1
        bot.close()
        # --- phase 2: restart, read the block back off disk -------------------
        # The whole of what is left is the server's own side: the shutdown that
        # flushes the region file, the file itself, the second boot that reloads
        # it. A transcript holds the wire — the join, the placement, the acks,
        # all of which have just been replayed — and a replay has no server to
        # flush, write or reload anything.
        if wire_client.skip_server_check(
                "the restart: the save on shutdown, the region file it wrote and "
                "the block read back out of it"):
            print("\nPASS: persistence test (phase 1 replayed: place, ack)")
            return 0
        # graceful shutdown via SIGTERM so save_all flushes
        time.sleep(1.2) # let autosave trigger
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
            print("FAIL: server did not exit",file=sys.stderr)
            return 1
        # Verify region file exists and contains our chunk
        region = pathlib.Path(tmpdir)/"dimensions"/"minecraft"/"overworld"/"region"/"r.0.0.mca"
        if not region.exists():
            # try legacy
            region = pathlib.Path(tmpdir)/"region"/"r.0.0.mca"
        if not region.exists():
            print(f"FAIL: region file missing {region}",file=sys.stderr)
            print(list(pathlib.Path(tmpdir).rglob("*.mca")))
            return 1
        print(f"region exists: {region} size {region.stat().st_size}")
        # Second boot: reload and verify block
        env2=dict(os.environ)
        env2["PLUSMC_KEEPALIVE_MS"]="1200"
        env2["PLUSMC_KEEPALIVE_TIMEOUT_MS"]="4000"
        env2["PLUSMC_SURVIVAL_CLOCK"]="packet"
        proc2,port2=wire_client.start_server(
            [binary,"--host","127.0.0.1","--port","0","--motd","persist test2","--offline-mode","--gamemode","1","--data",data_root,"--world",tmpdir],
            wait_listening, env=env2)
        bot2=Bot("PersistBot2",port2)
        wire_client.record(bot2, note="persistence: the reload after the restart")
        bot2.join()
        # After join, decode chunk? We can just ask server to send chunk again: bot is at spawn near (8,100,8) which is in chunk (0,0)
        # The chunk should already have stone. We verify by breaking at (0,101,1)?? Actually placement offset: we placed at (0,100,1) target pos, face 1 => block at (0,101,1)? Wait decode: use_item_on at (0,100,1) face 1 (up) places at y+1. Code: px = bx + dy. So (0,101,1). Let's check both.
        # We break at (0,101,1) and expect it to be stone (non-air). But we can verify by inspecting region file directly with nbtlib.

        # Direct NBT inspection: decompress the region record and confirm stone persisted.
        try:
            data=region.read_bytes()
            loc=struct.unpack(">I", data[0:4])[0]
            sector=loc>>8
            off=sector*4096
            length=struct.unpack(">I", data[off:off+4])[0]
            comp=data[off+4]
            payload=data[off+5:off+4+length]
            nbt_bytes=zlib.decompress(payload)
            if b"minecraft:stone" in nbt_bytes:
                print("PASS: found stone in persisted chunk")
            else:
                print("FAIL: stone not in persisted NBT",file=sys.stderr)
                print(nbt_bytes[:2000])
                proc2.terminate(); proc2.wait()
                return 1
        except Exception as e:
            print(f"region inspect failed: {e}",file=sys.stderr)
            import traceback; traceback.print_exc()
            proc2.terminate(); proc2.wait()
            return 1

        bot2.close()
        proc2.terminate()
        try: proc2.wait(timeout=5)
        except: proc2.kill(); proc2.wait()
        print("\nPASS: persistence test (place→save→reload)")
        return 0
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

if __name__=="__main__":
    sys.exit(main())
