import '../test/runtime';
import type { ComponentProps } from 'react';
import { cleanup, render, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { RootControlPanel } from './RootControlPanel';
import { healthDevice, rootStatus } from '../test/fixtures';

afterEach(cleanup);

function panel(overrides: Partial<ComponentProps<typeof RootControlPanel>> = {}) {
  return (
    <RootControlPanel
      devices={[healthDevice(0, { root: rootStatus({ role: 'leaf' }) })]}
      healthEpoch={0}
      currentEpoch={0}
      healthCurrentCursor={1}
      rootRecords={{}}
      commandRecords={{}}
      pendingRoot={{}}
      announcement=""
      onSetRoot={() => undefined}
      {...overrides}
    />
  );
}

describe('root device controls', () => {
  it('uses one checked switch per known node and sends only its opposite root state', async () => {
    const onSetRoot = vi.fn();
    const user = userEvent.setup();
    const { rerender } = render(panel({ onSetRoot }));
    const leafSwitch = within(document.body).getByRole('switch', { name: 'Turn root on for Node 1' });
    expect(leafSwitch.getAttribute('aria-checked')).toBe('false');
    await user.click(leafSwitch);
    expect(onSetRoot).toHaveBeenCalledWith(0, true);

    rerender(panel({ onSetRoot, devices: [healthDevice(0, { root: rootStatus({ role: 'root' }) })] }));
    const rootSwitch = within(document.body).getByRole('switch', { name: 'Turn root off for Node 1' });
    expect(rootSwitch.getAttribute('aria-checked')).toBe('true');
    await user.click(rootSwitch);
    expect(onSetRoot).toHaveBeenLastCalledWith(0, false);
  });

  it('keeps unknown, disconnected, and pending switches disabled with truthful state', () => {
    render(panel({
      devices: [
        healthDevice(0, { root: null }),
        healthDevice(1, { connected: false, root: rootStatus({ node: 2, role: 'root' }) }),
        healthDevice(2, { root: rootStatus({ node: 3, role: 'leaf' }) }),
      ],
      pendingRoot: {
        2: { desired: true, requestId: 7, baselineCursor: 4, baselineEpoch: 0, phase: 'confirming' },
      },
    }));
    const ui = within(document.body);
    const unknown = ui.getByRole('checkbox', { name: 'Root role unknown for Device 0' });
    expect(unknown.getAttribute('aria-checked')).toBe('mixed');
    expect(unknown).toHaveProperty('disabled', true);
    expect(ui.getByText(/awaiting bridge status/i)).not.toBeNull();
    expect(ui.getByRole('switch', { name: 'Turn root off for Node 2' })).toHaveProperty('disabled', true);
    expect(ui.getByRole('switch', { name: 'Turn root on for Node 3' })).toHaveProperty('disabled', true);
    expect(ui.getByText(/Requested ROOT ON; awaiting bridge confirmation/i)).not.toBeNull();
  });

  it('treats a current null health root as a tombstone until a newer stream root arrives', () => {
    const preReconnectRoot = { device: 0, ...rootStatus({ role: 'root', cursor: 5 }) };
    const { rerender } = render(panel({
      devices: [healthDevice(0, { root: null })],
      healthCurrentCursor: 5,
      rootRecords: { 0: preReconnectRoot },
    }));
    const ui = within(document.body);
    const unknown = ui.getByRole('checkbox', { name: 'Root role unknown for Device 0' });
    expect(unknown).toHaveProperty('disabled', true);
    expect(ui.queryByRole('switch', { name: 'Turn root off for Node 1' })).toBeNull();

    rerender(panel({
      devices: [healthDevice(0, { root: null })],
      healthCurrentCursor: 5,
      rootRecords: { 0: { ...preReconnectRoot, cursor: 6 } },
    }));
    expect(ui.getByRole('switch', { name: 'Turn root off for Node 1' })).toHaveProperty('disabled', false);
  });

  it('deduplicates serial aliases and compares root-specific cursors only in the current epoch', () => {
    const root = { device: 0, ...rootStatus({ role: 'root' }), cursor: 5 };
    const { rerender } = render(panel({
      devices: [
        healthDevice(0, { root: rootStatus({ role: 'leaf', cursor: 4 }) }),
        healthDevice(1, { owner_device: 0, root: rootStatus({ role: 'leaf', cursor: 4 }) }),
      ],
        rootRecords: { 0: root },
    }));
    const ui = within(document.body);
    expect(ui.getAllByRole('rowheader')).toHaveLength(1);
    expect(ui.getByRole('switch', { name: 'Turn root off for Node 1' })).not.toBeNull();

    rerender(panel({
      devices: [healthDevice(0, { root: rootStatus({ role: 'leaf', cursor: 6 }) })],
      healthEpoch: 0,
      currentEpoch: 0,
      rootRecords: { 0: root },
    }));
    expect(ui.getByRole('switch', { name: 'Turn root on for Node 1' })).not.toBeNull();

    rerender(panel({
      devices: [healthDevice(0, { root: rootStatus({ role: 'root', cursor: 99 }) })],
      healthEpoch: 0,
      currentEpoch: 1,
      rootRecords: { 0: { device: 0, ...rootStatus({ role: 'leaf' }), cursor: 1 } },
    }));
    expect(ui.getByRole('switch', { name: 'Turn root on for Node 1' })).not.toBeNull();
  });
});
