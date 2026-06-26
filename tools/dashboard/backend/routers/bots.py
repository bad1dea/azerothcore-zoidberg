"""REST endpoints for bot data."""
from __future__ import annotations
import logging
import re
import time
from datetime import datetime, timezone
from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
import db
from models import (
    BotSummary, BotDetail, BotEvent, BotTarget, BotPosition,
    InventoryItem, EquipmentItem, QuestEntry, QuestLog, QuestObjective, EnrichedGuideStep,
    CLASS_NAMES, RACE_NAMES, EQUIP_SLOTS, derive_status,
)

log = logging.getLogger(__name__)

router = APIRouter(prefix="/api")

DELAYED_THRESHOLD_S = 30.0
STALE_THRESHOLD_S = 120.0

# ─── helpers ────────────────────────────────────────────────────────────────

_live_state_exists: bool | None = None  # cached after first check
_unresolved_guide_steps: dict[str, dict] = {}


async def _resolve_guid(name: str) -> int | None:
    """Resolve character GUID for a bot. Checks live_state first (always populated by C++),
    then idlebot_bots, then characters table by name as last resort."""
    row = await db.fetchone(
        "SELECT character_guid FROM idlebot_live_state WHERE bot_name = %s AND character_guid IS NOT NULL",
        (name,),
    )
    if row and row["character_guid"]:
        return int(row["character_guid"])
    row = await db.fetchone(
        "SELECT character_guid FROM idlebot_bots WHERE bot_name = %s",
        (name,),
    )
    if row and row["character_guid"]:
        return int(row["character_guid"])
    row = await db.fetchone("SELECT guid FROM characters WHERE name = %s", (name,))
    return int(row["guid"]) if row and row.get("guid") else None


async def _live_table_ok() -> bool:
    global _live_state_exists
    if _live_state_exists is None:
        _live_state_exists = await db.table_exists("idlebot_live_state")
    return _live_state_exists


def _stale_fields(updated_at, source: str) -> dict:
    """Return is_stale + stale_seconds for live_state rows only."""
    if source != "live_state" or updated_at is None:
        return {"is_stale": False, "stale_seconds": None, "live_state_status": "live"}
    now = datetime.now(timezone.utc)
    if updated_at.tzinfo is None:
        updated_at = updated_at.replace(tzinfo=timezone.utc)
    secs = (now - updated_at).total_seconds()
    if secs > STALE_THRESHOLD_S:
        status = "stale"
    elif secs > DELAYED_THRESHOLD_S:
        status = "delayed"
    else:
        status = "live"
    return {"is_stale": secs > STALE_THRESHOLD_S, "stale_seconds": round(secs, 1), "live_state_status": status}


def _ensure_utc(dt: datetime | None) -> datetime | None:
    if dt is None:
        return None
    if dt.tzinfo is None:
        return dt.replace(tzinfo=timezone.utc)
    return dt.astimezone(timezone.utc)


def _event_status(row: dict) -> str:
    created_at = _ensure_utc(row.get("created_at"))
    if created_at and created_at > datetime.now(timezone.utc):
        return "invalid_timestamp"

    event_session = row.get("bot_session_id")
    current_session = row.get("current_bot_session_id")
    event_reset = int(row.get("reset_id") or 0)
    current_reset = int(row.get("current_reset_id") or 0)
    if current_session and event_session and current_session != event_session:
        return "stale"
    if current_reset and event_reset and current_reset != event_reset:
        return "stale"

    if row.get("event_type") == "FAILURE":
        current_state = row.get("current_state") or ""
        current_failure = row.get("current_last_failure_code")
        if current_state == "blocked" and current_failure and current_failure == row.get("event_code"):
            return "active"
        return "resolved"
    return "current"


