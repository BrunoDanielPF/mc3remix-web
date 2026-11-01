import assert from 'node:assert/strict';
import test from 'node:test';
import { VirtualLan } from '../web/virtual-lan.mjs';

function request(type) {
  const frame = new Uint8Array(342);
  frame.fill(255, 0, 6);
  frame.set([2, 0, 0, 0, 0, 2], 6);
  frame.set([8, 0, 0x45], 12);
  frame[23] = 17;
  frame.set([0, 68, 0, 67], 34);
  frame[42] = 1;
  frame[43] = 1;
  frame[44] = 6;
  frame.set([0x12, 0x34, 0x56, 0x78], 46);
  frame.set([2, 0, 0, 0, 0, 2], 70);
  frame.set([99, 130, 83, 99, 53, 1, type, 61, 7, 1, 2, 0, 0, 0, 0, 2, 255], 278);
  return frame;
}

function sum(bytes) {
  let n = 0;
  for (let i = 0; i < bytes.length; i += 2) n += (bytes[i] << 8) | (bytes[i + 1] || 0);
  while (n >>> 16) n = (n & 65535) + (n >>> 16);
  return n;
}

test('DHCP assigns a distinct address and preserves transaction and client ID', () => {
  const lan = new VirtualLan(2);
  for (const [type, expectedReply] of [[1, 2], [3, 5]]) {
    const result = lan.respond(request(type));
    assert.equal(result.handled, true);
    const frame = result.response;
    assert.deepEqual([...frame.slice(58, 62)], [10, 0, 0, 11]);
    assert.deepEqual([...frame.slice(46, 50)], [0x12, 0x34, 0x56, 0x78]);
    assert.deepEqual([...frame.slice(278, 285)], [99, 130, 83, 99, 53, 1, expectedReply]);
    assert.equal(frame.includes(61), true);
    assert.equal(sum(frame.slice(14, 34)), 65535);
    const udp = new Uint8Array(12 + frame.length - 34);
    udp.set(frame.slice(26, 34));
    udp[9] = 17;
    udp[10] = frame[38]; udp[11] = frame[39];
    udp.set(frame.slice(34), 12);
    assert.equal(sum(udp), 65535);
  }
  assert.equal(lan.respond(request(7)).handled, true);
  assert.equal(lan.respond(request(7)).response, undefined);
});

test('ARP gateway reply targets the requester and leaves peer frames for relay', () => {
  const lan = new VirtualLan(2);
  const arp = new Uint8Array(60);
  arp.fill(255, 0, 6);
  arp.set([2, 0, 0, 0, 0, 2, 8, 6, 0, 1, 8, 0, 6, 4, 0, 1], 6);
  arp.set([2, 0, 0, 0, 0, 2, 10, 0, 0, 11], 22);
  arp.set([10, 0, 0, 1], 38);
  const reply = lan.respond(arp);
  assert.equal(reply.handled, true);
  assert.deepEqual([...reply.response.slice(0, 6)], [2, 0, 0, 0, 0, 2]);
  assert.deepEqual([...reply.response.slice(6, 12)], [2, 0, 0, 0, 255, 1]);
  assert.deepEqual([...reply.response.slice(20, 22)], [0, 2]);
  assert.deepEqual([...reply.response.slice(28, 32)], [10, 0, 0, 1]);
  arp.set([10, 0, 0, 12], 38);
  assert.equal(lan.respond(arp).handled, false);
});

test('ARP proxy resolves only peers present in the same room', () => {
  const lan = new VirtualLan(1);
  const arp = new Uint8Array(42);
  arp.fill(255, 0, 6);
  arp.set([2, 0, 0, 0, 0, 1, 8, 6, 0, 1, 8, 0, 6, 4, 0, 1], 6);
  arp.set([2, 0, 0, 0, 0, 1, 10, 0, 0, 10], 22);
  arp.set([10, 0, 0, 11], 38);
  assert.equal(lan.respond(arp, new Set()).handled, false);
  const result = lan.respond(arp, new Set([2]));
  assert.equal(result.handled, true);
  const reply = result.response;
  assert.deepEqual([...reply.slice(0, 6)], [2, 0, 0, 0, 0, 1]);
  assert.deepEqual([...reply.slice(6, 12)], [2, 0, 0, 0, 0, 2]);
  assert.deepEqual([...reply.slice(20, 32)], [0, 2, 2, 0, 0, 0, 0, 2, 10, 0, 0, 11]);
  assert.deepEqual([...reply.slice(32, 42)], [2, 0, 0, 0, 0, 1, 10, 0, 0, 10]);
});
