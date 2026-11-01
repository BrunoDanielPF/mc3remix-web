#!/usr/bin/env python3
"""Opaque binary packet relay for the browser LAN prototype.

The first client to connect to /v1/rooms/<32 lowercase hex digits> creates a
room. Later clients with the same code join it. The relay never sees game
assets and never simulates the game.
"""

import argparse
import asyncio
import contextlib
import json
import re
import time
from collections import deque
from dataclasses import dataclass, field
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed


ROOM_PATH = re.compile(r"^/v1/rooms/([0-9a-f]{32})$")
MAX_PEERS = 8
MAX_PACKET = 65536
MAX_PACKETS_PER_SECOND = 256
MAX_BYTES_PER_SECOND = 2 * 1024 * 1024
QUEUE_LIMIT = 32
MAX_NAME_CHARS = 24


@dataclass(eq=False)
class Peer:
    socket: object
    slot: int
    name: str
    outgoing: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(QUEUE_LIMIT))
    recent: deque = field(default_factory=deque)
    recent_bytes: int = 0
    overloaded: bool = False

    def within_rate(self, size):
        now = time.monotonic()
        while self.recent and self.recent[0][0] <= now - 1:
            _, old_size = self.recent.popleft()
            self.recent_bytes -= old_size
        self.recent.append((now, size))
        self.recent_bytes += size
        return len(self.recent) <= MAX_PACKETS_PER_SECOND and self.recent_bytes <= MAX_BYTES_PER_SECOND


class RoomRelay:
    def __init__(self):
        self.rooms = {}

    @staticmethod
    def queue(peer, message):
        if peer.overloaded:
            return
        try:
            peer.outgoing.put_nowait(message)
        except asyncio.QueueFull:
            peer.overloaded = True
            asyncio.create_task(peer.socket.close(1013, "receiver too slow"))

    @staticmethod
    async def writer(peer):
        while True:
            await peer.socket.send(await peer.outgoing.get())

    async def handler(self, socket):
        request = urlsplit(socket.request.path)
        match = ROOM_PATH.fullmatch(request.path)
        if not match:
            await socket.close(1008, "invalid room path")
            return
        if len(request.query) > 256:
            await socket.close(1008, "invalid player name")
            return
        names = parse_qs(request.query, keep_blank_values=True).get("name", [])
        if len(names) > 1 or (names and (not names[0].strip() or len(names[0].strip()) > MAX_NAME_CHARS
                                  or any(ord(ch) < 32 or ord(ch) == 127 for ch in names[0]))):
            await socket.close(1008, "invalid player name")
            return
        code = match.group(1)
        room = self.rooms.setdefault(code, {})
        if len(room) >= MAX_PEERS:
            await socket.close(1013, "room full")
            return
        slot = next(i for i in range(1, MAX_PEERS + 1) if i not in room)
        peer = Peer(socket, slot, names[0].strip() if names else f"Jogador {slot}")
        existing = list(room)
        existing_names = {str(other_slot): other.name for other_slot, other in room.items()}
        room[slot] = peer
        writer = asyncio.create_task(self.writer(peer))
        self.queue(peer, json.dumps({"type": "welcome", "version": 1, "slot": slot, "peers": existing,
                                     "names": existing_names, "name": peer.name}))
        for other in room.values():
            if other is not peer:
                self.queue(other, json.dumps({"type": "peer_joined", "slot": slot, "name": peer.name}))
        try:
            async for packet in socket:
                if not isinstance(packet, bytes):
                    await socket.close(1003, "binary packets only")
                    break
                if not packet:
                    await socket.close(1008, "empty packet")
                    break
                if not peer.within_rate(len(packet)):
                    await socket.close(1008, "packet rate exceeded")
                    break
                for other in room.values():
                    if other is not peer:
                        self.queue(other, packet)
        except ConnectionClosed:
            pass
        finally:
            if room.get(slot) is peer:
                del room[slot]
                for other in room.values():
                    self.queue(other, json.dumps({"type": "peer_left", "slot": slot}))
                if not room:
                    del self.rooms[code]
            writer.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await writer


async def run(host, port, origins):
    relay = RoomRelay()
    async with serve(relay.handler, host, port, origins=origins, compression=None,
                     max_size=MAX_PACKET, max_queue=8, write_limit=32768,
                     ping_interval=30, ping_timeout=60):
        print(f"room relay on ws://{host}:{port}/v1/rooms/<code>", flush=True)
        await asyncio.Future()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8621)
    parser.add_argument("--origin", action="append", help="allowed browser Origin; repeat for more")
    args = parser.parse_args()
    local = args.host in ("127.0.0.1", "localhost", "::1")
    if not local and not args.origin:
        parser.error("a non-loopback listener requires at least one --origin")
    origins = args.origin if args.origin else [None, "http://127.0.0.1:8620", "http://localhost:8620",
                                               "http://127.0.0.1:8630", "http://localhost:8630"]
    asyncio.run(run(args.host, args.port, origins))


if __name__ == "__main__":
    main()