def _row_to_summary(row: dict, source: str = "live_state") -> BotSummary:
    pos = BotPosition(
        map_id=row.get("map_id") or 0,
        x=row.get("x") or 0.0,
        y=row.get("y") or 0.0,
        z=row.get("z") or 0.0,
        orientation=row.get("orientation") or 0.0,
    )
    target = None
    if row.get("target_entry"):
        raw_target_level = row.get("target_level")
        template_min_level = row.get("target_minlevel")
        template_max_level = row.get("target_maxlevel")
        target_level = raw_target_level if raw_target_level and int(raw_target_level) > 0 else (template_max_level or template_min_level)

        target = BotTarget(
            entry=row.get("target_entry"),
            name=row.get("target_name") or row.get("target_template_name"),
            level=target_level,
            distance=row.get("target_distance"),
            role=row.get("target_role"),
        )
    class_id = row.get("class_id") or row.get("class")
    race_id = row.get("race_id") or row.get("race")
    guide_id = row.get("guide_id")
    alive = bool(row.get("alive", True))
    is_ghost = bool(row.get("is_ghost", False))
    state = row.get("state") or row.get("step_state") or "idle"
    if row.get("step_state") == "blocked" or row.get("blocked_reason") or row.get("last_failure_code") or row.get("requires_user_action"):
        state = "blocked"
    return BotSummary(
        bot_name=row.get("bot_name") or row.get("name") or "",
        character_guid=row.get("character_guid") or row.get("guid"),
        class_id=class_id,
        class_name=CLASS_NAMES.get(class_id) if class_id else None,
        race_id=race_id,
        race_name=RACE_NAMES.get(race_id) if race_id else None,
        level=row.get("level"),
        map_id=pos.map_id,
        x=pos.x,
        y=pos.y,
        z=pos.z,
        orientation=pos.orientation,
        position=pos,
        hp_pct=float(row.get("hp_pct") or 100),
        mana_pct=float(row.get("mana_pct") or 100),
        alive=alive,
        is_ghost=is_ghost,
        in_combat=bool(row.get("in_combat", False)),
        state=state,
        status=derive_status(state, alive, is_ghost, guide_id),
        guide_id=guide_id,
        step_index=int(row.get("step_index") or 0),
        step_total=int(row.get("step_total") or 0),
        step_name=row.get("step_name"),
        quest_id=row.get("quest_id"),
        objective_text=row.get("objective_text"),
        target_entry=target.entry if target else None,
        target_name=target.name if target else None,
        target_level=target.level if target else None,
        target_distance=target.distance if target else None,
        target_role=target.role if target else None,
        target=target,
        death_count_total=int(row.get("death_count_total") or 0),
        death_count_current_step=int(row.get("death_count_current_step") or 0),
        money=int(row.get("money") or 0),
        bag_used=int(row.get("bag_used") or 0),
        bag_total=int(row.get("bag_total") or 0),
        durability_pct=int(row.get("durability_pct") or 100),
        soak_run_id=row.get("soak_run_id"),
        bot_session_id=row.get("bot_session_id"),
        reset_id=int(row.get("reset_id") or 0),
        blocked_reason=row.get("blocked_reason"),
        blocked_since=_ensure_utc(row.get("blocked_since")),
        last_failure_code=row.get("last_failure_code"),
        requires_user_action=bool(row.get("requires_user_action", False)),
        updated_at=_ensure_utc(row.get("updated_at")),
        data_source=source,
        **_stale_fields(row.get("updated_at"), source),
    )


def _in_placeholders(values: list[int]) -> str:
    return ",".join(["%s"] * len(values))


def _objective_text(row: dict, index: int, fallback: str) -> str:
    text = row.get(f"ObjectiveText{index}")
    return text if text else fallback


async def _fetch_name_map(table: str, id_column: str, name_column: str, ids: set[int]) -> dict[int, str]:
    if not ids:
        return {}
    ordered = sorted(ids)
    rows = await db.fetchall(
        f"SELECT {id_column} AS id, {name_column} AS name FROM {table} WHERE {id_column} IN ({_in_placeholders(ordered)})",
        tuple(ordered),
    )
    return {int(r["id"]): str(r["name"]) for r in rows if r.get("name")}


async def _build_quest_objectives(rows: list[dict]) -> dict[int, list[QuestObjective]]:
    npc_ids: set[int] = set()
    go_ids: set[int] = set()
    item_ids: set[int] = set()

    for row in rows:
        for i in range(1, 5):
            target_id = int(row.get(f"RequiredNpcOrGo{i}") or 0)
            if target_id > 0:
                npc_ids.add(target_id)
            elif target_id < 0:
                go_ids.add(abs(target_id))
        for i in range(1, 7):
            item_id = int(row.get(f"RequiredItemId{i}") or 0)
            if item_id:
                item_ids.add(item_id)

    npc_names = await _fetch_name_map("acore_world.creature_template", "entry", "name", npc_ids)
    go_names = await _fetch_name_map("acore_world.gameobject_template", "entry", "name", go_ids)
    item_names = await _fetch_name_map("acore_world.item_template", "entry", "name", item_ids)

    result: dict[int, list[QuestObjective]] = {}
    for row in rows:
        objectives: list[QuestObjective] = []

        for i in range(1, 5):
            target_id = int(row.get(f"RequiredNpcOrGo{i}") or 0)
            required = int(row.get(f"RequiredNpcOrGoCount{i}") or 0)
            if not target_id or required <= 0:
                continue

            progress = int(row.get(f"mobcount{i}") or 0)
            if target_id > 0:
                name = npc_names.get(target_id, f"Creature {target_id}")
                kind = "creature"
                display_id = target_id
            else:
                display_id = abs(target_id)
                name = go_names.get(display_id, f"Object {display_id}")
                kind = "object"

            objectives.append(QuestObjective(
                kind=kind,
                text=_objective_text(row, i, name),
                progress=progress,
                required=required,
                target_id=display_id,
                target_name=name,
            ))

        for i in range(1, 7):
            item_id = int(row.get(f"RequiredItemId{i}") or 0)
            required = int(row.get(f"RequiredItemCount{i}") or 0)
            if not item_id or required <= 0:
                continue
            name = item_names.get(item_id, f"Item {item_id}")
            objectives.append(QuestObjective(
                kind="item",
                text=name,
                progress=int(row.get(f"itemcount{i}") or 0),
                required=required,
                target_id=item_id,
                target_name=name,
            ))

        required_kills = int(row.get("RequiredPlayerKills") or 0)
        if required_kills > 0:
            objectives.append(QuestObjective(
                kind="player",
                text="Player kills",
                progress=int(row.get("playercount") or 0),
                required=required_kills,
            ))

        if not objectives and int(row.get("explored") or 0) > 0:
            objectives.append(QuestObjective(
                kind="explore",
                text="Explore objective",
                progress=1,
                required=1,
            ))

        result[int(row["quest_id"])] = objectives

    return result


