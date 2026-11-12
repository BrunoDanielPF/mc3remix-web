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
          this.names.set(message.slot, message.name || `Player ${message.slot}`);
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

// Browser lobby control and game frames share one authenticated socket.
export class MultiplayerClient extends EventTarget {
  constructor(origin) {
    super();
    this.socket = new WebSocket(`${origin.replace(/\/$/, '')}/v2/network`);
    this.socket.binaryType = 'arraybuffer';
    this.slot = null;
    this.peers = new Set();
    this.names = new Map();
    this.room = null;
    this.host = false;
    this.pending = null;
    this.ready = new Promise((resolve, reject) => {
      let opened = false;
      this.socket.addEventListener('message', event => {
        if (event.data instanceof ArrayBuffer) {
          this.dispatchEvent(new CustomEvent('packet', { detail: event.data }));
          return;
        }
        let message;
        try { message = JSON.parse(event.data); } catch { return; }
        if (message.type === 'rooms' && message.version === 2) {
          opened = true;
          resolve(this);
          this.dispatchEvent(new CustomEvent('rooms', { detail: message.rooms }));
        } else if (message.type === 'joined' && message.version === 2) {
          this.slot = message.slot;
          this.room = message.room;
          this.host = message.host;
          this.name = message.name;
          this.peers = new Set(message.peers);
          this.names = new Map(Object.entries(message.names || {}).map(([slot, name]) => [Number(slot), name]));
          this.pending?.resolve(message);
          this.pending = null;
          this.dispatchEvent(new CustomEvent('joined', { detail: message }));
        } else if (message.type === 'error') {
          this.pending?.reject(new Error(message.code || 'room_error'));
          this.pending = null;
          this.dispatchEvent(new CustomEvent('room_error', { detail: message.code }));
        } else if (message.type === 'peer_joined') {
          this.peers.add(message.slot);
          this.names.set(message.slot, message.name);
          this.dispatchEvent(new CustomEvent('room', { detail: message }));
        } else if (message.type === 'peer_left') {
          this.peers.delete(message.slot);
          this.names.delete(message.slot);
          this.dispatchEvent(new CustomEvent('room', { detail: message }));
        } else if (message.type === 'room_ready') {
          if (this.room) this.room.state = 'ready';
          this.dispatchEvent(new CustomEvent('room', { detail: message }));
        }
      });
      this.socket.addEventListener('error', () => {
        if (!opened) reject(new Error('connection_failed'));
      });
      this.socket.addEventListener('close', event => {
        if (!opened) reject(new Error('connection_closed'));
        this.pending?.reject(new Error('connection_closed'));
        this.pending = null;
        this.slot = null;
        this.dispatchEvent(new CustomEvent('close', { detail: event }));
      });
    });
  }

  command(message) {
    if (this.pending || this.slot || this.socket.readyState !== WebSocket.OPEN)
      return Promise.reject(new Error('connection_unavailable'));
    return new Promise((resolve, reject) => {
      this.pending = { resolve, reject };
      this.socket.send(JSON.stringify(message));
    });
  }

  create(options) { return this.command({ type: 'create', ...options }); }
  join(roomId, playerName, password = '') {
    return this.command({ type: 'join', roomId, playerName, password });
  }
  markReady() {
    if (!this.host || this.socket.readyState !== WebSocket.OPEN) return;
    this.socket.send(JSON.stringify({ type: 'ready' }));
  }
  send(packet) {
    if (!this.slot || this.socket.readyState !== WebSocket.OPEN)
      throw new Error('Room is not connected');
    if (packet.byteLength < 1 || packet.byteLength > 65536)
      throw new RangeError('Packet must be 1..65536 bytes');
    this.socket.send(packet);
  }
  close() { this.socket.close(); }
}
