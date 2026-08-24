export type EventPath = 'local' | 'tavrn';
export type RootRole = 'leaf' | 'root';
export type CommandName = 'on' | 'off' | 'status' | 'gtt' | 'invalid';
export type CommandStatus = 'accepted' | 'duplicate' | 'busy' | 'malformed' | 'overflow' | 'rejected';
export type GttValueState = 'not_applicable' | 'known' | 'unknown';
export type GttFreshness = 'not_applicable' | 'active' | 'soft_stale' | 'hard_expired' | 'departed';
export type GttDeparted = 'not_applicable' | 'false' | 'true' | 'unknown';

export interface EventRecord {
  cursor: number;
  kind: 'event';
  device: number;
  now: number;
  root: string;
  wearable: number;
  packet: string;
  schema: 1;
  event: number;
  confidence: number;
  svm: number;
  mic: number;
  seq: number;
  observer: string;
  path: EventPath;
}

export interface RootStatus {
  now: number;
  local: string;
  node: number;
  role: RootRole;
  roots: number;
  announced: number;
  acked: number;
  rejected: number;
  pending: number;
  rootless_drop: number;
}

export interface HealthRoot extends RootStatus {
  cursor: number;
  kind: 'root';
}

export interface RootRecord extends RootStatus {
  cursor: number;
  kind: 'root';
  device: number;
}

export interface CommandRecord {
  cursor: number;
  kind: 'command';
  device: number;
  now: number;
  local: string;
  command: CommandName;
  status: CommandStatus;
}

export type StreamRecord = EventRecord | RootRecord | CommandRecord;

export interface EventsResponse {
  schema: 'mind.api.v1';
  gap: boolean;
  oldest_cursor: number;
  current_cursor: number;
  events: StreamRecord[];
}

export interface HealthDevice {
  device: number;
  path: string;
  connected: boolean;
  parse_errors: number;
  overlong_lines: number;
  reconnects: number;
  last_record_cursor: number;
  root: HealthRoot | null;
  gtt: GttSnapshot | null;
  owner_device: number;
}

export interface HealthResponse {
  schema: 'mind.health.v2';
  oldest_cursor: number;
  current_cursor: number;
  devices: HealthDevice[];
}

export interface GttEntry {
  index: number;
  adva: string;
  last: number;
  soft: number;
  hard: number;
  departed_deadline: number;
  serial: number;
  serial_state: GttValueState;
  hop: number;
  hop_state: GttValueState;
  freshness: GttFreshness;
  departed: GttDeparted;
}

export interface GttSnapshot {
  generation: number;
  completed_at_ms: number;
  query_at_ms: number;
  local: string;
  entry_count: number;
  nondeparted_count: number;
  entries: GttEntry[];
}

export interface RootCommandResponse {
  schema: 'mind.command.v1';
  accepted: true;
  device: number;
  command: 'on' | 'off';
}

export interface GttCommandResponse {
  schema: 'mind.command.v1';
  accepted: true;
  device: number;
  command: 'gtt';
}

interface ErrorResponse {
  schema: 'mind.error.v1';
  accepted: false;
  error: 'invalid_query' | 'invalid_json' | 'invalid_body' | 'unknown_device' | 'disconnected' | 'write_failed';
}

type JsonObject = Record<string, unknown>;

export class DecodeError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'DecodeError';
  }
}

export class ApiError extends Error {
  constructor(message: string, readonly status?: number) {
    super(message);
    this.name = 'ApiError';
  }
}

function object(value: unknown, context: string): JsonObject {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new DecodeError(`${context} must be an object.`);
  }
  return value as JsonObject;
}

function exactKeys(value: JsonObject, keys: readonly string[], context: string): void {
  const actual = Object.keys(value);
  if (actual.length !== keys.length || actual.some((key) => !keys.includes(key))) {
    throw new DecodeError(`${context} has unexpected or missing fields.`);
  }
}

