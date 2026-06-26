import type { EventEntry } from '../../types/bot';
import { relativeTime } from '../../utils/classInfo';

const EVENT_COLORS: Record<string, string> = {
  DEATH: '#ef4444',
  RECOVERY: '#f59e0b',
  QUEST: '#22c55e',
  GUIDE: '#60a5fa',
  COMBAT: '#f97316',
  FAILURE: '#dc2626',
  LEVEL: '#a855f7',
  COLLECT: '#06b6d4',
  OBJECT: '#84cc16',
};

const STATUS_COLORS: Record<string, string> = {
  active: '#ef4444',
  stale: '#6b7280',
  resolved: '#22c55e',
  invalid_timestamp: '#f59e0b',
  current: '#60a5fa',
};

export function EventsTab({ events, error }: { events: EventEntry[]; error?: string }) {
  if (error && !events.length) return <div style={{ padding: 16, color: '#f59e0b', fontSize: 12 }}>⚠ {error}</div>;
  if (!events.length) {
    return <div style={{ padding: 16, color: '#666', fontSize: 12 }}>No events.</div>;
  }

  return (
    <div style={{ padding: '12px 16px', display: 'flex', flexDirection: 'column', gap: 4, overflowY: 'auto', maxHeight: '100%' }}>
      {events.map((ev, i) => {
        const color = EVENT_COLORS[ev.event_type] ?? '#aaa';
        const statusColor = STATUS_COLORS[ev.status ?? 'current'] ?? '#888';
        return (
          <div key={i} style={{
            background: '#16213e',
            borderRadius: 5,
            padding: '5px 8px',
            borderLeft: `3px solid ${color}`,
            display: 'flex',
            gap: 8,
            alignItems: 'flex-start',
          }}>
            <div style={{ width: 60, flexShrink: 0 }}>
              <div style={{ fontSize: 9, color, textTransform: 'uppercase', fontWeight: 700 }}>
                {ev.event_type}
              </div>
              <div style={{ fontSize: 9, color: '#555', marginTop: 1 }}>
                {relativeTime(ev.created_at)}
              </div>
              {ev.status && (
                <div style={{ fontSize: 9, color: statusColor, marginTop: 2, fontWeight: 700 }}>
                  {ev.status.replace('_', ' ')}
                </div>
              )}
            </div>
            <div style={{ fontSize: 11, color: '#ccc', lineHeight: 1.4, wordBreak: 'break-word' }}>
              {ev.detail}
            </div>
          </div>
        );
      })}
    </div>
  );
}
