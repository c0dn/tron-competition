/*
 * app.js — MIND live monitor.
 *
 * Subscribes to the broker over MQTT/WebSocket, decodes the binary uplink
 * records, and renders two live panels (wearables + ESP32-C3 nodes) plus an
 * incident feed. `mqtt` is the global from vendor/mqtt.min.js.
 */
import { decodeRecord, eventTypeInfo } from './binaryReader.js';

const STALE_MS = 15000;   /* no update within this -> mark offline */
const FEED_MAX = 60;

/* -------- state -------- */
const wearables = new Map();  // deviceId -> record
const nodes = new Map();      // nodeId  -> record
const feed = [];              // newest first
let client = null;

/* -------- dom helpers -------- */
const $ = (id) => document.getElementById(id);
const now = () => Date.now();
const fmtAge = (ms) => ms < 1000 ? 'now' : ms < 60000 ? `${(ms/1000)|0}s ago` : `${(ms/60000)|0}m ago`;
const fmtUptime = (s) => s < 60 ? `${s}s` : s < 3600 ? `${(s/60)|0}m` : `${(s/3600)|0}h ${((s%3600)/60)|0}m`;

/* -------- connection -------- */
function defaultBrokerUrl() {
  const host = location.hostname || 'localhost';
  return `ws://${host}:9001`;
}

function connect() {
  const url = $('broker').value.trim() || defaultBrokerUrl();
  localStorage.setItem('mind_broker', url);
  setStatus('connecting', `connecting to ${url}…`);

  if (client) { try { client.end(true); } catch (_) {} }
  client = mqtt.connect(url, { reconnectPeriod: 2000, connectTimeout: 8000 });

  client.on('connect', () => {
    setStatus('online', `connected to ${url}`);
    client.subscribe('mind/ingest/#');
    client.subscribe('mind/node/+/lwt');
  });
  client.on('reconnect', () => setStatus('connecting', 'reconnecting…'));
  client.on('close', () => setStatus('offline', 'disconnected'));
  client.on('error', (e) => setStatus('offline', `error: ${e.message}`));
  client.on('message', onMessage);
}

function setStatus(cls, text) {
  const el = $('conn');
  el.className = `conn ${cls}`;
  el.textContent = text;
}

/* -------- message handling -------- */
function onMessage(topic, payload) {
  if (topic.startsWith('mind/ingest/')) {
    let rec;
    try { rec = decodeRecord(payload); }
    catch (e) { console.warn('decode failed', e.message); return; }
    if (rec.kind === 'event') onEvent(rec);
    else if (rec.kind === 'status') onStatus(rec);
    return;
  }
  const m = topic.match(/^mind\/node\/(\d+)\/lwt$/);
  if (m) {
    const id = Number(m[1]);
    const n = nodes.get(id) || { nodeId: id };
    n.rootPresence = payload.toString();   // 'online' | 'offline'
    nodes.set(id, n);
    render();
  }
}

function onEvent(rec) {
  const w = wearables.get(rec.deviceId) || { deviceId: rec.deviceId };
  Object.assign(w, rec, { lastSeen: now() });
  wearables.set(rec.deviceId, w);

  const info = eventTypeInfo(rec.eventType);
  if (info.severity >= 2) {
    feed.unshift({
      ts: now(), deviceId: rec.deviceId, nodeId: rec.nodeId,
      name: info.name, severity: info.severity,
      confidence: rec.confidence, rssi: rec.rssi,
    });
    feed.length = Math.min(feed.length, FEED_MAX);
  }
  render();
}

function onStatus(rec) {
  const n = nodes.get(rec.nodeId) || { nodeId: rec.nodeId };
  Object.assign(n, rec, { lastSeen: now() });
  nodes.set(rec.nodeId, n);
  render();
}

/* -------- rendering -------- */
function nodeOnline(n) {
  if (n.isRoot && n.rootPresence === 'offline') return false;
  if (n.lastSeen && now() - n.lastSeen < STALE_MS) return true;
  return false;
}

function render() {
  renderWearables();
  renderNodes();
  renderFeed();
}

function renderWearables() {
  const host = $('wearables');
  const list = [...wearables.values()].sort((a, b) => a.deviceId - b.deviceId);
  $('wearable-count').textContent = list.length;
  host.innerHTML = list.map((w) => {
    const info = eventTypeInfo(w.eventType);
    const stale = !(w.lastSeen && now() - w.lastSeen < STALE_MS);
    return `
      <div class="card sev${info.severity} ${stale ? 'stale' : ''}">
        <div class="card-top">
          <span class="id">wearable #${w.deviceId}</span>
          <span class="badge sev${info.severity}">${info.name}</span>
        </div>
        <div class="metric-row">
          <div class="metric"><span>conf</span><b>${w.confidence ?? '–'}</b></div>
          <div class="metric"><span>SVM</span><b>${w.accelSvm ?? '–'}<small>mg</small></b></div>
          <div class="metric"><span>mic</span><b>${w.micLevel ?? '–'}</b></div>
        </div>
        <div class="card-foot">
          <span>via node #${w.nodeId ?? '?'} · ${w.rssi ?? '?'} dBm</span>
          <span>${w.lastSeen ? fmtAge(now() - w.lastSeen) : '—'}</span>
        </div>
      </div>`;
  }).join('') || '<div class="empty">waiting for wearable beacons…</div>';
}

function renderNodes() {
  const host = $('nodes');
  const list = [...nodes.values()].sort((a, b) => a.nodeId - b.nodeId);
  $('node-count').textContent = list.length;
  host.innerHTML = list.map((n) => {
    const online = nodeOnline(n);
    return `
      <div class="card node ${online ? '' : 'stale'}">
        <div class="card-top">
          <span class="id">node #${n.nodeId} ${n.isRoot ? '<span class="root">ROOT</span>' : ''}</span>
          <span class="dot ${online ? 'up' : 'down'}"></span>
        </div>
        <div class="metric-row">
          <div class="metric"><span>level</span><b>${n.meshLevel ?? '–'}</b></div>
          <div class="metric"><span>children</span><b>${n.childCount ?? '–'}</b></div>
          <div class="metric"><span>parent</span><b>${n.parentRssi ?? '–'}<small>dBm</small></b></div>
        </div>
        <div class="card-foot">
          <span>heap ${n.freeHeap ? (n.freeHeap/1024|0)+' KB' : '–'} · up ${n.uptimeS != null ? fmtUptime(n.uptimeS) : '–'}</span>
          <span>${n.lastSeen ? fmtAge(now() - n.lastSeen) : '—'}</span>
        </div>
      </div>`;
  }).join('') || '<div class="empty">waiting for nodes…</div>';
}

function renderFeed() {
  const host = $('feed');
  host.innerHTML = feed.map((f) => {
    const t = new Date(f.ts).toLocaleTimeString();
    return `<div class="feed-row sev${f.severity}">
        <span class="feed-time">${t}</span>
        <span class="feed-name">${f.name}</span>
        <span class="feed-meta">wearable #${f.deviceId} · conf ${f.confidence} · via #${f.nodeId} · ${f.rssi} dBm</span>
      </div>`;
  }).join('') || '<div class="empty">no incidents yet</div>';
}

/* -------- boot -------- */
$('broker').value = localStorage.getItem('mind_broker') || defaultBrokerUrl();
$('connect').addEventListener('click', connect);
setInterval(render, 1000);   // keep ages / offline state fresh
connect();
