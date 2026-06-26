"""Pydantic response models for the IdleBot dashboard API."""
from __future__ import annotations
from datetime import datetime
from typing import Optional
from pydantic import BaseModel, Field

CLASS_NAMES = {
    1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
    6: "Death Knight", 7: "Shaman", 8: "Mage", 9: "Warlock", 11: "Druid",
}
RACE_NAMES = {
    1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf", 5: "Undead",
    6: "Tauren", 7: "Gnome", 8: "Troll", 10: "Blood Elf", 11: "Draenei",
}
EQUIP_SLOTS = {
    0: "Head", 1: "Neck", 2: "Shoulders", 3: "Body", 4: "Chest",
    5: "Waist", 6: "Legs", 7: "Feet", 8: "Wrists", 9: "Hands",
    10: "Finger1", 11: "Finger2", 12: "Trinket1", 13: "Trinket2",
    14: "Back", 15: "MainHand", 16: "OffHand", 17: "Ranged", 18: "Tabard",
}

# Status codes used by marker colour logic
# healthy / traveling / recovering / warning / dead / no_guide
STATUS_MAP = {
    "idle": "traveling",
    "running": "healthy",
    "paused": "warning",
    "blocked": "warning",
    "quarantined": "warning",
    "combat": "healthy",
    "recover": "recovering",
    "loot": "traveling",
    "recovery": "recovering",
    "dead": "dead",
    "ghost": "dead",
    "stuck": "warning",
}


def derive_status(state: str | None, alive: bool, is_ghost: bool, guide_id: str | None) -> str:
    if not guide_id:
        return "no_guide"
    if is_ghost or not alive:
        return "dead"
    return STATUS_MAP.get(state or "idle", "traveling")


class BotPosition(BaseModel):
    map_id: int = 0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    orientation: float = 0.0


class BotTarget(BaseModel):
    entry: Optional[int] = None
    name: Optional[str] = None
    level: Optional[int] = None
    distance: Optional[float] = None
    role: Optional[str] = None


class EnrichedGuideStep(BaseModel):
    guideName: Optional[str] = None
    stepIndex: int = 0
    rawAction: Optional[str] = None
    actionType: str = "unknown"
    actionLabel: str = "Unknown"
    questId: Optional[int] = None
    questTitle: Optional[str] = None
    questLevel: Optional[int] = None
    objectiveIndex: Optional[int] = None
    objectiveType: Optional[str] = None
    objectiveText: Optional[str] = None
    current: Optional[int] = None
    required: Optional[int] = None
    targetEntries: list[int] = Field(default_factory=list)
    targetNames: list[str] = Field(default_factory=list)
    itemIds: list[int] = Field(default_factory=list)
    itemNames: list[str] = Field(default_factory=list)
    gameObjectIds: list[int] = Field(default_factory=list)
    gameObjectNames: list[str] = Field(default_factory=list)
    pickup: Optional[BotTarget] = None
    turnin: Optional[BotTarget] = None
    displayTitle: str = "Guide step"
    displaySubtitle: Optional[str] = None
    status: str = "unknown"
    debugRaw: Optional[str] = None
    confidence: str = "low"
    source: str = "guide"
    unresolvedReason: Optional[str] = None
    requiresSpecialTravel: bool = False
    specialTravelType: Optional[str] = None  # "druid_teleport_moonglade", etc.
    targetMap: Optional[str] = None          # human-readable zone name for the target


class BotSummary(BaseModel):
    """Compact model for the map view and WebSocket snapshot."""
    bot_name: str
    character_guid: Optional[int] = None
    class_id: Optional[int] = None
    class_name: Optional[str] = None
    race_id: Optional[int] = None
    race_name: Optional[str] = None
    level: Optional[int] = None
    map_id: int = 0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    orientation: float = 0.0
    position: Optional[BotPosition] = None
    hp_pct: float = 100.0
    mana_pct: float = 100.0
    alive: bool = True
    is_ghost: bool = False
    in_combat: bool = False
    state: str = "idle"
    status: str = "traveling"
    soak_run_id: Optional[str] = None
    bot_session_id: Optional[str] = None
    reset_id: int = 0
    blocked_reason: Optional[str] = None
    blocked_since: Optional[datetime] = None
    last_failure_code: Optional[str] = None
    requires_user_action: bool = False
    guide_id: Optional[str] = None
    step_index: int = 0
    step_total: int = 0
    step_name: Optional[str] = None
    quest_id: Optional[int] = None
    objective_text: Optional[str] = None
    target_entry: Optional[int] = None
    target_name: Optional[str] = None
    target_level: Optional[int] = None
    target_distance: Optional[float] = None
    target_role: Optional[str] = None
    target: Optional[BotTarget] = None
    enriched_guide_step: Optional[EnrichedGuideStep] = None
    death_count_total: int = 0
    death_count_current_step: int = 0
    money: int = 0
    bag_used: int = 0
    bag_total: int = 0
    durability_pct: int = 100
    updated_at: Optional[datetime] = None
    data_source: str = "live_state"  # "live_state" | "fallback"
    live_state_status: str = "live"
    is_stale: bool = False
    stale_seconds: Optional[float] = None


class BotDetail(BotSummary):
    """Full model including quests/events for the side panel."""
    pass


class InventoryItem(BaseModel):
    bag_id: int = 0
    slot: int
    bag_slot: int
    bag_name: Optional[str] = None
    item_entry: int
    item_name: Optional[str] = None
    count: int = 1
    durability: Optional[int] = None
    max_durability: Optional[int] = None


class EquipmentItem(BaseModel):
    slot: int
    slot_name: str
    item_entry: int
    item_name: Optional[str] = None
    durability: Optional[int] = None
    max_durability: Optional[int] = None


class QuestEntry(BaseModel):
    quest_id: int
    title: Optional[str] = None
    status: str  # in_progress | complete | failed | unknown
    description: Optional[str] = None
    completion_log: Optional[str] = None
    objectives: list["QuestObjective"] = Field(default_factory=list)


class QuestObjective(BaseModel):
    kind: str
    text: str
    progress: int = 0
    required: int = 0
    target_id: Optional[int] = None
    target_name: Optional[str] = None


class QuestLog(BaseModel):
    active: list[QuestEntry] = Field(default_factory=list)
    completed_count: int = 0
    guide_quest_id: Optional[int] = None
    guide_step: Optional[str] = None
    guide_objective: Optional[str] = None
    enriched_guide_step: Optional[EnrichedGuideStep] = None
    error: Optional[str] = None


class BotEvent(BaseModel):
    event_type: str
    event_code: Optional[str] = None
    soak_run_id: Optional[str] = None
    bot_session_id: Optional[str] = None
    reset_id: int = 0
    detail: Optional[str] = None
    created_at: Optional[datetime] = None
    status: str = "current"


class WsSnapshot(BaseModel):
    type: str = "snapshot"
    ts: float
    bots: list[BotSummary]
