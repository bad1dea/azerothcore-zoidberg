export const CLASS_COLORS: Record<number, string> = {
  1: '#C69B3A',
  2: '#F58CBA',
  3: '#ABD473',
  4: '#FFF569',
  5: '#FFFFFF',
  6: '#C41F3B',
  7: '#0070DE',
  8: '#69CCF0',
  9: '#9482C9',
  11: '#FF7D0A',
};

export const CLASS_NAMES: Record<number, string> = {
  1: 'Warrior',
  2: 'Paladin',
  3: 'Hunter',
  4: 'Rogue',
  5: 'Priest',
  6: 'Death Knight',
  7: 'Shaman',
  8: 'Mage',
  9: 'Warlock',
  11: 'Druid',
};

export const RACE_NAMES: Record<number, string> = {
  1: 'Human',
  2: 'Orc',
  3: 'Dwarf',
  4: 'Night Elf',
  5: 'Undead',
  6: 'Tauren',
  7: 'Gnome',
  8: 'Troll',
  10: 'Blood Elf',
  11: 'Draenei',
};

export const MAP_NAMES: Record<number, string> = {
  0: 'Eastern Kingdoms',
  1: 'Kalimdor',
  530: 'Outland',
  571: 'Northrend',
};

export function getStatusColor(bot: { alive: boolean; is_ghost: boolean; in_combat: boolean; state: string; guide_id: string | null; class_id: number }): string {
  if (!bot.alive) return '#8B0000';
  if (bot.is_ghost) return '#888888';
  if (bot.in_combat) return '#FF2020';
  if (bot.state === 'recover') return '#FFD700';
  if (bot.state === 'paused') return '#FF8C00';
  if (bot.state === 'blocked') return '#dc2626';
  if (bot.state === 'idle' && !bot.guide_id) return '#9B59B6';
  return CLASS_COLORS[bot.class_id] ?? '#AAAAAA';
}

export function formatMoney(copper: number): string {
  const g = Math.floor(copper / 10000);
  const s = Math.floor((copper % 10000) / 100);
  const c = copper % 100;
  const parts: string[] = [];
  if (g > 0) parts.push(`${g}g`);
  if (s > 0) parts.push(`${s}s`);
  if (c > 0 || parts.length === 0) parts.push(`${c}c`);
  return parts.join(' ');
}

export function relativeTime(isoStr: string): string {
  const ts = new Date(isoStr).getTime();
  if (!Number.isFinite(ts)) return 'unknown time';
  const diff = Math.floor((Date.now() - ts) / 1000);
  if (diff < 0) return 'timestamp error';
  if (diff < 60) return `${diff}s ago`;
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  return `${Math.floor(diff / 3600)}h ago`;
}