function string(value: unknown, context: string): string {
  if (typeof value !== 'string') throw new DecodeError(`${context} must be a string.`);
  return value;
}

function integer(value: unknown, context: string, minimum = 0, maximum = Number.MAX_SAFE_INTEGER): number {
  if (!Number.isInteger(value) || typeof value !== 'number' || value < minimum || value > maximum) {
    throw new DecodeError(`${context} must be an integer in range.`);
  }
  return value;
}

function boolean(value: unknown, context: string): boolean {
  if (typeof value !== 'boolean') throw new DecodeError(`${context} must be a boolean.`);
  return value;
}

function enumValue<T extends string>(value: unknown, allowed: readonly T[], context: string): T {
  if (typeof value !== 'string' || !allowed.includes(value as T)) {
    throw new DecodeError(`${context} is invalid.`);
  }
  return value as T;
}

function literalNumber<T extends number>(value: unknown, allowed: readonly T[], context: string): T {
  if (typeof value !== 'number' || !allowed.includes(value as T)) {
    throw new DecodeError(`${context} is invalid.`);
  }
  return value as T;
}

function hex(value: unknown, length: number, context: string): string {
  const result = string(value, context);
  if (!new RegExp(`^[0-9a-f]{${length}}$`).test(result)) {
    throw new DecodeError(`${context} must be ${length} lowercase hexadecimal characters.`);
  }
  return result;
}

function rootStatus(value: unknown, context: string): RootStatus {
  const input = object(value, context);
  exactKeys(input, ['now', 'local', 'node', 'role', 'roots', 'announced', 'acked', 'rejected', 'pending', 'rootless_drop'], context);
  return {
    now: integer(input.now, `${context}.now`, 0, 0xffffffff),
    local: hex(input.local, 12, `${context}.local`),
    node: integer(input.node, `${context}.node`, 1, 6),
    role: enumValue(input.role, ['leaf', 'root'], `${context}.role`),
    roots: integer(input.roots, `${context}.roots`, 0, 255),
    announced: integer(input.announced, `${context}.announced`, 0, 255),
    acked: integer(input.acked, `${context}.acked`, 0, 255),
    rejected: integer(input.rejected, `${context}.rejected`, 0, 255),
    pending: integer(input.pending, `${context}.pending`, 0, 255),
    rootless_drop: integer(input.rootless_drop, `${context}.rootless_drop`, 0, 0xffffffff),
  };
}

function eventRecord(value: unknown): EventRecord {
  const input = object(value, 'event record');
  exactKeys(input, ['cursor', 'kind', 'device', 'now', 'root', 'wearable', 'packet', 'schema', 'event', 'confidence', 'svm', 'mic', 'seq', 'observer', 'path'], 'event record');
  const record: EventRecord = {
    cursor: integer(input.cursor, 'event.cursor', 1),
    kind: enumValue(input.kind, ['event'], 'event.kind'),
    device: integer(input.device, 'event.device'),
    now: integer(input.now, 'event.now', 0, 0xffffffff),
    root: hex(input.root, 12, 'event.root'),
    wearable: integer(input.wearable, 'event.wearable', 1, 254),
    packet: hex(input.packet, 6, 'event.packet'),
    schema: literalNumber(input.schema, [1], 'event.schema'),
    event: integer(input.event, 'event.event', 0, 5),
    confidence: integer(input.confidence, 'event.confidence', 0, 100),
    svm: integer(input.svm, 'event.svm', 0, 8000),
    mic: integer(input.mic, 'event.mic', 0, 255),
    seq: integer(input.seq, 'event.seq', 0, 255),
    observer: hex(input.observer, 12, 'event.observer'),
    path: enumValue(input.path, ['local', 'tavrn'], 'event.path'),
  };
  if (record.seq !== Number.parseInt(record.packet.slice(-2), 16)) {
    throw new DecodeError('event.seq must match the packet ID low byte.');
  }
  if (record.event === 0 && (record.confidence !== 0 || record.mic !== 0)) {
    throw new DecodeError('heartbeat events must have zero confidence and microphone values.');
  }
  return record;
}

