"""Integration tests for the standalone LAN packet relay."""

import asyncio
import json
import logging
import sys
import unittest
from pathlib import Path

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed, InvalidStatus

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from room_relay import MAX_PACKET, RoomRelay  # noqa: E402


class RelayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.relay = RoomRelay()
        logger = logging.getLogger("room-relay-tests")
        logger.addHandler(logging.NullHandler())
        logger.propagate = False
        self.server = await serve(self.relay.handler, "127.0.0.1", 0,
                                  origins=[None, "http://127.0.0.1:8620"],
                                  compression=None, max_size=MAX_PACKET, logger=logger)
        self.port = self.server.sockets[0].getsockname()[1]
        self.clients = []

    async def asyncTearDown(self):
        await asyncio.gather(*(client.close() for client in self.clients))
        self.server.close()
        await self.server.wait_closed()

    async def join(self, code, name=None):
        suffix = f"?name={name}" if name is not None else ""
        client = await connect(f"ws://127.0.0.1:{self.port}/v1/rooms/{code}{suffix}", proxy=None)
        self.clients.append(client)
        welcome = json.loads(await asyncio.wait_for(client.recv(), 1))
        self.assertEqual(welcome["type"], "welcome")
        self.assertEqual(welcome["version"], 1)
        return client, welcome

    async def test_room_isolation_binary_and_disconnect(self):
        room_a = "a" * 32
        room_b = "b" * 32
        first, one = await self.join(room_a)
        second, two = await self.join(room_a)
        third, three = await self.join(room_b)
        self.assertEqual((one["slot"], two["slot"], three["slot"]), (1, 2, 1))
        self.assertEqual(two["peers"], [1])
        self.assertEqual(json.loads(await first.recv()), {"type": "peer_joined", "slot": 2,
                                                        "name": "Jogador 2"})
        packet = b"\x00\xffMC3 opaque packet"
        await first.send(packet)
        self.assertEqual(await asyncio.wait_for(second.recv(), 1), packet)
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(third.recv(), 0.1)
        await second.close()
        self.assertEqual(json.loads(await first.recv()), {"type": "peer_left", "slot": 2})
        self.assertEqual(set(self.relay.rooms), {room_a, room_b})
        await first.close()
        await third.close()
        for _ in range(20):
            if not self.relay.rooms:
                break
            await asyncio.sleep(0.01)
        self.assertFalse(self.relay.rooms)

    async def test_rejects_text_and_oversized_packets(self):
        client, _ = await self.join("c" * 32)
        await client.send("text is not a game packet")
        with self.assertRaises(ConnectionClosed) as closed:
            await client.recv()
        self.assertEqual(closed.exception.rcvd.code, 1003)
        oversized, _ = await self.join("d" * 32)
        await oversized.send(b"x" * (MAX_PACKET + 1))
        with self.assertRaises(ConnectionClosed) as closed:
            await oversized.recv()
        self.assertEqual(closed.exception.rcvd.code, 1009)

    async def test_room_limit(self):
        code = "e" * 32
        for _ in range(8):
            await self.join(code)
        extra = await connect(f"ws://127.0.0.1:{self.port}/v1/rooms/{code}", proxy=None)
        self.clients.append(extra)
        with self.assertRaises(ConnectionClosed) as closed:
            await extra.recv()
        self.assertEqual(closed.exception.rcvd.code, 1013)

    async def test_browser_origin(self):
        uri = f"ws://127.0.0.1:{self.port}/v1/rooms/{'f' * 32}"
        allowed = await connect(uri, origin="http://127.0.0.1:8620", proxy=None)
        self.clients.append(allowed)
        self.assertEqual(json.loads(await allowed.recv())["type"], "welcome")
        with self.assertRaises(InvalidStatus) as rejected:
            await connect(uri, origin="https://untrusted.example", proxy=None)
        self.assertEqual(rejected.exception.response.status_code, 403)

    async def test_player_name_without_account(self):
        first, one = await self.join("1" * 32, "Ana")
        second, two = await self.join("1" * 32, "Beto")
        self.assertEqual((one["name"], two["name"]), ("Ana", "Beto"))
        self.assertEqual(two["names"], {"1": "Ana"})
        self.assertEqual(json.loads(await first.recv()), {"type": "peer_joined", "slot": 2,
                                                        "name": "Beto"})
        await second.close()
        self.assertEqual(json.loads(await first.recv()), {"type": "peer_left", "slot": 2})
        invalid = await connect(f"ws://127.0.0.1:{self.port}/v1/rooms/{'2' * 32}?name=", proxy=None)
        self.clients.append(invalid)
        with self.assertRaises(ConnectionClosed) as closed:
            await invalid.recv()
        self.assertEqual(closed.exception.rcvd.code, 1008)


if __name__ == "__main__":
    unittest.main()
