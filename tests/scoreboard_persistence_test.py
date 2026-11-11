#!/usr/bin/env python3
"""Integration test: the scoreboard across a restart (Phase 4 milestone M).

`scoreboard.dat` is only worth writing if the next boot reads it and the next
client sees it. This test drives that whole loop on a real world directory:

  * first boot: a player joins, creates a writable `dummy` objective, sets a
    score, shows it in the sidebar and adds a team, all through `/scoreboard`;
  * the server is stopped with SIGTERM, so the shutdown save runs (the same
    `save_scoreboard_state` the autosave and the backup call);
  * second boot, same directory: a joining client's `send_initial_scoreboard`
    burst must carry the objective, the display slot and the score, which is
    what proves the load landed *and* that it reaches the wire. The file's own
    schema is pinned by tests/scoreboard_save_test.cpp; this test is the
    session path.

Run: scoreboard_persistence_test.py /path/to/plusmc
"""
import os
import re
import shutil
import signal
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wire_client  # noqa: E402
from scoreboard_wire_test import (  # noqa: E402
    CB_SET_DISPLAY_OBJECTIVE,
    CB_SET_OBJECTIVE,
    CB_SET_SCORE,
    HOST,
    SLOT_SIDEBAR,
    all_of,
    drain,
    read_display_objective,
    read_set_objective,
    read_set_score,
    send_command,
)
from scoreboard_wire_test import Bot  # noqa: E402

PROTO = 777


class RecordingBot(Bot):
    """A Bot that keeps every packet, including the join burst.

    `Bot.join` consumes the play-state packets (the scoreboard snapshot is sent
    before the chunk batch), so a test that wants to inspect the join has to
    record on the read path. `Reader` calls this `recv`, so the recording is
    complete whether a packet is read during the join or by a later drain.
    """

    def __init__(self, name, port):
        self.received = []
        super().__init__(name, port)

    def recv(self):
        pid, body = super().recv()
        self.received.append((pid, body))
        return pid, body


def wait_listening(proc, timeout=25):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline().decode(errors="replace")
        if line:
            sys.stderr.write(line)
        m = re.search(r"listening on.*:(\d+)", line)
        if m:
            return int(m.group(1))
    return None


def wait_port(port):
    for _ in range(50):
        try:
            s = socket.create_connection((HOST, port), timeout=1)
            s.close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} /path/to/plusmc", file=sys.stderr)
        return 1
    binary = os.path.realpath(sys.argv[1])
    data_root = os.path.join(os.path.dirname(__file__), "..", "data", "vanilla")
    env = dict(os.environ)
    env["PLUSMC_KEEPALIVE_MS"] = "60000"
    env["PLUSMC_KEEPALIVE_TIMEOUT_MS"] = "300000"
    env["PLUSMC_DAY_CYCLE"] = "0"
    env["PLUSMC_MOB_SPAWNING"] = "0"

    tmp = tempfile.mkdtemp(prefix="pmc-scoreboard-persist-")
    world_dir = os.path.join(tmp, "world")
    proc = None
    try:
        # ---- first boot: build a board, then save it on shutdown -------------
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "scoreboard persist",
             "--offline-mode", "--gamemode", "1", "--world", world_dir,
             "--data", data_root],
            wait_listening, env=env)
        if port is None:
            print("FAIL: first boot did not listen", file=sys.stderr)
            return 1
        wait_port(port)
        bot = RecordingBot("BoardBot", port)
        bot.join()
        bot.start_reader()
        drain(bot, 1.0)
        send_command(bot, "scoreboard objectives add gate dummy Gate")
        send_command(bot, "scoreboard players set BoardBot gate 9")
        packets = drain(bot, 1.5)
        # The setdisplay is what makes the objective tracked (vanilla's gate),
        # and its burst is what a joining client gets from the file later.
        send_command(bot, "scoreboard objectives setdisplay sidebar gate")
        packets = drain(bot, 1.5)
        assert (SLOT_SIDEBAR, "gate") in [
            read_display_objective(b) for b in all_of(packets, CB_SET_DISPLAY_OBJECTIVE)
        ], "the live setdisplay must still send its display packet"
        send_command(bot, "scoreboard teams add red")
        drain(bot, 0.8)
        send_command(bot, "scoreboard teams join red BoardBot")
        drain(bot, 0.8)
        bot.close()

        proc.send_signal(signal.SIGTERM)
        assert proc.wait(timeout=30) == 0, "first boot must exit cleanly on SIGTERM"
        proc = None
        board_file = os.path.join(world_dir, "scoreboard.dat")
        assert os.path.isfile(board_file), f"shutdown must write {board_file}"

        # ---- second boot: the joining client must be handed the board --------
        proc, port = wire_client.start_server(
            [binary, "--host", HOST, "--port", "0", "--motd", "scoreboard persist",
             "--offline-mode", "--gamemode", "1", "--world", world_dir,
             "--data", data_root],
            wait_listening, env=env)
        if port is None:
            print("FAIL: second boot did not listen", file=sys.stderr)
            return 1
        wait_port(port)
        bot = RecordingBot("BoardBot", port)
        bot.join()
        bot.start_reader()
        # The join burst is already in `received`; a short drain covers anything
        # that raced the chunk-batch ack.
        joined = list(bot.received) + drain(bot, 1.0)
        objectives = [read_set_objective(b) for b in all_of(joined, CB_SET_OBJECTIVE)]
        assert ("gate", 0) in objectives, \
            f"the restored objective must be in the join burst: {objectives}"
        displays = [read_display_objective(b) for b in all_of(joined, CB_SET_DISPLAY_OBJECTIVE)]
        assert (SLOT_SIDEBAR, "gate") in displays, \
            f"the restored display slot must be in the join burst: {displays}"
        scores = [read_set_score(b) for b in all_of(joined, CB_SET_SCORE)]
        assert ("BoardBot", "gate", 9) in scores, \
            f"the restored score must be in the join burst: {scores}"
        bot.close()
        print("scoreboard persistence: all checks passed")
        return 0
    finally:
        if proc is not None:
            proc.kill()
            proc.wait()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
