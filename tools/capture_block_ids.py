#!/usr/bin/env python3
"""Capture clientbound packet IDs for block updates from a live vanilla 26.2 server."""
import socket, struct, sys, time, zlib, os, subprocess, signal

PROTO = 776
HOST = "127.0.0.1"
PORT = 25566

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

def read_exact(sock, n):
    chunks, got = [], 0
    while got < n:
        c = sock.recv(n - got)
        if not c: raise EOFError(f"EOF {got}/{n}")
        chunks.append(c); got += len(c)
    return b"".join(chunks)

def read_varint_stream(sock):
    result, shift = 0, 0
    for _ in range(5):
        b = read_exact(sock, 1)[0]
        result |= (b & 0x7F) << shift; shift += 7
        if not (b & 0x80): return result
    raise ValueError("VarInt too long")

def read_frame(sock, threshold=-1):
    length = read_varint_stream(sock)
    data = read_exact(sock, length)
    if threshold < 0: return data
    dl, off = rd_varint(data, 0)
    payload = data[off:]
    if dl > 0: payload = zlib.decompress(payload)
    return payload

def write_frame(sock, threshold, payload):
    if threshold < 0:
        sock.sendall(varint(len(payload)) + payload)
    else:
        body = varint(0) + payload
        sock.sendall(varint(len(body)) + body)

def position(x, y, z):
    return struct.pack(">q", ((x & 0x3FFFFFF) << 38) | ((y & 0xFFF) << 26) | (z & 0x3FFFFFF))

# Start server
work_dir = "/tmp/capture_blocks"
os.makedirs(work_dir, exist_ok=True)
with open(os.path.join(work_dir, "server.properties"), "w") as f:
    f.write(f"online-mode=false\nserver-port={PORT}\nlevel-type=minecraft\\:flat\ngamemode=creative\n")
with open(os.path.join(work_dir, "eula.txt"), "w") as f:
    f.write("eula=true\n")

proc = subprocess.Popen(
    ["java", "-Xmx1G", "-jar", os.path.abspath("build/_dl/boot/server-boot.jar"), "nogui"],
    cwd=work_dir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

print("Waiting for server...", file=sys.stderr)
for _ in range(120):
    line = proc.stdout.readline().decode(errors="replace")
    if line: sys.stderr.write(line)
    if "Done" in line and "For help" in line:
        time.sleep(1)  # let the server finish binding
        break

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(15)
for attempt in range(10):
    try:
        sock.connect((HOST, PORT))
        break
    except ConnectionRefusedError:
        time.sleep(0.5)
else:
    print("Cannot connect", file=sys.stderr); sys.exit(1)

# Handshake -> Login
write_frame(sock, -1, varint(0x00) + varint(PROTO) + varint(9) + b"127.0.0.1" + struct.pack(">H", PORT) + varint(2))
write_frame(sock, -1, varint(0x00) + varint(9) + b"CapBot123")

# Read Set Compression + Login Success
threshold = -1
try:
    for _ in range(3):
        frame = read_frame(sock)
        pid, pos = rd_varint(frame, 0)
        print(f"Login resp: 0x{pid:02x} len={len(frame)}", file=sys.stderr)
        if pid == 0x03:  # Set Compression
            thresh, _ = rd_varint(frame, pos)
            threshold = thresh
            print(f"  threshold={threshold}", file=sys.stderr)
        elif pid == 0x02:  # Login Success
            break
except EOFError as e:
    print(f"Login read failed: {e}", file=sys.stderr)
    sock.close(); proc.kill(); proc.wait(); sys.exit(1)

# Send Login Acknowledged
write_frame(sock, threshold, varint(0x03))

# Configuration phase: read until Finish Configuration, ack everything
for _ in range(30):
    frame = read_frame(sock, threshold)
    pid, pos = rd_varint(frame, 0)
    if pid == 0x03:  # Finish Configuration
        print("Config done", file=sys.stderr)
        break
    if pid == 0x07:  # select_known_packs
        write_frame(sock, threshold, varint(0x07) + varint(0))
    if pid == 0x00:  # settings: respond with our settings
        pass

# Send Finish Configuration
write_frame(sock, threshold, varint(0x03))

# Wait for Join Game
frame = read_frame(sock, threshold)
pid, pos = rd_varint(frame, 0)
print(f"First play: 0x{pid:02x}", file=sys.stderr)

# Send Teleport Confirm
write_frame(sock, threshold, varint(0x00) + varint(0))

# Drain chunk batch + wait for terrain
time.sleep(2)

# Read all pending packets, responding to keep-alives
all_packets = {}
def drain(timeout=2):
    start = time.time()
    while time.time() - start < timeout:
        try:
            frame = read_frame(sock, threshold)
            pid, pos = rd_varint(frame, 0)
            body = frame[pos:]
            key = f"0x{pid:02x}"
            if key not in all_packets: all_packets[key] = []
            all_packets[key].append(body[:32].hex())
            if pid == 0x2c:  # keep_alive
                write_frame(sock, threshold, varint(0x1c) + body[:8])
        except: break

drain()

# Send block_dig (STOP_DESTROY_BLOCK = 2) at (0, 4, 0)
print("\n=== block_dig STOP at (0,4,0) ===", file=sys.stderr)
write_frame(sock, threshold, varint(0x29) + varint(2) + position(0, 4, 0) + varint(0) + varint(1))
drain(3)

# Send block_place at (0, 4, 1) - place stone on top of grass
print("\n=== block_place at (0,4,1) ===", file=sys.stderr)
place = (varint(0x42) + varint(0) + position(0, 4, 1) + varint(1)
         + struct.pack(">f", 0.5) + struct.pack(">f", 0.0) + struct.pack(">f", 0.5)
         + bytes([0]) + bytes([0]) + varint(2))
write_frame(sock, threshold, place)
drain(3)

# Print results
print("\n=== CAPTURED CLIENTBOUND PACKET IDs ===")
for pid, bodies in sorted(all_packets.items()):
    print(f"  {pid}: {len(bodies)}x  first_body={bodies[0]}")

proc.kill(); proc.wait()