def _status_label(status: int | None) -> str:
    return {1: "complete", 3: "in_progress", 5: "failed"}.get(status or 0, "unknown")


def _parse_guide_action(step_name: str | None, objective_text: str | None, quest_id: int | None) -> tuple[str, int | None, int | None, str]:
    raw = objective_text or step_name or ""
    match = re.search(r"([a-z_]+):(\d+)(?:/(\d+))?", raw)
    if match:
        return match.group(1), int(match.group(2)), int(match.group(3)) if match.group(3) else None, raw

    lower = (step_name or "").lower()
    if lower.startswith("accept "):
        return "accept", quest_id, None, raw
    if lower.startswith("turn in "):
        return "turnin", quest_id, None, raw
    if "objective" in lower:
        idx_match = re.search(r"objective\s+(\d+)", lower)
        return "quest_objective_complete", quest_id, int(idx_match.group(1)) if idx_match else None, raw
    return "unknown", quest_id, None, raw


def _action_label(action_type: str) -> str:
    return {
        "accept": "Pick up",
        "turnin": "Turn in",
        "quest_objective_complete": "Complete objective",
        "quest_complete": "Complete quest",
        "quest_turnin": "Turn in",
        "quest_accept": "Pick up",
    }.get(action_type, action_type.replace("_", " ").title() if action_type else "Unknown")


async def _fetch_quest_context(character_guid: int | None, quest_id: int) -> dict | None:
    return await db.fetchone(
        "SELECT qt.ID AS quest_id, qt.LogTitle AS title, qt.QuestLevel AS quest_level, "
        "qt.LogDescription AS description, qt.QuestCompletionLog AS completion_log, "
        "qt.RequiredNpcOrGo1, qt.RequiredNpcOrGo2, qt.RequiredNpcOrGo3, qt.RequiredNpcOrGo4, "
        "qt.RequiredNpcOrGoCount1, qt.RequiredNpcOrGoCount2, qt.RequiredNpcOrGoCount3, qt.RequiredNpcOrGoCount4, "
        "qt.RequiredItemId1, qt.RequiredItemId2, qt.RequiredItemId3, qt.RequiredItemId4, qt.RequiredItemId5, qt.RequiredItemId6, "
        "qt.RequiredItemCount1, qt.RequiredItemCount2, qt.RequiredItemCount3, qt.RequiredItemCount4, qt.RequiredItemCount5, qt.RequiredItemCount6, "
        "qt.RequiredPlayerKills, qt.ObjectiveText1, qt.ObjectiveText2, qt.ObjectiveText3, qt.ObjectiveText4, "
        "qs.status, qs.explored, qs.playercount, "
        "qs.mobcount1, qs.mobcount2, qs.mobcount3, qs.mobcount4, "
        "qs.itemcount1, qs.itemcount2, qs.itemcount3, qs.itemcount4, qs.itemcount5, qs.itemcount6 "
        "FROM acore_world.quest_template qt "
        "LEFT JOIN character_queststatus qs ON qs.quest = qt.ID AND qs.guid = %s "
        "WHERE qt.ID = %s",
        (character_guid or 0, quest_id),
    )


async def _fetch_quest_actor(quest_id: int, ender: bool) -> BotTarget | None:
    creature_table = "creature_questender" if ender else "creature_queststarter"
    gameobject_table = "gameobject_questender" if ender else "gameobject_queststarter"
    creature = await db.fetchone(
        f"SELECT c.id AS entry, ct.name, ct.maxlevel AS level FROM acore_world.{creature_table} c "
        "LEFT JOIN acore_world.creature_template ct ON c.id = ct.entry "
        "WHERE c.quest = %s LIMIT 1",
        (quest_id,),
    )
    if creature:
        return BotTarget(entry=creature["entry"], name=creature.get("name"), level=creature.get("level"), role="quest_ender" if ender else "quest_starter")

    gameobject = await db.fetchone(
        f"SELECT g.id AS entry, gt.name FROM acore_world.{gameobject_table} g "
        "LEFT JOIN acore_world.gameobject_template gt ON g.id = gt.entry "
        "WHERE g.quest = %s LIMIT 1",
        (quest_id,),
    )
    if gameobject:
        return BotTarget(entry=gameobject["entry"], name=gameobject.get("name"), role="quest_ender" if ender else "quest_starter")
    return None


