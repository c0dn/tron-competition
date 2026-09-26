export function formatAge(milliseconds: number): string {
  if (milliseconds < 1_000) return 'just now';
  if (milliseconds < 60_000) return `${Math.floor(milliseconds / 1_000)} seconds ago`;
  return `${Math.floor(milliseconds / 60_000)} minutes ago`;
}

export function formatDeviceTime(milliseconds: number): string {
  return `${(milliseconds / 1_000).toFixed(3)} s`;
}

export function eventLabel(event: number): string {
  switch (event) {
    case 0:
      return 'Heartbeat';
    case 1:
      return 'Motion';
    case 2:
      return 'Possible fall';
    case 3:
      return 'Confirmed fall';
    case 4:
      return 'Possible distress';
    case 5:
      return 'Fall and shout';
    default:
      return `Event ${event}`;
  }
}
