import type { BotDetail } from '../../types/bot';
import { CLASS_NAMES, RACE_NAMES, CLASS_COLORS, getStatusColor } from '../../utils/classInfo';
import { HpBar } from '../ui/HpBar';
import { StatusBadge } from '../ui/StatusBadge';
import { MoneyDisplay } from '../ui/MoneyDisplay';

export function SummaryTab({ bot }: { bot: BotDetail }) {
  const classColor = CLASS_COLORS[bot.class_id] ?? '#aaa';
  const statusColor = getStatusColor(bot);

  const stateLabel = () => {
    if (!bot.alive) return 'Dead';
    if (bot.is_ghost) return 'Ghost';
    if (bot.in_combat) return 'Combat';
    if (bot.state === 'blocked') return 'Blocked';
    return bot.state ?? 'idle';
  };

  return (
    <div style={{ padding: '12px 16px', display: 'flex', flexDirection: 'column', gap: 12 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
        <div style={{
          width: 36, height: 36, borderRadius: '50%',
          background: classColor + '33',
          border: `2px solid ${classColor}`,
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          fontWeight: 700, fontSize: 16, color: classColor,
        }}>
          {bot.level}
        </div>
        <div>
          <div style={{ fontWeight: 700, fontSize: 15, color: classColor }}>{bot.bot_name}</div>
          <div style={{ fontSize: 11, color: '#aaa' }}>
            {RACE_NAMES[bot.race_id] ?? `Race ${bot.race_id}`} {CLASS_NAMES[bot.class_id] ?? `Class ${bot.class_id}`}
          </div>
        </div>
        <div style={{ marginLeft: 'auto', display: 'flex', flexDirection: 'column', alignItems: 'flex-end', gap: 4 }}>
          <StatusBadge label={stateLabel()} color={statusColor} />
          {bot.live_state_status === 'stale' && (
            <span style={{ fontSize: 10, color: '#f59e0b' }}>
              ⚠ stale {bot.stale_seconds != null ? `${bot.stale_seconds}s` : ''}
            </span>
          )}
          {bot.live_state_status === 'delayed' && (
            <span style={{ fontSize: 10, color: '#fbbf24' }}>
              delayed {bot.stale_seconds != null ? `${bot.stale_seconds}s` : ''}
            </span>
          )}
        </div>
      </div>

      <div>
        <HpBar pct={bot.hp_pct ?? 0} color="#22c55e" label="HP" height={12} />
        <HpBar pct={bot.mana_pct ?? 0} color="#3b82f6" label="Mana" height={12} />
      </div>

      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 8 }}>
        <div style={stat_block}>
          <div style={stat_label}>Money</div>
          <MoneyDisplay copper={bot.money ?? 0} />
        </div>
        <div style={stat_block}>
          <div style={stat_label}>Bags</div>
          <span>{bot.bag_used ?? 0} / {bot.bag_total ?? '?'} slots</span>
        </div>
        <div style={stat_block}>
          <div style={stat_label}>Durability</div>
          <span style={{ color: (bot.durability_pct ?? 100) < 30 ? '#ef4444' : '#aaa' }}>
            {bot.durability_pct ?? 100}%
          </span>
        </div>
        <div style={stat_block}>
          <div style={stat_label}>Deaths</div>
          <span>{bot.death_count_total ?? 0}</span>
        </div>
      </div>

      {bot.guide_id && (
        <div style={{ background: '#16213e', borderRadius: 6, padding: '8px 10px' }}>
          <div style={stat_label}>Guide</div>
          <div style={{ fontSize: 12, color: '#60a5fa', marginBottom: 4 }}>{bot.guide_id}</div>
          <div style={stat_label}>Step</div>
          <div style={{ fontSize: 12, color: '#e0e0e0' }}>
            {bot.step_index} / {bot.step_total || '?'}
            {bot.step_name && <span style={{ color: '#aaa', marginLeft: 6 }}>— {bot.step_name}</span>}
          </div>
          {bot.step_total > 0 && (
            <div style={{ marginTop: 6 }}>
              <div style={{ background: '#2a2a3e', borderRadius: 3, height: 4, overflow: 'hidden' }}>
                <div style={{
                  width: `${(bot.step_index / bot.step_total) * 100}%`,
                  height: '100%',
                  background: '#60a5fa',
                  borderRadius: 3,
                }} />
              </div>
            </div>
          )}
        </div>
      )}

      {bot.objective_text && (
        <div style={{ background: '#16213e', borderRadius: 6, padding: '8px 10px' }}>
          <div style={stat_label}>Objective</div>
          <div style={{ fontSize: 12, color: '#fbbf24' }}>{bot.objective_text}</div>
        </div>
      )}

      {bot.blocked_reason && (
        <div style={{ background: '#2a1111', borderRadius: 6, padding: '8px 10px', borderLeft: '3px solid #dc2626' }}>
          <div style={stat_label}>Blocked</div>
          <div style={{ fontSize: 12, color: '#fecaca' }}>{bot.blocked_reason}</div>
          {bot.blocked_since && (
            <div style={{ fontSize: 10, color: '#fca5a5', marginTop: 4 }}>
              Since {bot.blocked_since}
            </div>
          )}
        </div>
      )}
    </div>
  );
}

const stat_block: React.CSSProperties = {
  background: '#16213e',
  borderRadius: 6,
  padding: '6px 8px',
};

const stat_label: React.CSSProperties = {
  fontSize: 10,
  color: '#666',
  textTransform: 'uppercase',
  letterSpacing: '0.05em',
  marginBottom: 2,
};
