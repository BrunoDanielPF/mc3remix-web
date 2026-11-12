#!/usr/bin/env python3
"""Room discovery and opaque binary packet relay for the browser game.

/v2/network lists, creates, and joins public or password protected rooms.
The original /v1/rooms/<code> route remains for older diagnostic clients.
The relay never sees game assets and never simulates the game.
"""

import argparse
import asyncio
import contextlib
import hashlib
import hmac
import json
import re
import secrets
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
MAX_ROOM_NAME_CHARS = 40
MAX_GAME_PLAYERS = 4
MAX_ACTIVE_GAME_ROOMS = 256
NETWORK_PATH = "/v2/network"


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


@dataclass
class GameRoom:
    code: str
    title: str
    capacity: int
    private: bool
    salt: bytes
    password_hash: bytes
    peers: dict = field(default_factory=dict)
    ready: bool = False

    def summary(self):
        return {"id": self.code, "name": self.title, "private": self.private,
                "players": len(self.peers), "capacity": self.capacity,
                "state": "ready" if self.ready else "preparing"}

    def accepts_password(self, password):
        if not self.private:
            return True
        return isinstance(password, str) and hmac.compare_digest(
            self.password_hash, hashlib.sha256(self.salt + password.encode("utf-8")).digest())


class RoomRelay:
    def __init__(self):
        self.rooms = {}
        self.game_rooms = {}
        self.browsers = set()

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

    def room_list(self):
        return {"type": "rooms", "version": 2,
                "rooms": [room.summary() for room in self.game_rooms.values()]}

    async def publish_rooms(self):
        if not self.browsers:
            return
        message = json.dumps(self.room_list())
        for browser in tuple(self.browsers):
            try:
                await asyncio.wait_for(browser.send(message), 1)
            except (ConnectionClosed, TimeoutError):
                self.browsers.discard(browser)

    @staticmethod
    async def command_error(socket, code):
        await socket.send(json.dumps({"type": "error", "code": code}))

    @staticmethod
    def valid_name(value, limit):
        return (isinstance(value, str) and 1 <= len(value.strip()) <= limit
                and not any(ord(ch) < 32 or ord(ch) == 127 for ch in value))

    async def network_handler(self, socket):
        self.browsers.add(socket)
        await socket.send(json.dumps(self.room_list()))
        peer = None
        room = None
        writer = None
        bad_password_attempts = 0
        try:
            async for packet in socket:
                if isinstance(packet, bytes):
                    if peer is None or not packet:
                        await socket.close(1008, "not in a room")
                        break
                    if not peer.within_rate(len(packet)):
                        await socket.close(1008, "packet rate exceeded")
                        break
                    for other in room.peers.values():
                        if other is not peer:
                            self.queue(other, packet)
                    continue
                try:
                    command = json.loads(packet)
                except (ValueError, TypeError):
                    await socket.close(1008, "invalid command")
                    break
                if not isinstance(command, dict) or not isinstance(command.get("type"), str):
                    await socket.close(1008, "invalid command")
                    break
                action = command["type"]
                if action == "list" and peer is None:
                    await socket.send(json.dumps(self.room_list()))
                    continue
                if action == "ready" and peer is not None and peer.slot == 1:
                    if not room.ready:
                        room.ready = True
                        for other in room.peers.values():
                            self.queue(other, json.dumps({"type": "room_ready"}))
                        await self.publish_rooms()
                    continue
                if peer is not None:
                    await socket.close(1008, "already in a room")
                    break
                player_name = command.get("playerName")
                if action not in ("create", "join") or not self.valid_name(player_name, MAX_NAME_CHARS):
                    await self.command_error(socket, "invalid_player")
                    continue
                if action == "create":
                    title = command.get("name")
                    capacity = command.get("capacity")
                    visibility = command.get("visibility")
                    password = command.get("password", "")
                    if (not self.valid_name(title, MAX_ROOM_NAME_CHARS)
                            or type(capacity) is not int or not 2 <= capacity <= MAX_GAME_PLAYERS
                            or visibility not in ("public", "private")
                            or not isinstance(password, str)
                            or (visibility == "private" and (not isinstance(password, str)
                                or not 4 <= len(password) <= 32
                                or any(ord(ch) < 32 or ord(ch) == 127 for ch in password)))
                            or (visibility == "public" and password)):
                        await self.command_error(socket, "invalid_room")
                        continue
                    if len(self.game_rooms) >= MAX_ACTIVE_GAME_ROOMS:
                        await self.command_error(socket, "server_full")
                        continue
                    code = secrets.token_hex(16)
                    salt = secrets.token_bytes(16)
                    digest = hashlib.sha256(salt + password.encode("utf-8")).digest() if password else b""
                    room = GameRoom(code, title.strip(), capacity, visibility == "private", salt, digest)
                    self.game_rooms[code] = room
                    slot = 1
                else:
                    code = command.get("roomId")
                    room = self.game_rooms.get(code) if isinstance(code, str) else None
                    if room is None:
                        await self.command_error(socket, "room_gone")
                        continue
                    if not room.accepts_password(command.get("password", "")):
                        bad_password_attempts += 1
                        await self.command_error(socket, "bad_password")
                        if bad_password_attempts >= 5:
                            await socket.close(1008, "too many password attempts")
                            break
                        continue
                    if not room.ready:
                        await self.command_error(socket, "room_preparing")
                        continue
                    if len(room.peers) >= room.capacity:
                        await self.command_error(socket, "room_full")
                        continue
                    slot = next(i for i in range(1, room.capacity + 1) if i not in room.peers)
                existing = list(room.peers)
                names = {str(number): other.name for number, other in room.peers.items()}
                peer = Peer(socket, slot, player_name.strip())
                room.peers[slot] = peer
                self.browsers.discard(socket)
                writer = asyncio.create_task(self.writer(peer))
                self.queue(peer, json.dumps({"type": "joined", "version": 2, "room": room.summary(),
                                             "slot": slot, "peers": existing, "names": names,
                                             "name": peer.name, "host": slot == 1}))
                for other in room.peers.values():
                    if other is not peer:
                        self.queue(other, json.dumps({"type": "peer_joined", "slot": slot,
                                                      "name": peer.name}))
                await self.publish_rooms()
        except ConnectionClosed:
            pass
        finally:
            self.browsers.discard(socket)
            if room is not None and peer is not None and room.peers.get(peer.slot) is peer:
                del room.peers[peer.slot]
                if peer.slot == 1:
                    self.game_rooms.pop(room.code, None)
                    for other in room.peers.values():
                        asyncio.create_task(other.socket.close(1012, "host left"))
                else:
                    for other in room.peers.values():
                        self.queue(other, json.dumps({"type": "peer_left", "slot": peer.slot}))
                await self.publish_rooms()
            if writer is not None:
                writer.cancel()
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await writer

    async def handler(self, socket):
        request = urlsplit(socket.request.path)
        if request.path == NETWORK_PATH and not request.query:
            await self.network_handler(socket)
            return
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
        print(f"room relay on ws://{host}:{port}/v2/network", flush=True)
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
