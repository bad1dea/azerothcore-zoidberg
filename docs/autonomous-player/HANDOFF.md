# Session Handoff

## Current milestone
**Gate 2 — COMPLETE (2026-07-01).** Ten slices verified live across two
races/classes (Orc Warrior, Human Priest) — user confirmed this
representative sample satisfies Gate 2's "every race" bar (see
`ROADMAP.md`'s Week 3 entry).

**Gate 3 — levels 1–12, IN PROGRESS.** First slice done: `GuideRuntime`
(ADR-019), the project's first real automatic multi-step execution —
every prior capability in this arc required a human to trigger each
individual step; this is the first genuinely autonomous behavior.
Verified live: a bot completed a 3-waypoint patrol with **zero commands**
issued between `guidestart` and completion. Full per-slice history is in
`KNOWN_FAILURES.md` and `ARCHITECTURE.md` (ADR-008 through ADR-019) —
this file stays a live summary, not a growing archive.

## What's proven, end to end, through real production code (not
## reimplemented or DB-shortcut)
**Orc Warrior** (`Grunttestbot`, Valley of Trials) and **Human Priest**
(`Priestestbot`, Northshire Abbey) both independently complete the full
arc: correct racial spawn (no teleport) → real navmesh movement → real
quest accept/turn-in → real melee combat (and, for casters, a real
finding that a level-1 Priest has no offensive spell yet — not a defect)
→ real loot → real death/recovery → real vendor buy/repair and
gossip/trainer interaction (both correctly blocked once by genuine
insufficient-funds validation). See `ARCHITECTURE.md` ADR-008 through
ADR-018 for the full per-component history.

**New this session: automatic multi-step execution.** `GuideRuntime::Tick`
is now called every second per registered bot from
`BotLifecycleMgr::Update` (previously that tick fired and did nothing —
pure Gate 0 bookkeeping). `.autonomousplayer guidestart <charname>`
attaches a fixed 3-waypoint patrol and starts it; verified live that the
bot's `CurrentStep` advanced 0→1→2→3 (`finished=true`) and its final
position exactly matched the last waypoint, with no command issued after
the initial `guidestart`. Deliberately minimal: one step type (`MoveTo`)
only, no persistence, no combat-in-guide yet — see ADR-019.

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-019.
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- Components: `Lifecycle/BotSessionMgr`, `Setup/PendingCharacterCreations`,
  `Navigation/BotNavigation`, `QuestEngine/BotQuestEngine`, `Combat/BotCombat`
  (melee + `RequestCastSpell`), `Inventory/BotLoot`, `Recovery/BotRecovery`,
  `Economy/BotEconomy`, `Gossip/BotGossip`, `Growth/BotGrowth`,
  `GuideRuntime/BotGuideRuntime` (all `.h`/`.cpp` pairs).
- `Lifecycle/BotLifecycleMgr.{h,cpp}`: `BotSession` now carries a
  `GuideRuntime::BotGuideState`; `Update()` dispatches `GuideRuntime::Tick`
  on each per-bot second-tick — the first time that dispatch point has
  ever done anything.
- `Commands/cs_autonomousplayer.cpp` now has ~24 debug commands
  (`provision`, `login`, `status`, `moveto`, `acceptquest`, `queststatus`,
  `turnin`, `attack`, `creaturestatus`, `loot`, `releasespirit`,
  `reclaimcorpse`, `attackguid` [unreliable, see below], `multipull`,
  `buy`, `repair`, `gossiphello`, `gossiptrain`, `learnspell`,
  `castspell`, `spellbook`, `guidestart`, `guidestatus`) — all still
  debug-only triggers except `guidestart`, which is the first command
  that kicks off *autonomous* behavior rather than a one-shot action.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 29 times across this arc (a few fix-and-retry
  cycles for compile errors caught before ever reaching live testing);
  currently deployed commit compiles clean.
- Every capability above verified **live** on zoidberg, not just compiled.

## Current repository state
- Branch: `mod-autonomous-player`. Most recent commit: `cc45ec0`
  (GuideRuntime), plus this handoff commit — all pushed to origin.
