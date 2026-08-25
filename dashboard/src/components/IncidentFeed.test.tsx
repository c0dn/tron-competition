import '../test/runtime';
import { cleanup, render, within } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import { IncidentFeed } from './IncidentFeed';
import { eventRecord } from '../test/fixtures';
import type { IncidentLocalizationView } from '../localization/IncidentLocalization';
import type { LogicalEvent } from '../state/store';

afterEach(cleanup);

function logicalEvent(): LogicalEvent {
  const record = eventRecord({ observer_rssi_dbm: -58 });
  return {
    key: '7:00002a',
    fingerprint: '1:3:75:2400:86:42',
    record,
    evidence: [{
      root: record.root,
      observer: record.observer,
      path: record.path,
      device: record.device,
      observerRssiDbm: -58,
      sampleCount: 0xffffffff,
    }],
    evidenceOverflowCount: 0,
    evidenceSaturated: false,
    conflict: false,
    firstReceivedAt: 1_000,
    collectUntil: 3_000,
  };
}

describe('IncidentFeed localization presentation', () => {
  it('uses one contextual live announcement and public location states without provider vocabulary', () => {
    const logical = logicalEvent();
    const views: readonly IncidentLocalizationView[] = [
      { logicalEvent: logical, status: 'collecting', contributorIds: [], contributorCount: 0 },
      { logicalEvent: { ...logical, key: '8:00002b' }, status: 'insufficient', contributorIds: ['a'], contributorCount: 1 },
      {
        logicalEvent: { ...logical, key: '9:00002c' }, status: 'ballpark', contributorIds: ['a', 'b', 'c'], contributorCount: 3,
        geometryWarning: true, normalizedSpread: null, x: 0.25, y: 0.75,
      },
      {
        logicalEvent: { ...logical, key: '10:00002d' }, status: 'ballpark', contributorIds: ['a', 'b', 'c', 'd'], contributorCount: 4,
        geometryWarning: false, normalizedSpread: 0, x: 0.5, y: 0.5,
      },
    ];

    render(<IncidentFeed views={views} conflictCount={0} loading={false} />);
    const ui = within(document.body);
    expect(ui.getByText('4 logical events')).not.toBeNull();
    expect(ui.getByText('Collecting')).not.toBeNull();
    expect(ui.getByText('Insufficient — 1 of 3 positioned RSSI contributors')).not.toBeNull();
    expect(ui.getAllByText('Ballpark')).toHaveLength(2);
    expect(ui.getAllByText(/strongest RSSI -58 dBm/i)).toHaveLength(4);
    expect(ui.getAllByText(/4294967295 samples/i)).toHaveLength(4);
    expect(ui.getAllByText('Not available')).toHaveLength(1);
    expect(ui.getAllByText('0.00').length).toBeGreaterThan(0);
    const liveRegions = ui.getAllByRole('status');
    expect(liveRegions).toHaveLength(1);
    expect(liveRegions[0]?.textContent).toContain('Wearable 7, packet 00002a: collecting.');
    expect(liveRegions[0]?.textContent).toContain('Wearable 7, packet 00002a: insufficient, 1 of 3 contributors.');
    expect(liveRegions[0]?.textContent).toContain('Wearable 7, packet 00002a: ballpark at normalized coordinates 0.25, 0.75, 3 contributors, normalized spread not available.');
    expect(liveRegions[0]?.textContent).toContain('Wearable 7, packet 00002a: ballpark at normalized coordinates 0.50, 0.50, 4 contributors, normalized spread 0.00.');
    expect(document.body.textContent).not.toMatch(/estimated|invalid_input|meters|precision/i);
  });

  it('uses singular event count copy', () => {
    const logical = logicalEvent();
    render(<IncidentFeed
      views={[{ logicalEvent: logical, status: 'collecting', contributorIds: [], contributorCount: 0 }]}
      conflictCount={0}
      loading={false}
    />);

    expect(within(document.body).getByText('1 logical event')).not.toBeNull();
  });
});