function healthRoot(value: unknown, context: string): HealthRoot {
  const input = object(value, context);
  exactKeys(input, ['cursor', 'kind', 'now', 'local', 'node', 'role', 'roots', 'announced', 'acked', 'rejected', 'pending', 'rootless_drop'], context);
  return {
    cursor: integer(input.cursor, `${context}.cursor`, 1),
    kind: enumValue(input.kind, ['root'], `${context}.kind`),
    ...rootStatus({
      now: input.now,
      local: input.local,
      node: input.node,
      role: input.role,
      roots: input.roots,
      announced: input.announced,
      acked: input.acked,
      rejected: input.rejected,
      pending: input.pending,
      rootless_drop: input.rootless_drop,
    }, context),
  };
}

function rootRecord(value: unknown): RootRecord {
  const input = object(value, 'root record');
  exactKeys(input, ['cursor', 'kind', 'device', 'now', 'local', 'node', 'role', 'roots', 'announced', 'acked', 'rejected', 'pending', 'rootless_drop'], 'root record');
  const status = rootStatus({
    now: input.now,
    local: input.local,
    node: input.node,
    role: input.role,
    roots: input.roots,
    announced: input.announced,
    acked: input.acked,
    rejected: input.rejected,
    pending: input.pending,
    rootless_drop: input.rootless_drop,
  }, 'root record');
  return {
    cursor: integer(input.cursor, 'root.cursor', 1),
    kind: enumValue(input.kind, ['root'], 'root.kind'),
    device: integer(input.device, 'root.device'),
    ...status,
  };
}

function commandRecord(value: unknown): CommandRecord {
  const input = object(value, 'command record');
  exactKeys(input, ['cursor', 'kind', 'device', 'now', 'local', 'command', 'status'], 'command record');
  return {
    cursor: integer(input.cursor, 'command.cursor', 1),
    kind: enumValue(input.kind, ['command'], 'command.kind'),
    device: integer(input.device, 'command.device'),
    now: integer(input.now, 'command.now', 0, 0xffffffff),
    local: hex(input.local, 12, 'command.local'),
    command: enumValue(input.command, ['on', 'off', 'status', 'gtt', 'invalid'], 'command.command'),
    status: enumValue(input.status, ['accepted', 'duplicate', 'busy', 'malformed', 'overflow', 'rejected'], 'command.status'),
  };
}

function streamRecord(value: unknown): StreamRecord {
  const input = object(value, 'stream record');
  switch (input.kind) {
    case 'event':
      return eventRecord(input);
    case 'root':
      return rootRecord(input);
    case 'command':
      return commandRecord(input);
    default:
      throw new DecodeError('stream record.kind is invalid.');
  }
}

function hasEmptyRingCursors(oldest: number, current: number): boolean {
  return oldest === 1 && current === 0;
}

function validateRingCursors(oldest: number, current: number, context: string): void {
  if (hasEmptyRingCursors(oldest, current)) return;
  if (oldest < 1 || current < oldest) {
    throw new DecodeError(`${context} cursor range is invalid.`);
  }
}