- zoidberg's live `ac-worldserver` is running the latest pushed commit.
- Two reusable test fixtures on zoidberg:
  - account `ap_test1` (id 204), character `Grunttestbot` (guid 2014, Orc
    Warrior, level 2, 0 copper), currently at `-618.5, -4251.7, 38.7` on
    map 1 (Valley of Trials spawn area, end of its just-completed patrol).
  - account `ap_priest1` (id 205), character `Priestestbot` (guid 2015,
    Human Priest, level 1, 0 copper, quest 783 rewarded), near Marshal
    McBride (`-8902.6, -162.6, 81.9` on map 0, Northshire Abbey).
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
None currently blocking. Full history (6 bugs in Gate 1, 1 in Gate 2 —
all fixed) plus several documented non-bug findings (quest interaction
range, quest-gated loot, no-graveyard-nearby ghost behavior, insufficient-
funds rejections, no-offensive-spell-at-level-1) is in
`KNOWN_FAILURES.md`. One minor, non-blocking anomaly noted there too: a
one-time transient `provision` failure right after a fresh redeploy,
succeeded on identical retry — not root-caused (debug/test-provisioning
path only, not the runtime bot loop).

## Decisions made
- User's standing direction has escalated across this arc: "investigate
  how playerbots keeps its sessions alive - fix it and get to gate 3 on
  your own" → "keep going" → "keep going im sleeping" → "continue on your
  own until we get to gate 5." Interpreted as: keep working autonomously
  through this project's own bounded-increment/gate methodology
  indefinitely — design briefly, implement the smallest testable
  increment, compile-check, live-verify on zoidberg, update docs, commit,
  before starting the next thing — only pausing for a genuine blocker or
  a decision only the user can make.
- User explicitly confirmed (2026-07-01, `AskUserQuestion`) that Gate 2's
  race-coverage bar is a representative sample (2 races/classes), not all
  ten WotLK races literally — this was a real scope decision (hours of
  work either way), correctly escalated rather than guessed.
- Every opcode-reuse component follows the pattern established in Gate 1:
  find the real public `WorldSession::Handle*Opcode`, feed it a
  synthesized packet, read "what's available" state directly off the live
  server-side object. **One deliberate exception:**
  `Combat::RequestCastSpell` calls `Unit::CastSpell` directly (ADR-018).
  `GuideRuntime` is not an opcode-reuse component at all — it's a
  scheduler that calls the *other* components' already-proven primitives
  automatically; this distinction matters for how future guide step types
  get added (compose existing Request* functions, don't invent new opcode
  synthesis inside GuideRuntime itself).

## NEXT TASK
Gate 3's natural next slice: **add a combat-capable step type to
GuideRuntime** (e.g. `StepType::KillNearest`: walk to + attack + loot the
nearest creature of a given entry, fully automatic, composing the
already-proven `Navigation`/`Combat`/`Inventory` primitives — no new
opcode work). This is the natural progression from "walk automatically"
to "do a representative quest/combat loop automatically," working toward
Gate 3's "dense camps... ranged/melee pulls" language.

Design considerations:
1. `KillNearest` needs its own internal sub-phase (e.g. approaching →
   attacking → looting → done) since it's not a single fire-and-check
   action like `MoveTo` — think through how `BotGuideState`/`GuideStep`
   should represent that (an inner enum on the step, or a separate
   per-step-type progress field) before implementing.
2. Reuse `Combat::RequestAttack`, `Inventory::LootCorpse` — no new
   `WorldSession::Handle*` calls needed for this slice.
3. Test with the already-known Mottled Boar/Scorpid Worker mobs near
   `Grunttestbot`'s current position, or the Diseased Young Wolf near
   `Priestestbot` — either fixture works.
4. Verify live the same way as this slice: start the guide once, then
   only poll status — no manual attack/loot commands.

After that, remaining Gate 3 scope (per `ROADMAP.md`): dense camps/caves,
ranged pulls, pets, full bags, broader guide validation, more race/class
combos completing starting-region routes. Pick incrementally, same
discipline as every slice so far.

## Next-session acceptance criteria
- A combat-capable `GuideRuntime` step type is implemented, compiled
  clean, and verified live: a bot walks to, kills, and loots a real
  creature with no manual command after the guide starts.
- `check_no_playerbots_dependency.sh` and `check_no_forbidden_apis.sh`
  still pass.
- Docs updated (new ADR if the sub-phase design is non-trivial), committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX}.md. Continue Gate 3 autonomously per the
user's standing instruction: implement HANDOFF.md's NEXT TASK (combat-
capable GuideRuntime step), design briefly, implement the smallest
testable increment, compile-check and live-verify on zoidberg
(build-and-deploy is pre-approved), update docs, commit. Keep going
without stopping to check in, except for a genuine blocker or an
ambiguous decision only the user can make.
