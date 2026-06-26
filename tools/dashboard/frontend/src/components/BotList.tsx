import type { BotSummary } from '../types/bot';
import { CLASS_NAMES, CLASS_COLORS, getStatusColor } from '../utils/classInfo';
import { HpBar } from './ui/HpBar';
import { StatusBadge } from './ui/StatusBadge';

interface Props {
  bots: BotSummary[];
  onSelect: (name: string) => void;
}

export function BotList({ bots, onSelect }: Props) {
  if (!bots.length) {
    return (
      <div style={{ padding: 24, color: '#555', textAlign: 'center', fontSize: 12 }}>
        No bots online. Waiting for data…
      </div>
    );
  }

  const sorted = [...bots].sort((a, b) => a.bot_name.localeCompare(b.bot_name));

  return (
    <div style={{ overflowY: 'auto', flex: 1 }}>
      {sorted.map(bot => {
        const classColor = CLASS_COLORS[bot.class_id] ?? '#aaa';
        const statusColor = getStatusColor(bot);

        const stateLabel = () => {
          if (!bot.alive) return 'Dead';
          if (bot.is_ghost) return 'Ghost';
          if (bot.in_combat) return 'Combat';
          if (bot.state === 'blocked' && bot.blocked_reason) return `Blocked`;
          return bot.state ?? 'idle';
        };

        return (
          <div
            key={bot.bot_name}
            onClick={() => onSelect(bot.bot_name)}
            style={{
              padding: '10px 14px',
              borderBottom: '1px solid #1e1e3e',
              cursor: 'pointer',
              transition: 'background 0.1s',
            }}
            onMouseEnter={e => (e.currentTarget.style.background = '#16213e')}
            onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
          >
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
              <div style={{
                width: 28, height: 28, borderRadius: '50%',
                background: classColor + '22',
                border: `1.5px solid ${classColor}`,
                display: 'flex', alignItems: 'center', justifyContent: 'center',
                fontWeight: 700, fontSize: 12, color: classColor, flexShrink: 0,
              }}>
                {bot.level ?? '?'}
              </div>
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ fontWeight: 600, color: classColor, fontSize: 13 }}>{bot.bot_name}</div>
                <div style={{ fontSize: 10, color: '#666' }}>{CLASS_NAMES[bot.class_id] ?? `cls${bot.class_id}`}</div>
              </div>
              <StatusBadge label={stateLabel()} color={statusColor} size="sm" />
              {bot.live_state_status === 'stale' && (
                <span style={{ fontSize: 9, color: '#f59e0b', fontWeight: 600, flexShrink: 0 }}>
                  ⚠ stale
                </span>
              )}
              {bot.live_state_status === 'delayed' && (
                <span style={{ fontSize: 9, color: '#fbbf24', fontWeight: 600, flexShrink: 0 }}>
                  delayed
                </span>
              )}
            </div>

            <div style={{ paddingLeft: 36 }}>
              <HpBar pct={bot.hp_pct ?? 0} color="#22c55e" height={6} />
              <HpBar pct={bot.mana_pct ?? 0} color="#3b82f6" height={6} />
              {bot.guide_id && (
                <div style={{ fontSize: 10, color: '#555', marginTop: 3 }}>
                  Step {bot.step_index}/{bot.step_total || '?'} — {bot.guide_id}
                </div>
              )}
              {bot.death_count_total > 0 && (
                <div style={{ fontSize: 10, color: '#ef4444', marginTop: 1 }}>
                  ☠ {bot.death_count_total} deaths
                </div>
              )}
              {bot.blocked_reason && (
                <div style={{ fontSize: 10, color: '#fca5a5', marginTop: 1 }}>
                  Blocked: {bot.blocked_reason}
                </div>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
}
