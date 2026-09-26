import { describe, expect, it } from 'vitest';
import { gatewayTopology } from '../state/devices';
import { gttEntry, gttSnapshot, healthDevice, rootStatus } from '../test/fixtures';

describe('Gateway GTT authority', () => {
  it('accepts only the sole physical device zero with one matching self entry', () => {
    const gtt = gttSnapshot({
      local: '1842de524add',
      entries: [
        gttEntry({ index: 0, adva: '1842de524add' }),
        gttEntry({ index: 1, adva: '0102545678c1', freshness: 'soft_stale', departed: 'unknown' }),
      ],
    });
    const result = gatewayTopology(
      [healthDevice(0, { root: rootStatus({ local: gtt.local }), gtt })],
      0,
      0,
      1,
      {},
    );
    expect(result).toMatchObject({ available: true, topology: { gtt: { local: '1842de524add' } } });
  });

  it('fails closed for missing self identity, root mismatch, or another physical owner', () => {
    const noSelf = gttSnapshot({ local: '1842de524add', entries: [gttEntry({ adva: '0102545678c0' })] });
    expect(gatewayTopology([healthDevice(0, { gtt: noSelf })], 0, 0, 0, {})).toMatchObject({ available: false, issue: 'gtt_self_missing' });

    const valid = gttSnapshot({ local: '1842de524add', entries: [gttEntry({ adva: '1842de524add' })] });
    expect(gatewayTopology([healthDevice(0, { root: rootStatus({ local: '0102545678c0' }), gtt: valid })], 0, 0, 1, {})).toMatchObject({ available: false, issue: 'gtt_root_mismatch' });
    const duplicatedSelf = gttSnapshot({ local: '1842de524add', entries: [gttEntry({ index: 0, adva: '1842de524add' }), gttEntry({ index: 1, adva: '1842de524add' })] });
    expect(gatewayTopology([healthDevice(0, { gtt: duplicatedSelf })], 0, 0, 0, {})).toMatchObject({ available: false, issue: 'gtt_self_duplicate' });
    expect(gatewayTopology([healthDevice(0, { gtt: valid }), healthDevice(1)], 0, 0, 0, {})).toMatchObject({ available: false, issue: 'gateway_duplicate' });
  });

  it('requires current health and a connected Gateway at the authority boundary', () => {
    const valid = gttSnapshot({ local: '1842de524add', entries: [gttEntry({ adva: '1842de524add' })] });
    const device = healthDevice(0, { root: rootStatus({ local: valid.local }), gtt: valid });
    expect(gatewayTopology([device], 0, 0, 1, {}, false)).toMatchObject({ available: false, issue: 'health_unavailable' });
    expect(gatewayTopology([{ ...device, connected: false }], 0, 0, 1, {})).toMatchObject({ available: false, issue: 'gateway_disconnected' });
  });
});