def _remember_unresolved(guide: str | None, step_index: int, raw: str, quest_id: int | None, objective_index: int | None, reason: str) -> None:
    key = f"{guide}:{step_index}:{raw}:{quest_id}:{objective_index}"
    if key in _unresolved_guide_steps:
        return
    item = {
        "guide": guide,
        "stepIndex": step_index,
        "raw": raw,
        "questId": quest_id,
        "objectiveIndex": objective_index,
        "reason": reason,
    }
    _unresolved_guide_steps[key] = item
    log.warning(
        "[GuideStepEnrich] unresolved quest objective: guide=%s step=%s raw=%s questId=%s objectiveIndex=%s reason=%s",
        guide, step_index, raw, quest_id, objective_index, reason,
    )


_npc_zone_cache: dict[int, str] = {}

async def _get_npc_zone(npc_entry: int) -> str | None:
    """Return a simple zone tag for a creature entry, or None if unknown.
    Uses the creature's spawn coordinates to classify the zone.
    Result is cached per entry (zone doesn't change between requests).
    """
    if npc_entry in _npc_zone_cache:
        return _npc_zone_cache[npc_entry]
    row = await db.fetchone(
        "SELECT position_x AS x, position_y AS y, map FROM acore_world.creature WHERE id1 = %s LIMIT 1",
        (npc_entry,),
    )
    zone: str | None = None
    if row:
        x, y, map_id = float(row.get("x") or 0), float(row.get("y") or 0), int(row.get("map") or 0)
        # Moonglade: map 1 (Kalimdor), x:[7000,9200], y:[-4000,-2000]
        if map_id == 1 and 7000.0 <= x <= 9200.0 and -4000.0 <= y <= -2000.0:
            zone = "moonglade"
    _npc_zone_cache[npc_entry] = zone
    return zone


async def enrich_guide_step(row: dict) -> EnrichedGuideStep | None:
    guide = row.get("guide_id")
    step_index = int(row.get("step_index") or 0)
    action_type, quest_id, objective_index, raw = _parse_guide_action(row.get("step_name"), row.get("objective_text"), row.get("quest_id"))
    if not raw and not quest_id:
        return None

    enriched = EnrichedGuideStep(
        guideName=guide,
        stepIndex=step_index,
        rawAction=raw,
        actionType=action_type,
        actionLabel=_action_label(action_type),
        questId=quest_id,
        objectiveIndex=objective_index,
        displayTitle=row.get("step_name") or "Guide step",
        displaySubtitle=row.get("objective_text") or None,
        debugRaw=raw,
        source="guide",
    )

    if not quest_id:
        enriched.unresolvedReason = "no quest id in live state or guide action"
        _remember_unresolved(guide, step_index, raw, None, objective_index, enriched.unresolvedReason)
        return enriched

    qrow = await _fetch_quest_context(row.get("character_guid") or row.get("guid"), quest_id)
    if not qrow:
        enriched.unresolvedReason = "quest_template row not found"
        _remember_unresolved(guide, step_index, raw, quest_id, objective_index, enriched.unresolvedReason)
        return enriched

    objective_map = await _build_quest_objectives([qrow])
    objectives = objective_map.get(int(quest_id), [])
    objective = objectives[objective_index - 1] if objective_index and 0 < objective_index <= len(objectives) else (objectives[0] if objectives else None)

    enriched.questTitle = qrow.get("title")
    enriched.questLevel = qrow.get("quest_level")
    enriched.displayTitle = qrow.get("title") or f"Quest {quest_id}"
    enriched.status = _status_label(qrow.get("status"))
    enriched.confidence = "medium"

    if objective:
        enriched.objectiveType = objective.kind
        enriched.objectiveText = objective.text
        enriched.current = objective.progress
        enriched.required = objective.required
        enriched.displaySubtitle = objective.text
        enriched.confidence = "high"
        if objective.kind == "creature" and objective.target_id:
            enriched.targetEntries = [objective.target_id]
            if objective.target_name:
                enriched.targetNames = [objective.target_name]
        elif objective.kind == "object" and objective.target_id:
            enriched.gameObjectIds = [objective.target_id]
            if objective.target_name:
                enriched.gameObjectNames = [objective.target_name]
        elif objective.kind == "item" and objective.target_id:
            enriched.itemIds = [objective.target_id]
            if objective.target_name:
                enriched.itemNames = [objective.target_name]
    elif action_type == "quest_objective_complete":
        enriched.unresolvedReason = "no matching objective field found"
        _remember_unresolved(guide, step_index, raw, quest_id, objective_index, enriched.unresolvedReason)

    if action_type in ("accept", "quest_accept"):
        enriched.pickup = await _fetch_quest_actor(quest_id, ender=False)
        enriched.displaySubtitle = f"Pick up from {enriched.pickup.name}" if enriched.pickup and enriched.pickup.name else "Pick up quest"
    elif action_type in ("turnin", "quest_turnin"):
        enriched.turnin = await _fetch_quest_actor(quest_id, ender=True)
        enriched.displaySubtitle = f"Turn in to {enriched.turnin.name}" if enriched.turnin and enriched.turnin.name else "Turn in quest"
        # Detect if the turn-in NPC is in Moonglade (requires druid class travel).
        if enriched.turnin and enriched.turnin.entry:
            npc_zone = await _get_npc_zone(enriched.turnin.entry)
            if npc_zone == "moonglade":
                enriched.requiresSpecialTravel = True
                enriched.specialTravelType = "druid_teleport_moonglade"
                enriched.targetMap = "Moonglade"
                enriched.confidence = "high"

    return enriched


