export interface BotPosition {
  map_id: number;
  x: number;
  y: number;
  z: number;
  orientation: number;
}

export interface BotTarget {
  entry: number | null;
  name: string | null;
  level: number | null;
  distance: number | null;
  role: string | null;
}

export interface BotSummary {
  bot_name: string;
  character_guid?: number | null;
  class_id: number;
  class_name?: string | null;
  race_id: number;
  race_name?: string | null;
  level: number;
  map_id: number;
  x: number;
  y: number;
  z: number;
  orientation: number;
  position?: BotPosition | null;
  hp_pct: number;
  mana_pct: number;
  alive: boolean;
  is_ghost: boolean;
  in_combat: boolean;
  state: string;
  soak_run_id?: string | null;
  bot_session_id?: string | null;
  reset_id?: number;
  blocked_reason?: string | null;
  blocked_since?: string | null;
  last_failure_code?: string | null;
  requires_user_action?: boolean;
  guide_id: string | null;
  step_index: number;
  step_total: number;
  step_name: string | null;
  quest_id: number | null;
  objective_text: string | null;
  target_entry: number | null;
  target_name: string | null;
  target_level: number | null;
  target_distance: number | null;
  target_role: string | null;
  target?: BotTarget | null;
  enriched_guide_step?: EnrichedGuideStep | null;
  death_count_total: number;
  death_count_current_step: number;
  money: number;
  bag_used: number;
  bag_total: number;
  durability_pct: number;
  updated_at: string | null;
  data_source?: string;
  live_state_status?: 'live' | 'delayed' | 'stale';
  is_stale?: boolean;
  stale_seconds?: number | null;
}

export interface InventoryItem {
  bag_id?: number;
  bag?: number;
  bag_slot?: number;
  bag_name?: string | null;
  slot: number;
  item_entry: number;
  item_name: string | null;
  count: number;
  durability?: number | null;
  max_durability?: number | null;
}

export interface EquipmentItem {
  slot: number;
  slot_name: string;
  item_entry: number;
  item_name: string | null;
  durability: number | null;
  max_durability: number | null;
}

export interface QuestLogEntry {
  quest_id: number;
  title: string | null;
  status: string;
  description?: string | null;
  completion_log?: string | null;
  objectives?: QuestObjective[];
}

export interface QuestObjective {
  kind: string;
  text: string;
  progress: number;
  required: number;
  target_id?: number | null;
  target_name?: string | null;
}

export interface QuestLog {
  active: QuestLogEntry[];
  completed_count: number;
  guide_quest_id: number | null;
  guide_step: string | null;
  guide_objective: string | null;
  enriched_guide_step?: EnrichedGuideStep | null;
  error: string | null;
}

export interface EnrichedGuideStep {
  guideName: string | null;
  stepIndex: number;
  rawAction: string | null;
  actionType: string;
  actionLabel: string;
  questId: number | null;
  questTitle: string | null;
  questLevel: number | null;
  objectiveIndex: number | null;
  objectiveType: string | null;
  objectiveText: string | null;
  current: number | null;
  required: number | null;
  targetEntries: number[];
  targetNames: string[];
  itemIds: number[];
  itemNames: string[];
  gameObjectIds: number[];
  gameObjectNames: string[];
  pickup: BotTarget | null;
  turnin: BotTarget | null;
  displayTitle: string;
  displaySubtitle: string | null;
  status: string;
  debugRaw: string | null;
  confidence: string;
  source: string;
  unresolvedReason: string | null;
}

export interface EventEntry {
  event_type: string;
  event_code?: string | null;
  soak_run_id?: string | null;
  bot_session_id?: string | null;
  reset_id?: number;
  detail: string;
  created_at: string;
  status?: 'active' | 'stale' | 'resolved' | 'invalid_timestamp' | 'current';
}

export interface BotDetail extends BotSummary {
  hp?: number;
  hp_max?: number;
  mana?: number;
  mana_max?: number;
}

export type MapId = 0 | 1 | 530 | 571;

export type ApiBotSummary = Partial<BotSummary> & {
  bot_name?: string;
};

export function normalizeBotSummary(raw: ApiBotSummary): BotSummary {
  const position = raw.position ?? null;
  const target = raw.target ?? null;

  return {
    bot_name: raw.bot_name ?? '',
    character_guid: raw.character_guid ?? null,
    class_id: raw.class_id ?? 0,
    class_name: raw.class_name ?? null,
    race_id: raw.race_id ?? 0,
    race_name: raw.race_name ?? null,
    level: raw.level ?? 0,
    map_id: raw.map_id ?? position?.map_id ?? 0,
    x: raw.x ?? position?.x ?? 0,
    y: raw.y ?? position?.y ?? 0,
    z: raw.z ?? position?.z ?? 0,
    orientation: raw.orientation ?? position?.orientation ?? 0,
    position,
    hp_pct: raw.hp_pct ?? 100,
    mana_pct: raw.mana_pct ?? 100,
    alive: raw.alive ?? true,
    is_ghost: raw.is_ghost ?? false,
    in_combat: raw.in_combat ?? false,
    state: raw.state ?? 'idle',
    soak_run_id: raw.soak_run_id ?? null,
    bot_session_id: raw.bot_session_id ?? null,
    reset_id: raw.reset_id ?? 0,
    blocked_reason: raw.blocked_reason ?? null,
    blocked_since: raw.blocked_since ?? null,
    last_failure_code: raw.last_failure_code ?? null,
    requires_user_action: raw.requires_user_action ?? false,
    guide_id: raw.guide_id ?? null,
    step_index: raw.step_index ?? 0,
    step_total: raw.step_total ?? 0,
    step_name: raw.step_name ?? null,
    quest_id: raw.quest_id ?? null,
    objective_text: raw.objective_text ?? null,
    target_entry: raw.target_entry ?? target?.entry ?? null,
    target_name: raw.target_name ?? target?.name ?? null,
    target_level: raw.target_level ?? target?.level ?? null,
    target_distance: raw.target_distance ?? target?.distance ?? null,
    target_role: raw.target_role ?? target?.role ?? null,
    target,
    enriched_guide_step: raw.enriched_guide_step ?? null,
    death_count_total: raw.death_count_total ?? 0,
    death_count_current_step: raw.death_count_current_step ?? 0,
    money: raw.money ?? 0,
    bag_used: raw.bag_used ?? 0,
    bag_total: raw.bag_total ?? 0,
    durability_pct: raw.durability_pct ?? 100,
    updated_at: raw.updated_at ?? null,
    data_source: raw.data_source,
    live_state_status: raw.live_state_status ?? 'live',
    is_stale: raw.is_stale,
    stale_seconds: raw.stale_seconds ?? null,
  };
}
