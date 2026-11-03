import { RoomClient, newRoomCode } from './room-client.js';

const field = (id) => document.getElementById(id);
const nameInput = field('player-name');
const codeInput = field('room-code');
const status = field('room-status');
const log = field('room-log');
const copyButton = field('room-copy');
const sendButton = field('room-send');
const params = new URLSearchParams(location.search);
const testPort = /^(localhost|127\.0\.0\.1)$/.test(location.hostname)
  && /^\d{2,5}$/.test(params.get('relay-port') || '') ? params.get('relay-port') : '8621';
const relay = location.protocol === 'https:' ? `wss://${location.host}` : `ws://${location.hostname}:${testPort}`;
let client = null;

nameInput.value = localStorage.getItem('mc3-player-name') || '';
codeInput.value = params.get('room') || '';

function record(message) {
  const item = document.createElement('div');
  item.textContent = `${new Date().toLocaleTimeString()}  ${message}`;
  log.append(item);
  if (log.childElementCount > 40) log.firstElementChild.remove();
  log.scrollTop = log.scrollHeight;
}

function setStatus(message, connected = false) {
  status.textContent = message;
  status.classList.toggle('connected', connected);
}

async function join(create) {
  if (create) codeInput.value = newRoomCode();
  const code = codeInput.value.trim().toLowerCase();
  const name = nameInput.value.trim();
  if (!/^[0-9a-f]{32}$/.test(code)) {
    setStatus('Informe um código de sala válido com 32 caracteres hexadecimais.');
    return;
  }
  if (!name) {
    setStatus('Informe seu nome antes de entrar na sala.');
    nameInput.focus();
    return;
  }
  client?.close();
  copyButton.disabled = true;
  sendButton.disabled = true;
  setStatus('Conectando ao relay…');
  try {
    const current = new RoomClient(relay, code, name);
    client = current;
    current.addEventListener('room', (event) => {
      if (client !== current) return;
      const others = current.peers.size;
      setStatus(`Conectado como ${current.name} · ${others} outro${others === 1 ? '' : 's'} piloto${others === 1 ? '' : 's'} online`, true);
      record(event.detail.type === 'welcome' ? `Sala aberta · slot ${current.slot}` : `Evento: ${event.detail.type}`);
    });
    current.addEventListener('packet', (event) => {
      if (client !== current) return;
      record(`Pacote recebido: ${new TextDecoder().decode(event.detail)}`);
    });
    current.addEventListener('close', () => {
      if (client !== current) return;
      copyButton.disabled = true;
      sendButton.disabled = true;
      setStatus('Desconectado. Entre novamente para continuar.');
      record('Conexão encerrada');
    });
    await current.ready;
    if (client !== current) return;
    localStorage.setItem('mc3-player-name', name);
    codeInput.value = code;
    copyButton.disabled = false;
    sendButton.disabled = false;
    const url = new URL(location.href);
    url.searchParams.set('room', code);
    history.replaceState(null, '', url);
  } catch (error) {
    setStatus(`Não foi possível conectar: ${error.message}`);
    record(`Falha: ${error.message}`);
  }
}

field('room-create').addEventListener('click', () => join(true));
field('room-join').addEventListener('click', () => join(false));
copyButton.addEventListener('click', async () => {
  if (!client?.slot) return;
  const invite = new URL('/room.html', location.origin);
  invite.searchParams.set('room', codeInput.value);
  try {
    await navigator.clipboard.writeText(invite.href);
    record('Convite copiado');
  } catch (error) {
    record(`Não foi possível copiar o convite: ${error.message}`);
  }
});
sendButton.addEventListener('click', () => {
  if (!client?.slot) return;
  try {
    client.send(new TextEncoder().encode(`MC3 relay test from slot ${client.slot} at ${Date.now()}`));
    record('Pacote de teste enviado');
  } catch (error) {
    record(`Falha ao enviar: ${error.message}`);
  }
});