async def _attach_enriched_guide_steps(summaries: list[BotSummary], source_rows: list[dict]) -> list[BotSummary]:
    by_name = {summary.bot_name: summary for summary in summaries}
    for row in source_rows:
        name = row.get("bot_name") or row.get("name")
        if name in by_name:
            by_name[name].enriched_guide_step = await enrich_guide_step(row)
    return summaries


async def _fetch_all_summaries() -> list[BotSummary]:
    """Fetch all bot summaries, falling back to idlebot_bots+characters if live_state missing."""
    use_live = await _live_table_ok()

    if use_live:
        rows = await db.fetchall(
            "SELECT ls.*, b.death_count_total, b.death_count_current_step, "
            "COALESCE(ls.soak_run_id, b.soak_run_id) AS soak_run_id, "
            "COALESCE(ls.bot_session_id, b.bot_session_id) AS bot_session_id, "
            "COALESCE(ls.reset_id, b.reset_id) AS reset_id, "
            "COALESCE(ls.blocked_reason, b.blocked_reason) AS blocked_reason, "
            "COALESCE(ls.blocked_since, b.blocked_since) AS blocked_since, "
            "COALESCE(ls.last_failure_code, b.last_failure_code) AS last_failure_code, "
            "COALESCE(ls.requires_user_action, b.requires_user_action) AS requires_user_action, "
            "b.step_state, b.guide_id AS b_guide_id, "
            "ct.name AS target_template_name, ct.minlevel AS target_minlevel, ct.maxlevel AS target_maxlevel "
            "FROM idlebot_live_state ls "
            "LEFT JOIN idlebot_bots b ON ls.bot_name = b.bot_name "
            "LEFT JOIN acore_world.creature_template ct ON ls.target_entry = ct.entry"
        )
        # For bots in idlebot_bots but not yet in live_state, add fallback rows
        all_bots = await db.fetchall(
            "SELECT b.bot_name, b.guide_id, b.step_index, b.step_state, "
            "b.death_count_total, b.death_count_current_step, "
            "b.soak_run_id, b.bot_session_id, b.reset_id, b.blocked_reason, b.blocked_since, "
            "b.last_failure_code, b.requires_user_action, "
            "c.guid AS character_guid, c.level, c.class AS class_id, "
            "c.race AS race_id, c.money "
            "FROM idlebot_bots b "
            "LEFT JOIN characters c ON b.character_guid = c.guid "
            "WHERE b.active = 1"
        )
        live_names = {r["bot_name"] for r in rows}
        results = [_row_to_summary(r, "live_state") for r in rows]
        source_rows = list(rows)
        for r in all_bots:
            if r["bot_name"] not in live_names:
                results.append(_row_to_summary(r, "fallback"))
                source_rows.append(r)
        return await _attach_enriched_guide_steps(results, source_rows)

    # No live_state table — pure fallback
    rows = await db.fetchall(
        "SELECT b.bot_name, b.guide_id, b.step_index, b.step_state, "
        "b.death_count_total, b.death_count_current_step, "
        "c.guid AS character_guid, c.level, c.class AS class_id, "
        "c.race AS race_id, c.money "
        "FROM idlebot_bots b "
        "LEFT JOIN characters c ON b.character_guid = c.guid "
        "WHERE b.active = 1"
    )
    results = [_row_to_summary(r, "fallback") for r in rows]
    return await _attach_enriched_guide_steps(results, rows)