export function decodeEventsResponse(value: unknown, requestedAfter = 0): EventsResponse {
  integer(requestedAfter, 'requested after');
  const input = object(value, 'events response');
  exactKeys(input, ['schema', 'gap', 'oldest_cursor', 'current_cursor', 'events'], 'events response');
  if (input.schema !== 'mind.api.v1') throw new DecodeError('events response.schema is invalid.');
  if (!Array.isArray(input.events)) throw new DecodeError('events response.events must be an array.');
  if (input.events.length > 100) throw new DecodeError('events response exceeds the maximum page size.');
  const records = input.events.map(streamRecord);
  for (let index = 1; index < records.length; index += 1) {
    if (records[index - 1].cursor >= records[index].cursor) {
      throw new DecodeError('events response.events must have strictly increasing cursors.');
    }
  }
  const oldest = integer(input.oldest_cursor, 'events response.oldest_cursor');
  const current = integer(input.current_cursor, 'events response.current_cursor');
  const gap = boolean(input.gap, 'events response.gap');
  validateRingCursors(oldest, current, 'events response');
  if (hasEmptyRingCursors(oldest, current)) {
    if (gap || records.length !== 0) throw new DecodeError('empty rings must not report a gap or records.');
  } else {
    if (records.some((record) => record.cursor < oldest || record.cursor > current)) {
      throw new DecodeError('events response records are outside the cursor range.');
    }
    const epochReset = current < requestedAfter;
    if (epochReset) {
      if (gap || records.length !== 0) throw new DecodeError('an epoch reset response must be an empty non-gap page.');
    } else {
      const expectedGap = requestedAfter < oldest - 1;
      if (gap !== expectedGap) throw new DecodeError('events response gap does not match the requested cursor.');
      if (records.some((record) => record.cursor <= requestedAfter)) {
        throw new DecodeError('events response replayed a cursor outside gap recovery.');
      }
      if (records.length === 0 && current > requestedAfter) {
        throw new DecodeError('events response omitted available records.');
      }
      if (records.length > 0 && records.length < 100 && records[records.length - 1].cursor !== current) {
        throw new DecodeError('events response did not drain through its current cursor.');
      }
    }
  }
  return {
    schema: 'mind.api.v1',
    gap,
    oldest_cursor: oldest,
    current_cursor: current,
    events: records,
  };
}

function gttEntry(value: unknown, context: string): GttEntry {
  const input = object(value, context);
  exactKeys(input, ['index', 'adva', 'last', 'soft', 'hard', 'departed_deadline', 'serial', 'serial_state', 'hop', 'hop_state', 'freshness', 'departed'], context);
  return {
    index: integer(input.index, `${context}.index`, 0, 15),
    adva: hex(input.adva, 12, `${context}.adva`),
    last: integer(input.last, `${context}.last`, 0, 0xffffffff),
    soft: integer(input.soft, `${context}.soft`, 0, 0xffffffff),
    hard: integer(input.hard, `${context}.hard`, 0, 0xffffffff),
    departed_deadline: integer(input.departed_deadline, `${context}.departed_deadline`, 0, 0xffffffff),
    serial: integer(input.serial, `${context}.serial`, 0, 0xffff),
    serial_state: enumValue(input.serial_state, ['not_applicable', 'known', 'unknown'], `${context}.serial_state`),
    hop: integer(input.hop, `${context}.hop`, 0, 15),
    hop_state: enumValue(input.hop_state, ['not_applicable', 'known', 'unknown'], `${context}.hop_state`),
    freshness: enumValue(input.freshness, ['not_applicable', 'active', 'soft_stale', 'hard_expired', 'departed'], `${context}.freshness`),
    departed: enumValue(input.departed, ['not_applicable', 'false', 'true', 'unknown'], `${context}.departed`),
  };
}

