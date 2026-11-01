// Small per-player LAN service for the opt-in SMAP bridge. Gameplay frames
// still travel through the room relay; DHCP and gateway ARP stay local.
const SERVER_MAC = Uint8Array.of(0x02, 0, 0, 0, 0xff, 1);
const SERVER_IP = Uint8Array.of(10, 0, 0, 1);
const BROADCAST_IP = Uint8Array.of(255, 255, 255, 255);

function u16(bytes, at) { return (bytes[at] << 8) | bytes[at + 1]; }
function put16(bytes, at, value) { bytes[at] = value >> 8; bytes[at + 1] = value & 255; }
function equal(bytes, at, expected) { return expected.every((value, i) => bytes[at + i] === value); }

function checksum(bytes) {
  let sum = 0;
  for (let i = 0; i < bytes.length; i += 2) sum += (bytes[i] << 8) | (bytes[i + 1] || 0);
  while (sum >>> 16) sum = (sum & 0xffff) + (sum >>> 16);
  return (~sum) & 0xffff;
}

function dhcpOptions(payload) {
  const options = new Map();
  for (let at = 240; at < payload.length;) {
    const code = payload[at++];
    if (code === 255) break;
    if (code === 0) continue;
    if (at >= payload.length || at + 1 + payload[at] > payload.length) return null;
    const length = payload[at++];
    options.set(code, payload.slice(at, at + length));
    at += length;
  }
  return options;
}

function makeDhcpReply(payload, options, clientIp, replyType) {
  const bootp = payload.slice(0, 236);
  bootp[0] = 2;
  bootp.set(clientIp, 16);
  bootp.set(SERVER_IP, 20);
  const values = [99, 130, 83, 99, 53, 1, replyType, 54, 4, ...SERVER_IP,
    51, 4, 0, 0, 14, 16, 1, 4, 255, 255, 255, 0,
    3, 4, ...SERVER_IP, 6, 4, ...SERVER_IP];
  const clientId = options.get(61);
  if (clientId) values.push(61, clientId.length, ...clientId);
  values.push(255);
  const dhcp = new Uint8Array(Math.max(300, bootp.length + values.length));
  dhcp.set(bootp);
  dhcp.set(values, 236);
  const udpLength = 8 + dhcp.length;
  const packet = new Uint8Array(14 + 20 + udpLength);
  packet.fill(255, 0, 6);
  packet.set(SERVER_MAC, 6);
  put16(packet, 12, 0x0800);
  packet.set([0x45, 0, 0, 0, 0, 2, 0, 0, 64, 17, 0, 0], 14);
  put16(packet, 16, 20 + udpLength);
  packet.set(SERVER_IP, 26);
  packet.set(BROADCAST_IP, 30);
  put16(packet, 24, checksum(packet.subarray(14, 34)));
  put16(packet, 34, 67);
  put16(packet, 36, 68);
  put16(packet, 38, udpLength);
  packet.set(dhcp, 42);
  const pseudo = new Uint8Array(12 + udpLength);
  pseudo.set(SERVER_IP);
  pseudo.set(BROADCAST_IP, 4);
  pseudo[9] = 17;
  put16(pseudo, 10, udpLength);
  pseudo.set(packet.subarray(34), 12);
  put16(packet, 40, checksum(pseudo) || 0xffff);
  return packet;
}

export class VirtualLan {
  constructor(slot) {
    if (!Number.isInteger(slot) || slot < 1 || slot > 8) throw new RangeError('Invalid room slot');
    this.slot = slot;
    this.clientIp = Uint8Array.of(10, 0, 0, 9 + slot);
  }

  respond(frame, peerSlots = new Set()) {
    const bytes = frame instanceof Uint8Array ? frame : new Uint8Array(frame);
    if (bytes.length < 14 || bytes.length > 1518) return { handled: false };
    if (u16(bytes, 12) === 0x0806 && bytes.length >= 42
        && u16(bytes, 14) === 1 && u16(bytes, 16) === 0x0800
        && bytes[18] === 6 && bytes[19] === 4 && u16(bytes, 20) === 1) {
      const targetIp = bytes.subarray(38, 42);
      const gateway = equal(bytes, 38, SERVER_IP);
      const peerSlot = targetIp[0] === 10 && targetIp[1] === 0 && targetIp[2] === 0
        ? targetIp[3] - 9 : 0;
      if (!gateway && (peerSlot < 1 || peerSlot > 8 || !peerSlots.has(peerSlot)))
        return { handled: false };
      const senderMac = gateway ? SERVER_MAC : Uint8Array.of(2, 0, 0, 0, 0, peerSlot);
      const reply = new Uint8Array(Math.max(60, bytes.length));
      reply.set(bytes);
      reply.set(bytes.subarray(6, 12), 0);
      reply.set(senderMac, 6);
      put16(reply, 20, 2);
      reply.set(senderMac, 22);
      reply.set(targetIp, 28);
      reply.set(bytes.subarray(22, 28), 32);
      reply.set(bytes.subarray(28, 32), 38);
      return { handled: true, response: reply };
    }
    if (u16(bytes, 12) !== 0x0800 || bytes.length < 14 + 20 + 8 + 240) return { handled: false };
    const ipLength = (bytes[14] & 15) * 4;
    const udp = 14 + ipLength;
    if ((bytes[14] >> 4) !== 4 || ipLength < 20 || bytes[23] !== 17
        || bytes.length < udp + 8 + 240 || u16(bytes, udp) !== 68 || u16(bytes, udp + 2) !== 67)
      return { handled: false };
    const payload = bytes.subarray(udp + 8);
    if (payload[0] !== 1 || !equal(payload, 236, [99, 130, 83, 99])) return { handled: false };
    const options = dhcpOptions(payload);
    if (!options) return { handled: true };
    const type = options.get(53)?.[0];
    if (type !== 1 && type !== 3) return { handled: true };
    return { handled: true, response: makeDhcpReply(payload, options, this.clientIp, type === 1 ? 2 : 5) };
  }
}