async def _fetch_one_summary(name: str) -> BotSummary | None:
    use_live = await _live_table_ok()

    if use_live:
        row = await db.fetchone(
            "SELECT ls.*, b.death_count_total, b.death_count_current_step, "
            "COALESCE(ls.soak_run_id, b.soak_run_id) AS soak_run_id, "
            "COALESCE(ls.bot_session_id, b.bot_session_id) AS bot_session_id, "
            "COALESCE(ls.reset_id, b.reset_id) AS reset_id, "
            "COALESCE(ls.blocked_reason, b.blocked_reason) AS blocked_reason, "
            "COALESCE(ls.blocked_since, b.blocked_since) AS blocked_since, "
            "COALESCE(ls.last_failure_code, b.last_failure_code) AS last_failure_code, "
            "COALESCE(ls.requires_user_action, b.requires_user_action) AS requires_user_action, "
            "b.step_state, ct.name AS target_template_name, "
            "ct.minlevel AS target_minlevel, ct.maxlevel AS target_maxlevel "
            "FROM idlebot_live_state ls "
            "LEFT JOIN idlebot_bots b ON ls.bot_name = b.bot_name "
            "LEFT JOIN acore_world.creature_template ct ON ls.target_entry = ct.entry "
            "WHERE ls.bot_name = %s",
            (name,),
        )
        if row:
            summary = _row_to_summary(row, "live_state")
            enriched = await enrich_guide_step(row)
            summary.enriched_guide_step = enriched
            return summary

    # Fallback to idlebot_bots + characters
    row = await db.fetchone(
        "SELECT b.bot_name, b.guide_id, b.step_index, b.step_state, "
        "b.death_count_total, b.death_count_current_step, "
        "b.soak_run_id, b.bot_session_id, b.reset_id, b.blocked_reason, b.blocked_since, "
        "b.last_failure_code, b.requires_user_action, "
        "c.guid AS character_guid, c.level, c.class AS class_id, "
        "c.race AS race_id, c.money "
        "FROM idlebot_bots b "
        "LEFT JOIN characters c ON b.character_guid = c.guid "
        "WHERE b.bot_name = %s",
        (name,),
    )
    if not row:
        return None
    summary = _row_to_summary(row, "fallback")
    summary.enriched_guide_step = await enrich_guide_step(row)
    return summary


# ─── routes ─────────────────────────────────────────────────────────────────


@router.get("/bots", response_model=list[BotSummary])
async def list_bots():
    return await _fetch_all_summaries()


@router.get("/bots/{name}", response_model=BotDetail)
async def get_bot(name: str):
    summary = await _fetch_one_summary(name)
    if summary is None:
        raise HTTPException(status_code=404, detail=f"Bot '{name}' not found")
    return BotDetail(**summary.model_dump())


@router.get("/bots/{name}/inventory")
async def get_inventory(name: str):
    try:
        cguid = await _resolve_guid(name)
        if not cguid:
            return JSONResponse({"items": [], "error": "No character GUID linked"})

        rows = await db.fetchall(
            "SELECT ci.bag AS bag_id, "
            "CASE WHEN ci.bag = 0 THEN 0 ELSE bag_ci.slot END AS bag_slot, "
            "CASE WHEN ci.bag = 0 THEN 'Backpack' ELSE bag_it.name END AS bag_name, "
            "ci.slot, ii.itemEntry AS item_entry, ii.count, ii.durability, "
            "it.MaxDurability AS max_durability, it.name AS item_name "
            "FROM character_inventory ci "
            "JOIN item_instance ii ON ci.item = ii.guid "
            "LEFT JOIN acore_world.item_template it ON ii.itemEntry = it.entry "
            "LEFT JOIN character_inventory bag_ci ON ci.bag != 0 AND bag_ci.guid = ci.guid AND bag_ci.item = ci.bag "
            "LEFT JOIN item_instance bag_ii ON bag_ci.item = bag_ii.guid "
            "LEFT JOIN acore_world.item_template bag_it ON bag_ii.itemEntry = bag_it.entry "
            "WHERE ci.guid = %s "
            "AND ((ci.bag = 0 AND ci.slot >= 23 AND ci.slot < 39) OR ci.bag != 0) "
            "ORDER BY bag_slot, ci.slot",
            (cguid,),
        )
        return [
            InventoryItem(
                bag_id=r["bag_id"],
                slot=r["slot"],
                bag_slot=r["bag_slot"] or 0,
                bag_name=r.get("bag_name") or "Unknown Bag",
                item_entry=r["item_entry"],
                item_name=r.get("item_name"),
                count=r.get("count") or 1,
                durability=r.get("durability"),
                max_durability=r.get("max_durability"),
            )
            for r in rows
        ]
    except Exception as exc:
        log.exception("inventory error for %s", name)
        return JSONResponse({"items": [], "error": str(exc)}, status_code=200)


@router.get("/bots/{name}/equipment")
async def get_equipment(name: str):
    try:
        cguid = await _resolve_guid(name)
        if not cguid:
            return JSONResponse({"items": [], "error": "No character GUID linked"})

        rows = await db.fetchall(
            "SELECT ci.slot, ii.itemEntry AS item_entry, "
            "ii.durability, it.MaxDurability AS max_durability, "
            "it.name AS item_name "
            "FROM character_inventory ci "
            "JOIN item_instance ii ON ci.item = ii.guid "
            "LEFT JOIN acore_world.item_template it ON ii.itemEntry = it.entry "
            "WHERE ci.guid = %s AND ci.bag = 0 AND ci.slot < 19 "
            "ORDER BY ci.slot",
            (cguid,),
        )
        return [
            EquipmentItem(
                slot=r["slot"],
                slot_name=EQUIP_SLOTS.get(r["slot"], f"slot_{r['slot']}"),
                item_entry=r["item_entry"],
                item_name=r.get("item_name"),
                durability=r.get("durability"),
                max_durability=r.get("max_durability"),
            )
            for r in rows
        ]
    except Exception as exc:
        log.exception("equipment error for %s", name)
        return JSONResponse({"items": [], "error": str(exc)}, status_code=200)


