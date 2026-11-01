// Transport only. The PS2 LAN adapter must supply/consume the binary packets.
export class RoomClient extends EventTarget {
  constructor(url, roomCode, playerName = '') {
    super();
    if (!/^[0-9a-f]{32}$/.test(roomCode)) throw new Error('Invalid room code');
    const name = playerName.trim();
    if (name && (name.length > 24 || [...name].some(ch => ch.charCodeAt(0) < 32 || ch.charCodeAt(0) === 127)))
      throw new Error('Invalid player name');
    this.socket = new WebSocket(`${url.replace(/\/$/, '')}/v1/rooms/${roomCode}`
      + (name ? `?name=${encodeURIComponent(name)}` : ''));
    this.socket.binaryType = 'arraybuffer';
    this.slot = null;
    this.name = name;
    this.peers = new Set();
    this.names = new Map();
    this.ready = new Promise((resolve, reject) => {
      let welcomed = false;
      this.socket.addEventListener('message', event => {
        if (event.data instanceof ArrayBuffer) {
          this.dispatchEvent(new CustomEvent('packet', { detail: event.data }));
          return;
        }
        let message;
        try { message = JSON.parse(event.data); } catch { return; }
        if (message.type === 'welcome' && message.version === 1) {
          this.slot = message.slot;
          this.name = message.name || name;
          this.peers = new Set(message.peers);
          this.names = new Map(Object.entries(message.names || {}).map(([slot, peerName]) => [Number(slot), peerName]));
          welcomed = true;
          resolve(this);
        } else if (message.type === 'peer_joined') {
          this.peers.add(message.slot);
          this.names.set(message.slot, message.name || `Jogador ${message.slot}`);
        } else if (message.type === 'peer_left') {
          this.peers.delete(message.slot);
          this.names.delete(message.slot);
        }
        this.dispatchEvent(new CustomEvent('room', { detail: message }));
      });
      this.socket.addEventListener('error', () => {
        if (!welcomed) reject(new Error('Room connection failed'));
      });
      this.socket.addEventListener('close', event => {
        if (!welcomed) reject(new Error(`Room closed: ${event.code} ${event.reason}`));
        this.dispatchEvent(new CustomEvent('close', { detail: event }));
      });
    });
  }

  send(packet) {
    if (this.socket.readyState !== WebSocket.OPEN) throw new Error('Room is not connected');
    const size = packet.byteLength;
    if (size < 1 || size > 65536) throw new RangeError('Packet must be 1..65536 bytes');
    this.socket.send(packet);
  }

  close() { this.socket.close(); }
}

export function newRoomCode() {
  return Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');
}