function gttSnapshot(value: unknown, context: string): GttSnapshot {
  const input = object(value, context);
  exactKeys(input, ['generation', 'completed_at_ms', 'query_at_ms', 'local', 'entry_count', 'nondeparted_count', 'entries'], context);
  if (!Array.isArray(input.entries)) throw new DecodeError(`${context}.entries must be an array.`);
  const entries = input.entries.map((entry, index) => gttEntry(entry, `${context}.entries[${index}]`));
  const snapshot: GttSnapshot = {
    generation: integer(input.generation, `${context}.generation`, 1),
    completed_at_ms: integer(input.completed_at_ms, `${context}.completed_at_ms`),
    query_at_ms: integer(input.query_at_ms, `${context}.query_at_ms`, 0, 0xffffffff),
    local: hex(input.local, 12, `${context}.local`),
    entry_count: integer(input.entry_count, `${context}.entry_count`, 0, 16),
    nondeparted_count: integer(input.nondeparted_count, `${context}.nondeparted_count`, 0, 16),
    entries,
  };
  if (snapshot.entries.length !== snapshot.entry_count || snapshot.nondeparted_count > snapshot.entry_count) {
    throw new DecodeError(`${context} entry counts are inconsistent.`);
  }
  for (let index = 1; index < entries.length; index += 1) {
    if (entries[index - 1].index >= entries[index].index) {
      throw new DecodeError(`${context}.entries must have strictly increasing indices.`);
    }
  }
  if (new Set(entries.map((entry) => entry.adva)).size !== entries.length) {
    throw new DecodeError(`${context}.entries must have unique AdvAs.`);
  }
  if (entries.some((entry) => (entry.freshness === 'departed') !== (entry.departed === 'true'))) {
    throw new DecodeError(`${context}.entries departed state is inconsistent with freshness.`);
  }
  if (entries.filter((entry) => entry.departed !== 'true').length !== snapshot.nondeparted_count) {
    throw new DecodeError(`${context}.nondeparted_count is inconsistent with entries.`);
  }
  return snapshot;
}

export function decodeHealthResponse(value: unknown): HealthResponse {
  const input = object(value, 'health response');
  exactKeys(input, ['schema', 'oldest_cursor', 'current_cursor', 'devices'], 'health response');
  if (input.schema !== 'mind.health.v2') throw new DecodeError('health response.schema is invalid.');
  if (!Array.isArray(input.devices)) throw new DecodeError('health response.devices must be an array.');
  const devices = input.devices.map((value, index): HealthDevice => {
    const device = object(value, `health device ${index}`);
    exactKeys(device, ['device', 'path', 'connected', 'parse_errors', 'overlong_lines', 'reconnects', 'last_record_cursor', 'root', 'gtt', 'owner_device'], `health device ${index}`);
    return {
      device: integer(device.device, `health device ${index}.device`),
      path: string(device.path, `health device ${index}.path`),
      connected: boolean(device.connected, `health device ${index}.connected`),
      parse_errors: integer(device.parse_errors, `health device ${index}.parse_errors`),
      overlong_lines: integer(device.overlong_lines, `health device ${index}.overlong_lines`),
      reconnects: integer(device.reconnects, `health device ${index}.reconnects`),
      last_record_cursor: integer(device.last_record_cursor, `health device ${index}.last_record_cursor`),
      root: device.root === null ? null : healthRoot(device.root, `health device ${index}.root`),
      gtt: device.gtt === null ? null : gttSnapshot(device.gtt, `health device ${index}.gtt`),
      owner_device: integer(device.owner_device, `health device ${index}.owner_device`),
    };
  });
  const deviceIds = new Set(devices.map((device) => device.device));
  if (deviceIds.size !== devices.length) throw new DecodeError('health response contains duplicate device indices.');
  if (devices.some((device) => !deviceIds.has(device.owner_device))) {
    throw new DecodeError('health response contains an unknown owner device reference.');
  }
  if (devices.some((device) => devices.find((owner) => owner.device === device.owner_device)?.owner_device !== device.owner_device)) {
    throw new DecodeError('health response owner device references must identify physical owners.');
  }
  const oldest = integer(input.oldest_cursor, 'health response.oldest_cursor');
  const current = integer(input.current_cursor, 'health response.current_cursor');
  validateRingCursors(oldest, current, 'health response');
  if (devices.some((device) => device.last_record_cursor > current)) {
    throw new DecodeError('health response device cursor exceeds the ring cursor.');
  }
  if (devices.some((device) => device.root !== null && device.root.cursor > current)) {
    throw new DecodeError('health response root cursor exceeds the ring cursor.');
  }
  return { schema: 'mind.health.v2', oldest_cursor: oldest, current_cursor: current, devices };
}