@router.get("/bots/{name}/quests")
async def get_quests(name: str):
    try:
        cguid = await _resolve_guid(name)
        if not cguid:
            return QuestLog(active=[], completed_count=0, error="No character GUID linked")

        # Active quests: status 1=complete(ready to turn in), 3=incomplete, 5=failed
        rows = await db.fetchall(
            "SELECT qs.quest AS quest_id, qs.status, qs.explored, qs.playercount, "
            "qs.mobcount1, qs.mobcount2, qs.mobcount3, qs.mobcount4, "
            "qs.itemcount1, qs.itemcount2, qs.itemcount3, qs.itemcount4, qs.itemcount5, qs.itemcount6, "
            "qt.LogTitle AS title, qt.LogDescription AS description, qt.QuestCompletionLog AS completion_log, "
            "qt.RequiredNpcOrGo1, qt.RequiredNpcOrGo2, qt.RequiredNpcOrGo3, qt.RequiredNpcOrGo4, "
            "qt.RequiredNpcOrGoCount1, qt.RequiredNpcOrGoCount2, qt.RequiredNpcOrGoCount3, qt.RequiredNpcOrGoCount4, "
            "qt.RequiredItemId1, qt.RequiredItemId2, qt.RequiredItemId3, qt.RequiredItemId4, qt.RequiredItemId5, qt.RequiredItemId6, "
            "qt.RequiredItemCount1, qt.RequiredItemCount2, qt.RequiredItemCount3, qt.RequiredItemCount4, qt.RequiredItemCount5, qt.RequiredItemCount6, "
            "qt.RequiredPlayerKills, qt.ObjectiveText1, qt.ObjectiveText2, qt.ObjectiveText3, qt.ObjectiveText4 "
            "FROM character_queststatus qs "
            "LEFT JOIN acore_world.quest_template qt ON qs.quest = qt.ID "
            "WHERE qs.guid = %s "
            "ORDER BY qt.QuestLevel, qt.LogTitle",
            (cguid,),
        )

        # Map AzerothCore int status → human label
        STATUS_LABEL = {1: "complete", 3: "in_progress", 5: "failed"}
        objective_map = await _build_quest_objectives(rows)
        active = [
            QuestEntry(
                quest_id=r["quest_id"],
                title=r.get("title"),
                status=STATUS_LABEL.get(r.get("status") or 0, "unknown"),
                description=r.get("description"),
                completion_log=r.get("completion_log"),
                objectives=objective_map.get(int(r["quest_id"]), []),
            )
            for r in rows
        ]

        # Rewarded quests count from separate table
        rrow = await db.fetchone(
            "SELECT COUNT(*) AS cnt FROM character_queststatus_rewarded WHERE guid = %s",
            (cguid,),
        )
        completed_count = int(rrow["cnt"]) if rrow else 0

        # Current guide objective from live state
        lrow = await db.fetchone(
            "SELECT bot_name, character_guid, guide_id, step_index, quest_id AS guide_quest_id, "
            "quest_id, step_name AS guide_step, step_name, objective_text AS guide_objective, objective_text "
            "FROM idlebot_live_state WHERE bot_name = %s",
            (name,),
        )
        enriched = await enrich_guide_step(lrow) if lrow else None

        return QuestLog(
            active=active,
            completed_count=completed_count,
            guide_quest_id=lrow["guide_quest_id"] if lrow else None,
            guide_step=lrow["guide_step"] if lrow else None,
            guide_objective=lrow["guide_objective"] if lrow else None,
            enriched_guide_step=enriched,
        )
    except Exception as exc:
        log.exception("quests error for %s", name)
        return QuestLog(active=[], completed_count=0, error=str(exc))


@router.get("/bots/{name}/events", response_model=list[BotEvent])
async def get_events(name: str):
    rows = await db.fetchall(
        "SELECT e.event_type, e.event_code, e.soak_run_id, e.bot_session_id, e.reset_id, "
        "e.detail, e.created_at, "
        "COALESCE(ls.bot_session_id, b.bot_session_id) AS current_bot_session_id, "
        "COALESCE(ls.reset_id, b.reset_id) AS current_reset_id, "
        "COALESCE(ls.state, b.step_state) AS current_state, "
        "COALESCE(ls.last_failure_code, b.last_failure_code) AS current_last_failure_code "
        "FROM idlebot_events e "
        "JOIN idlebot_bots b ON e.bot_id = b.id "
        "LEFT JOIN idlebot_live_state ls ON ls.bot_name = b.bot_name "
        "WHERE b.bot_name = %s "
        "ORDER BY e.created_at DESC LIMIT 50",
        (name,),
    )
    if not rows:
        # Confirm bot exists
        exists = await db.fetchone(
            "SELECT 1 FROM idlebot_bots WHERE bot_name = %s", (name,)
        )
        if not exists:
            raise HTTPException(status_code=404, detail=f"Bot '{name}' not found")
    return [
        BotEvent(
            event_type=r["event_type"],
            event_code=r.get("event_code"),
            soak_run_id=r.get("soak_run_id"),
            bot_session_id=r.get("bot_session_id"),
            reset_id=int(r.get("reset_id") or 0),
            detail=r.get("detail"),
            created_at=_ensure_utc(r.get("created_at")),
            status=_event_status(r),
        )
        for r in rows
    ]


