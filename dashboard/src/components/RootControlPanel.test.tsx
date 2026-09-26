import '../test/runtime';
import { cleanup, render, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { RootControlPanel } from './RootControlPanel';
import { healthDevice, rootStatus } from '../test/fixtures';

afterEach(cleanup);

function panel(overrides: Partial<Parameters<typeof RootControlPanel>[0]> = {}) {
  return <RootControlPanel
    devices={[healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]}
    healthEpoch={0}
    currentEpoch={0}
    healthCurrentCursor={1}
    rootRecords={{}}
    pendingRoot={{}}
    rootErrors={{}}
    announcement=""
    onSetRoot={() => undefined}
    {...overrides}
  />;
}

describe('Gateway control', () => {
  it('uses one Gateway switch and sends only device zero', async () => {
    const onSetRoot = vi.fn();
    const user = userEvent.setup();
    const { rerender } = render(panel({ onSetRoot }));
    const off = within(document.body).getByRole('switch', { name: 'Turn Gateway on' });
    expect(off.getAttribute('aria-checked')).toBe('false');
    expect(within(document.body).getByRole('status', { name: 'Gateway command status' }).textContent).toBe('Gateway is off.');
    await user.click(off);
    expect(onSetRoot).toHaveBeenCalledWith(0, true);

    rerender(panel({ onSetRoot, devices: [healthDevice(0, { root: rootStatus({ role: 'root' }) })] }));
    expect(within(document.body).getByRole('switch', { name: 'Turn Gateway off' }).getAttribute('aria-checked')).toBe('true');
    expect(within(document.body).getByRole('status', { name: 'Gateway command status' }).textContent).toBe('Gateway is on.');
  });

  it('truthfully disables unknown, disconnected, pending, and error states', () => {
    const { rerender } = render(panel({ devices: [healthDevice(0, { root: null })] }));
    const ui = within(document.body);
    expect(ui.getByRole('checkbox', { name: 'Gateway status unknown' })).toHaveProperty('disabled', true);
    expect(ui.getByText('Unknown')).not.toBeNull();

    rerender(panel({ devices: [healthDevice(0, { connected: false, root: rootStatus() })] }));
    expect(ui.getByRole('checkbox', { name: 'Gateway status unknown' })).toHaveProperty('disabled', true);
    expect(ui.getByText('Disconnected')).not.toBeNull();

    rerender(panel({ pendingRoot: { 0: { desired: true, requestId: 1, baselineCursor: 1, baselineEpoch: 0, phase: 'confirming' } } }));
    expect(ui.getByRole('switch', { name: 'Turn Gateway on' })).toHaveProperty('disabled', true);
    expect(ui.getAllByText('Confirming')).toHaveLength(2);

    rerender(panel({ rootErrors: { 0: 'Serial command rejected.' } }));
    expect(ui.getByText('Error')).not.toBeNull();
    expect(ui.getByText('Serial command rejected.')).not.toBeNull();
  });

  it('does not use retained health or internal ROOT errors as Gateway authority', () => {
    render(panel({
      healthAuthoritative: false,
      rootErrors: { 0: 'Root command failed: disconnected.' },
    }));
    const ui = within(document.body);
    expect(ui.getByRole('checkbox', { name: 'Gateway status unknown' })).toHaveProperty('disabled', true);
    expect(ui.getByText('Refreshing')).not.toBeNull();
    expect(ui.getByText('Gateway command could not be completed. Retry Gateway.')).not.toBeNull();
    expect(document.body.textContent).not.toMatch(/root command/i);
  });

  it('sanitizes ROOT protocol wording from Gateway health errors', () => {
    render(panel({ healthError: 'mind_root_v1 decode failed' }));
    const ui = within(document.body);
    expect(ui.getByRole('alert').textContent).toContain('Gateway health is unavailable. Retrying.');
    expect(ui.getByRole('alert').textContent).not.toMatch(/root/i);
  });
});