function decodeErrorResponse(value: unknown): ErrorResponse | null {
  try {
    const input = object(value, 'error response');
    exactKeys(input, ['schema', 'accepted', 'error'], 'error response');
    if (input.schema !== 'mind.error.v1' || input.accepted !== false) return null;
    return {
      schema: 'mind.error.v1',
      accepted: false,
      error: enumValue(input.error, ['invalid_query', 'invalid_json', 'invalid_body', 'unknown_device', 'disconnected', 'write_failed'], 'error response.error'),
    };
  } catch {
    return null;
  }
}

export function decodeRootCommandResponse(value: unknown): RootCommandResponse {
  const input = object(value, 'root command response');
  exactKeys(input, ['schema', 'accepted', 'device', 'command'], 'root command response');
  if (input.schema !== 'mind.command.v1' || input.accepted !== true) {
    throw new DecodeError('root command response is invalid.');
  }
  return {
    schema: 'mind.command.v1',
    accepted: true,
    device: integer(input.device, 'root command response.device'),
    command: enumValue(input.command, ['on', 'off'], 'root command response.command'),
  };
}

export function decodeGttCommandResponse(value: unknown): GttCommandResponse {
  const input = object(value, 'gtt command response');
  exactKeys(input, ['schema', 'accepted', 'device', 'command'], 'gtt command response');
  if (input.schema !== 'mind.command.v1' || input.accepted !== true) {
    throw new DecodeError('gtt command response is invalid.');
  }
  return {
    schema: 'mind.command.v1',
    accepted: true,
    device: integer(input.device, 'gtt command response.device'),
    command: enumValue(input.command, ['gtt'], 'gtt command response.command'),
  };
}

async function responseJson(response: Response): Promise<unknown> {
  try {
    return await response.json();
  } catch {
    throw new ApiError('The bridge returned invalid JSON.', response.status);
  }
}

async function get(path: string, signal?: AbortSignal): Promise<unknown> {
  const response = await fetch(path, { signal, headers: { Accept: 'application/json' } });
  const body = await responseJson(response);
  if (response.status !== 200) {
    const error = decodeErrorResponse(body);
    throw new ApiError(error ? `Bridge error: ${error.error}.` : `Bridge request failed (${response.status}).`, response.status);
  }
  return body;
}

export async function fetchEvents(after: number, signal?: AbortSignal): Promise<EventsResponse> {
  return decodeEventsResponse(await get(`/api/events?after=${after}&limit=100`, signal), after);
}

export async function fetchHealth(signal?: AbortSignal): Promise<HealthResponse> {
  return decodeHealthResponse(await get('/api/health', signal));
}

export async function postRoot(device: number, active: boolean, signal?: AbortSignal): Promise<RootCommandResponse> {
  const response = await fetch('/api/root', {
    method: 'POST',
    signal,
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: JSON.stringify({ device, active }),
  });
  const body = await responseJson(response);
  if (response.status !== 202) {
    const error = decodeErrorResponse(body);
    throw new ApiError(error ? `Root command failed: ${error.error}.` : `Root command failed (${response.status}).`, response.status);
  }
  const command = decodeRootCommandResponse(body);
  const expectedCommand = active ? 'on' : 'off';
  if (command.device !== device || command.command !== expectedCommand) {
    throw new ApiError('Root command response did not echo the requested device and command.', response.status);
  }
  return command;
}

export async function postGtt(device: number, signal?: AbortSignal): Promise<GttCommandResponse> {
  const response = await fetch('/api/gtt', {
    method: 'POST',
    signal,
    headers: { 'Content-Type': 'application/json', Accept: 'application/json' },
    body: JSON.stringify({ device }),
  });
  const body = await responseJson(response);
  if (response.status !== 202) {
    const error = decodeErrorResponse(body);
    throw new ApiError(error ? `GTT command failed: ${error.error}.` : `GTT command failed (${response.status}).`, response.status);
  }
  const command = decodeGttCommandResponse(body);
  if (command.device !== device || command.command !== 'gtt') {
    throw new ApiError('GTT command response did not echo the requested device and command.', response.status);
  }
  return command;
}