@router.get("/health")
async def health():
    """Dashboard health — reports whether live telemetry is flowing."""
    live_ok = await _live_table_ok()
    if not live_ok:
        return {
            "status": "degraded",
            "live_state": False,
            "message": "idlebot_live_state table not found — using fallback data",
        }

    row = await db.fetchone(
        "SELECT COUNT(*) AS cnt, MAX(updated_at) AS last_update FROM idlebot_live_state"
    )
    count = int(row["cnt"]) if row else 0
    last_update = row["last_update"] if row else None

    if count == 0:
        return {
            "status": "degraded",
            "live_state": True,
            "message": "idlebot_live_state is empty — C++ WriteLiveState not yet running",
        }

    lag = None
    if last_update is not None:
        now = datetime.now(timezone.utc)
        if last_update.tzinfo is None:
            last_update = last_update.replace(tzinfo=timezone.utc)
        lag = round((now - last_update).total_seconds(), 1)
        if lag > 30:
            return {
                "status": "degraded",
                "live_state": True,
                "bot_count": count,
                "lag_seconds": lag,
                "message": f"Live state stale: last update {lag}s ago",
            }

    return {
        "status": "ok",
        "live_state": True,
        "bot_count": count,
        "lag_seconds": lag,
        "last_update": last_update.isoformat() if last_update else None,
    }


@router.get("/policy")
async def policy_status():
    """Playerlike policy status — shows whether cheat flags are enabled.
    Reads the worldserver config from well-known paths inside the container."""
    import os
    import re as _re

    CONF_PATHS = [
        "/opt/azeroth-server/etc/mod_idlebot.conf",
        "/home/khuong/azeroth-server/etc/mod_idlebot.conf",
        "./mod_idlebot.conf",
    ]

    flags = {
        "IdleBot.PlayerlikeMode": "1",
        "IdleBot.AllowCheatTeleport": "0",
        "IdleBot.AllowCheatResurrect": "0",
        "IdleBot.AllowForceQuestAdvance": "0",
        "IdleBot.AllowForceSkipForSoak": "0",
        "IdleBot.AllowGMRecovery": "0",
    }

    conf_source = "defaults (no config found)"
    for path in CONF_PATHS:
        if os.path.exists(path):
            try:
                text = open(path).read()
                for key in list(flags.keys()):
                    m = _re.search(rf"^\s*{_re.escape(key)}\s*=\s*(\S+)", text, _re.MULTILINE)
                    if m:
                        flags[key] = m.group(1).strip('"\'')
                conf_source = path
                break
            except Exception:
                pass

    # Also fetch quarantine events from the last 12 hours
    quarantined_bots = []
    try:
        rows = await db.fetchall(
            "SELECT b.bot_name, e.detail, e.created_at "
            "FROM idlebot_events e "
            "JOIN idlebot_bots b ON e.bot_id = b.id "
            "WHERE e.event_type = 'FAILURE' "
            "  AND e.detail LIKE '%quarantin%' "
            "  AND e.created_at >= NOW() - INTERVAL 12 HOUR "
            "ORDER BY e.created_at DESC "
            "LIMIT 20"
        )
        for r in rows:
            quarantined_bots.append({
                "bot_name": r["bot_name"],
                "reason": r.get("detail", "")[:200],
                "quarantined_at": r["created_at"].isoformat() if r.get("created_at") else None,
            })
    except Exception:
        pass

    playerlike_mode = flags.get("IdleBot.PlayerlikeMode", "1") in ("1", "true", "yes")
    cheat_flags = {k: v for k, v in flags.items() if k != "IdleBot.PlayerlikeMode"}
    any_cheat = any(v not in ("0", "false", "no") for v in cheat_flags.values())
    policy_ok = playerlike_mode and not any_cheat

    return {
        "playerlike_mode": playerlike_mode,
        "cheat_recovery": "OFF" if not any_cheat else "ON — WARNING",
        "policy_ok": policy_ok,
        "flags": flags,
        "config_source": conf_source,
        "quarantined_bots": quarantined_bots,
        "quarantined_count": len(quarantined_bots),
        "message": "Playerlike mode active — all cheat flags OFF" if policy_ok
                   else "WARNING: cheat/debug flags enabled — soak data invalid",
    }


# Expose fetch helpers for the WS router
async def fetch_all_summaries() -> list[BotSummary]:
    return await _fetch_all_summaries()


@router.get("/debug/unresolved-guide-steps")
async def unresolved_guide_steps():
    return list(_unresolved_guide_steps.values())
