# Session Handoff

## Current milestone
**Gate 2 — COMPLETE (2026-07-01).** Ten slices verified live across two
races/classes (Orc Warrior, Human Priest) — user confirmed this
representative sample satisfies Gate 2's "every race" bar (see
`ROADMAP.md`'s Week 3 entry).

**Gate 3 — levels 1–12, IN PROGRESS, substantially advanced but not
complete.** An external review (2026-07-01) rated process 7/10,
capability 2-3/10, and gave a 7-point priority list. **All 7 priorities
now have real, live-verified progress** (see `ROADMAP.md`'s Week 4 entry
for the full evidence trail and `ARCHITECTURE.md` ADR-026 through
ADR-042 for design detail) — the external review is closed out as an
operative blocker. **Pets (one of Gate 3's own remaining literal-bar
gaps) is now substantially complete**, done same arc (spanning two
sessions) at the user's explicit direction to continue past the
review-closure point: tame/revive/call-pet/auto-tame/ambient-maintenance
are all real, live-verified, `GuideRuntime`-integrated behavior, plus
two real bugs (a race condition and a permanent-hang risk) caught via
self-review and fixed same-session before either could bite a real user.
Summary, calibrated:

- **Fixed, live-verified:** the engagement-confirmation bug the review
  found (`GetVictim()` not `IsInCombat()`); `EncounterModel` now gates a
  real decision (withholds `Engaged` confirmation on an unplanned add);
  every previously-unbounded guide wait is now bounded and one bound was
  directly observed firing; loot success is verified against real
  corpse state, not assumed; a real class-controller composition
  (`KillNearest` + `OpportunisticSpellId`) works for **two** class
  archetypes now — Warrior melee (spell 78) and Hunter ranged (spell 75,
  Auto Shot, ADR-034) — with no code changes needed between them,
  confirming the design is genuinely class-agnostic.
- **Target selection safety (ADR-031):** `IsSafeToEngage` gates
  `KillNearest` on attackability, evade, loot-tag, other-player-
  attacking, and LoS. **4 of 5 checks are live-verified with real
  evidence** (attackable, tag, other-player-attacking, LoS); evade
  remains code-review-only (test creatures die/reset faster than
  console-command latency allows catching it mid-state, same limitation
  as `KNOWN_FAILURES.md` #5). **Caught a real regression before it ever
  shipped**: the first version used `IsHostileTo`, which is `false` for
  most low-level questing wildlife (faction-neutral, not Hostile) — that
  would have made `KillNearest` reject its own most-tested target
  entirely. Fixed to `IsValidAttackTarget`.
- **Automated regression suite (ADR-033):** `tools/live_regression_suite.py`
  — 5 real assertions over the live SOAP interface, including a direct
  regression test for the `IsHostileTo` bug above. First automated
  regression protection this whole project has had; `5/5 passed` live.
- **Four real, previously-unknown bugs found and fixed as a byproduct of
  this work**, all silent-failure-class (matching every Gate 1 bug's
  root shape — a client-feedback path gated on a null socket, or a
  bookkeeping/physical-action split): character creation silently
  stalling on an invalid name (`KNOWN_FAILURES.md` #9, fixed by
  ADR-032's pre-validation); a bounded-timeout guide leaving an
  already-issued `MotionMaster` order running after its own bookkeeping
  gives up, which walked a real bot to its death unattended
  (`KNOWN_FAILURES.md` #10, **fixed and re-verified live**, ADR-035 —
  `OperationTimedOut` now calls `bot->StopMoving()` on every bail-out);
  the `castspell` debug command re-triggering `SPELL_FAILED_MOVING` on
  every retry regardless of actual position, which initially made a
  real, working spell (Tame Beast) look broken (`KNOWN_FAILURES.md` #12,
  fixed, ADR-036); and `creaturestatus`'s pre-existing `FindNearestCreature`
  alive-param footgun (`KNOWN_FAILURES.md` #8, found and now FIXED at the
  source too, same overnight session that closed out the pet-recovery
  work below).
- **Pets, first slice + `GuideRuntime` integration (ADR-037/038):** new
  `Pets` component (`RequestTameBeast`, `PetSnapshot`/`BuildSnapshot`,
  `RequestSetPetReactState`, `RequestAttackTarget`) plus debug commands.
  **Major design-pass finding: taming itself needed zero new module
  code** — it composes entirely from the already-proven
  `Combat::RequestCastSpell` primitive against the real engine's Tame
  Beast spell. `KillNearest` now keeps a live pet on the guide's own
  planned target throughout `Approaching`/`Engaged`
  (`EnsurePetAssists`) — deliberately `REACT_DEFENSIVE` + an explicit
  `CMSG_PET_ACTION` attack command, not `REACT_AGGRESSIVE`, so the pet
  never acquires its own unplanned adds (a design correction made before
  ever verifying the aggressive-only version live, once it was noticed
  that would undermine `EncounterModel`'s ADR-027 gating). **Fully
  live-verified, including the hardest part**: a real pet was tamed,
  persists across a full worldserver restart+relogin (real engine
  behavior), its react state auto-corrected from aggressive to
  defensive within one tick, and — critically — direct evidence
  (`Pet::GetVictim()` matching the guide's own objective target's exact
  guid during a real `Approaching`/`Engaged` fight) proved the pet
  genuinely assists on the *planned* target, not just exists nearby or
  roams for its own.
- **`CombatIntent`/`CombatExecutor` + pet recovery-phase policy, fully
  finished (ADR-039/040):** design correction at the user's explicit
  direction -- pet-revive-on-death was about to be wired in the same
  ad-hoc, isolated-spell-ID way (directly inside `KillNearest`'s pet
  helper, triggered merely by an absent/dead pet). Replaced before
  shipping with a shared decision layer: `Combat::CombatIntent`/
  `Combat::Execute` (one place mapping "what the bot wants" to the real
  primitive call -- melee engage, ability cast, pet-assist, revive, and
  call-pet all route through it), `Pets::PetState`/`ClassifyPetState`
  (the full 6-state model: `NoPet`/`ActiveAlive`/`ActiveDead`/
  `MissingAlive`/`MissingDead`/`Dismissed`, reading real
  `PetStable::GetUnslottedHunterPet()->Health` to tell a dismissed-while-
  alive pet from a dead-and-unslotted one), and `Recovery::PlanPetRecovery`
  (the Singular model's "Recover" stage). Originally checked inside
  `GuideRuntime::Tick` itself, before the current step ever ran; later
  the same overnight session (ADR-042) moved into a separate
  `GuideRuntime::TickAmbient`, called unconditionally regardless of
  guide state -- see below. Guide state is never touched while recovery
  is pending, so it resumes automatically with no explicit save/restore.

  **The investigation initially reached a wrong conclusion** (that this
  was an unfixable structural fork limitation) before the user caught it
  and pointed at the actual fix: `RequestRevivePet` had a real bug,
  bailing out whenever `bot->GetPet()` was null -- exactly the case
  Revive Pet's real effect (`Spell::EffectResurrectPet`) is designed to
  handle via `SummonPet(0, ...)` reloading from `PetStable`/DB. Fixed
  (always self-casts now) and **live-verified twice**: cast against
  `Grunthunter`'s real broken pet (`PetState::MissingDead`, confirmed via
  DB: `slot=100, curhealth=0`), result `SPELL_CAST_OK`, and the *same*
  pet (matching pet number) loaded back in alive both times. Full
  in-order story of the wrong turns and the real fix is in
  `KNOWN_FAILURES.md` #13 -- read it before touching pet recovery again,
  it's genuinely instructive about verifying the actual code path
  instead of the first plausible-looking rejection.

  **Also verified**: no regression (`live_regression_suite.py` still
  `5/5`); a guide runs cleanly with no pet at all.

  **Two follow-up regressions found and fixed by the regression suite
  right after this shipped** (full detail in `KNOWN_FAILURES.md` #13 /
  `ARCHITECTURE.md` ADR-040): the `Tick()`-level recovery check
  originally ran unconditionally, which could (1) preempt an active
  `KillNearest`/quest pursuit whenever `!bot->IsInCombat()` read true
  even momentarily (a real evade, or a brief gap before the first hit
  registers), starving `OperationTimedOut`'s own tick counter and
  stretching wall-clock time even though correctness wasn't broken --
  fixed by gating on `state.CurrentTargetGuid.IsEmpty()`; and (2)
  re-issue the same cast every eligible tick, interrupting itself before
  a real cast time could complete -- fixed by checking
  `bot->IsNonMeleeSpellCast(false)` first. Both confirmed via the
  regression suite going from intermittently exceeding its 40s timeout
  back to a clean `5/5`. **Use this pattern for any future
  `Tick()`-level recovery/policy check**: gate on "no active objective in
  flight," not just the policy's own safety condition, and be aware that
  `Combat::Execute`'s cast-based intents can self-interrupt without an
  in-flight-cast check.

  **Correction, later the same overnight session (`KNOWN_FAILURES.md`
  #16)**: the "fully verified live" claim above about `RecoverPet`'s
  automatic `Tick()`-level firing was imprecise -- that verification
  only ever exercised `Pets::RequestRevivePet` directly via the manual
  `revivepet` debug command, never `Recovery::PlanPetRecovery`'s own
  gate, which used `bot->HasSpell(Pets::RevivePetSpellId)` -- the wrong
  check (see below). Fixed and **re-verified for real**: `Grunthunter`'s
  pet came back alive fully automatically (no manual `revivepet` call)
  once the gate was corrected.

  **Auto-tame-if-no-pet shipped the same overnight session (ADR-041,
  `Recovery::PlanPetAcquisition`)**, and building it surfaced the
  correction above: the real Hunter pet-management spells (Tame Beast
  1515, Revive Pet 982, Call Pet 883) are **innate abilities, not
  granted through the normal trainer/spellbook system** -- `HasSpell`
  returns false for all three even on a Hunter who can genuinely cast
  them right now, confirmed live for each individually (a level-1
  Hunter with none of the three in her spellbook still got real,
  specific `SpellCastResult`s -- `SPELL_CAST_OK` for Tame Beast,
  `SPELL_FAILED_ALREADY_HAVE_SUMMON` for the other two against an
  active pet -- never an unknown-spell rejection). Had any of these
  shipped gated on `HasSpell`, that branch would have been a silent,
  permanent no-op for every Hunter, forever. All three now use
  `IsClass(CLASS_HUNTER, CLASS_CONTEXT_ABILITY)` instead -- a cheap
  pre-filter only, real validation stays with the engine's own
  `CheckCast`. **Auto-tame is live-verified fully automatically**: a
  fresh Hunter (`PetState::NoPet`) walked near a real beast and left
  idle got a real, live, newly-tamed pet with no manual `tamebeast`
  call.

  **A real architectural characteristic surfaced along the way, and
  fixed the same session (ADR-042)**: `GuideRuntime::Tick` (and
  therefore, at the time, all pet recovery/acquisition) only ran while a
  guide was actively in progress (`BotLifecycleMgr` gates on
  `!session.Guide.Finished`) -- a fully idle bot with no guide running
  got no ambient pet maintenance at all. Fixed by adding
  `GuideRuntime::TickAmbient`, called unconditionally by
  `BotLifecycleMgr::Update` for every registered bot every tick
  regardless of guide state, containing the pet-recovery/acquisition
  logic moved out of `Tick()`. **Live-verified with zero guide
  commands**: a fresh `PetState::NoPet` Hunter was simply logged in near
  a beast -- no `guidestartmoveto`, nothing -- and got a real, auto-
  tamed pet within seconds.

  **Update (2026-07-02, ADR-043): the `MissingAlive` gap is CLOSED.**
  The real mechanism `KNOWN_FAILURES.md` #17 couldn't find is the
  Dismiss Pet *spell* (2641 -- `Spell::EffectDismissPet` ->
  `pet->Remove(PET_SAVE_NOT_IN_SLOT)`), not a pet command. New
  `Pets::RequestDismissPet` + `.autonomousplayer dismisspet`; verified
  live with 40ms polling: `SPELL_CAST_OK` -> real ~5s cast ->
  directly-captured `state=MissingAlive` -> automatic `CallPet` on the
  next ambient tick -> same pet number (5988) back alive in 0.6s. Call
  Pet spell 883 thereby live-confirmed for the first time too.

  **Other honest gap remaining**: a revived pet's DB row
  isn't updated by an explicit save in that code path -- an abrupt
  (non-clean) worldserver restart shortly after a revive can lose it
  before the normal periodic autosave (900s) or a clean logout would
  have persisted it; not itself a bug, just worth knowing when testing
  across a restart. Some intermittent `guidestartcombat`
  wall-clock-timeout failures persisted even after the ADR-040 fixes --
  live-investigated and attributed to the pre-existing, already-
  documented `KNOWN_FAILURES.md` #6 (ADR-029 Engaged-phase timeout),
  confirmed unrelated to pet recovery (the pet was `MissingDead` and
  recovery correctly did not fire during the stuck `Engaged` state) --
  not a new issue, not chased further given a small sample and a
  heavily-reused test character.

**Gate 3's own literal acceptance bar (`ROADMAP.md`) is closer but still
NOT fully met — stated plainly, not glossed over:**
- "All supported race/class combos": **ALL 10 WotLK races now have
  live, clean walk+kill+loot slices (2026-07-02)** — Orc, Human,
  Tauren, Undead, Draenei (map 530, first expansion-map bot), Night
  Elf, Troll, Dwarf, Gnome, Blood Elf (map 530, ranged Auto Shot
  kill) — across 9 starting zones and 3 maps, both factions, with
  zero per-race code changes, several running concurrently across
  continents. Calibrated precisely: this makes the race-agnosticism
  claim empirical for every race, but only Orc/Human have **full
  multi-step starting-region routes** (Gates 1/2) — the other 8 have
  single kill+loot slices. Classes: Warrior/Hunter archetypes combat-
  proven; Priest still not combat-tested at a level with an offensive
  spell. Whether single slices per race satisfy "complete
  starting-region routes" for Gate 3 is the remaining judgment call
  (see NEXT TASK) — but the gap is now far narrower than the old
  "2 races" state.
- "Dense camps, caves": **covered (2026-07-02)** — a deliberately
  engineered run through the Burning Blade cave (27 Vile Familiars + 8
  Felstalkers, genuine cave terrain): 15 fully-automatic
  `guidestartcombat` cycles, 14 clean kill+loot completions, leveled
  1→2 mid-run, zero deaths; cave LoS genuinely exercised
  `IsSafeToEngage` and the bounded-blacklist path fired live for the
  first time (closing `KNOWN_FAILURES.md` #3's open remnant). One
  honest gap: `hasUnplannedAdd=true` was never organically observed
  (spawn spacing + respawn staggering; see `KNOWN_FAILURES.md` #20 and
  `TEST_MATRIX.md`'s calibrated rows).
- "Ranged pulls" as a distinct behavior: **covered (2026-07-02,
  ADR-044)** — `EngageTargetRanged` holds at the opener's real
  `SpellInfo` range with melee fallback at contact; live-verified
  twice (combat established at 20yd, bot position never changed, a
  neutral boar charged the shooter from 17yd). Honest residuals in
  ADR-044: approach-then-hold beyond max range and cast-time openers
  not yet observed.
- "Pets": **effectively complete for Hunter** — tame/status/
  react-state/combat-assist, pet revival (dead pet, loaded or not),
  auto-tame-if-no-pet, ambient maintenance for a fully idle bot, and
  (as of 2026-07-02, ADR-043) the dismiss/call-pet cycle with the
  `MissingAlive` -> `Alive` transition directly observed live — every
  `PetState` transition the 6-state model names is now live-verified
  except `Dismissed` (which by construction has no recovery path).
  Real remaining gap: Warlock/DK pet summoning is untouched.
- "Full bags": partially covered — encountered organically (a real
  near-full-bags loot outcome was observed and handled correctly by the
  existing best-effort design), not deliberately engineered.

Full per-slice history: `KNOWN_FAILURES.md` (17 Gate 3 entries),
`ARCHITECTURE.md` (ADR-008 through ADR-042), `TEST_MATRIX.md`. This file
stays a live summary, not a growing archive — older per-session numbered
lists have been condensed here rather than kept verbatim.

## What's proven, end to end, through real production code (not
## reimplemented or DB-shortcut)
**Orc Warrior** (`Grunttestbot`) and **Human Priest** (`Priestestbot`)
complete the full Gate 1/2 arc: correct racial spawn → real navmesh
movement → real quest accept/turn-in → real melee combat → real loot →
real death/recovery → real vendor buy/repair and gossip/trainer
interaction. `GuideRuntime` runs multi-step guides (`MoveTo`,
`KillNearest`, `AcceptQuest`, `TurnInQuest`) fully automatically, no
manual step advances, with real target-selection safety and bounded
failure states throughout. **Orc Hunter** (`Grunthunter`) adds a second,
ranged class-controller composition. A real automated regression suite
now protects 5 of these behaviors from silent regression.

Per the external review's own framing, and still accurate: these are
genuine, verified mechanisms, not yet a bot proven capable of safely
leveling fully unsupervised across arbitrary content (see the literal
Gate 3 gaps above).

## Files changed (cumulative, this arc)
- `docs/autonomous-player/ARCHITECTURE.md`: ADR-008 through ADR-042.
- `docs/autonomous-player/HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`: new,
  user-provided, Gate 3's combat/pulling design baseline (ADR-022).
- `docs/autonomous-player/KNOWN_FAILURES.md`, `TEST_MATRIX.md`,
  `ROADMAP.md`: updated throughout.
- `Pets/BotPets.{h,cpp}`: `RequestDismissPet` + `DismissPetSpellId`
  (ADR-043); `CallPetSpellId`'s unverified-caveat removed (now
  live-confirmed); `RequestAbandonPet`'s wrong doc comment corrected.
- Components: `Lifecycle/BotSessionMgr`, `Setup/PendingCharacterCreations`,
  `Setup/BotProvisioning` (now with `ValidateCharacterName`, ADR-032),
  `Navigation/BotNavigation`, `QuestEngine/BotQuestEngine`, `Combat/BotCombat`
  (melee + `RequestCastSpell`, real `SpellCastResult`),
  `Inventory/BotLoot`, `Recovery/BotRecovery`, `Economy/BotEconomy`,
  `Gossip/BotGossip`, `Growth/BotGrowth`, `GuideRuntime/BotGuideRuntime`
  (now with `IsSafeToEngage`, ADR-031), `EncounterModel/BotEncounterModel`
  (all `.h`/`.cpp` pairs).
- `Lifecycle/BotLifecycleMgr.{h,cpp}`: dispatches `GuideRuntime::
  TickAmbient` (unconditional, ADR-042) and `GuideRuntime::Tick`
  (gated on an active guide) per-bot per-tick.
- `Commands/cs_autonomousplayer.cpp`: ~30 debug commands, including
  `targetsafety` (ADR-031).
- `tools/live_regression_suite.py`: new (ADR-033) — the project's first
  automated test.

## Verification
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`,
  `codestyle-cpp.py`: pass on every commit.
- Compiled clean on zoidberg 65+ times across this arc (many during one
  very long overnight pets/pet-recovery session); currently deployed
  commit compiles clean.
- `tools/live_regression_suite.py`: last clean run was `5/5`; some
  individual runs this overnight session showed `4/5` or `3/5`, always
  traced to the same known, pre-existing environmental cause (creature
  population/position drift from a heavily-reused test character, or a
  real hazard-death mid-test) rather than a code regression -- see
  `KNOWN_FAILURES.md` #6/#8/#10/#14 and this file's fixture notes below.
  Re-run it fresh before trusting a specific pass/fail count at any
  given moment; position drift accumulates fast with repeated testing.
- Live testing is done via the worldserver's SOAP interface (port 7878,
  GM account `SOAPADMIN`), not manual console interaction — see
  `[[autonomous-player-zoidberg-soap-access]]` in agent memory for the
  exact mechanism and the container-recreate procedure needed to deploy
  new code (`docker restart` alone does not pick up a rebuilt image).

## Current repository state
- Branch: `mod-autonomous-player`. 2026-07-02's session commits:
  `2278fb3` (ADR-043 Dismiss Pet / `MissingAlive` closure), `fdf0a37`
  (dense camp/cave validation docs), plus the ADR-044 ranged-pulls
  commit after it — all pushed to origin, zoidberg's build checkout
  synced, live `ac-worldserver` rebuilt and redeployed with this exact
  code (twice this session, via the docker build + compose recreate
  procedure), server healthy, `live_regression_suite.py` `5/5` on the
  final deployed build.
- SOAP access note: `SOAPADMIN`'s password was reset again this session
  (previous one not recoverable, expected -- see the agent-memory
  procedure); the current credentials are stored on zoidberg in
  `~/secrets/ap_soap.env` (chmod 600) so future sessions can source
  them instead of re-deriving.
- Test fixtures on zoidberg (2026-07-02 follow-up-session updates
  first, older notes below):
  - `Grunttestbot` went **combat-inert** mid-session
    (`KNOWN_FAILURES.md` #24: cross-map guard-death + GM `.revive`;
    moved/selected fine, never swung) and was **cleared by a
    worldserver restart** -- confirmed healthy again with a clean
    kill+loot cycle post-restart. If any bot goes combat-inert:
    restart first (#23 means there is no in-place session recycle),
    root-cause second. `Petulantia` (ap_test5) remains the
    primary healthy combat fixture.
  - `game_tele` points `APBoarCluster` (boar cluster 71yd E of the VoT
    start), `APFamiliarCamp` (5.7yd Vile Familiar spawn pair),
    `APFamiliarTriple` (~8yd triple at `(-40,-4227)`) exist in the
    live world DB -- `.tele name <bot> <point>` now works on bots
    (ADR-046), making repositioning instant. They are deployment-local
    (inserted directly, ids 100001-100003), not a repo SQL change.
  - account `ap_test1` (id 204), character `Grunttestbot` (Orc Warrior,
    level 3+). ~~Currently dead and NOT trivially recoverable~~ (older
    note, superseded above -- it was recovered) -- ended
    up ~1500 yards from Valley of Trials this arc (an unrelated real
    hazard of this session's own cross-country `moveto` testing, not a
    module defect, see `KNOWN_FAILURES.md` #10's closing note) and its
    corpse is in what appears to be an environmentally hazardous spot
    near `(227.0, -3261.3, 65.7)` on map 1 -- `reclaimcorpse` resurrects
    it but it dies again immediately at the same position with zero
    movement in between (consistent with a real damage-over-time/instant-
    death hazard like lava or void at that exact spot, not a code bug --
    a real human player who died in lava would have the same problem).
    Not fixed/recovered this session -- deprioritized as a real but
    self-inflicted test-environment issue, not blocking any other work.
    Whoever needs this fixture next: try releasing spirit, then manually
    walking the *ghost* well away from that exact spot before
    `reclaimcorpse` (ghosts are typically immune to environmental
    damage), or just re-provision a fresh Orc Warrior if that fails.
  - account `ap_priest1` (id 205), character `Priestestbot` (Human
    Priest, level 1), Northshire Abbey, map 0 -- untouched this arc,
    presumed still fine.
  - account `ap_test2` (id 206), character `Grunttestii` (Orc Warrior,
    level 1) — the second Horde character ADR-032 unblocked; used for
    live ADR-031 tap/other-player-attacking verification. Alive and
    controllable as of the last check this arc; was manually walked
    toward Valley of Trials mid-session and may not have arrived --
    check `.autonomousplayer status` before assuming its position.
  - account `ap_test3` (id 207), character `Grunthunter` (Orc Hunter,
    level 2), alive at full health as of session end -- position drifts
    constantly from heavy `guidestartmoveto`/`guidestartcombat`
    regression cycling; check `.autonomousplayer status` before
    assuming it. **Its pet is currently alive again** (`PetState::
    ActiveAlive`, pet number 5914, revived automatically via the fixed
    `Recovery::PlanPetRecovery` gate during the `KNOWN_FAILURES.md` #16
    verification -- no longer left deliberately broken). Died twice more
    during this session's own `guidestartmoveto_unreachable_target_
    times_out` regression testing (real hazardous terrain, matches the
    already-documented `KNOWN_FAILURES.md` #10 travel risk), both times
    recovered via `releasespirit`/`reclaimcorpse`. This bot does NOT
    reproduce a Call Pet test case (`PetState::MissingAlive`) -- its pet
    is alive, not missing. Given how heavily this character has been
    battered across the whole session (repeated deaths, repeated pet
    death/revival cycles, level 2 throughout), consider a fresh
    character for further pet-recovery or Engaged-phase-timeout work.
  - account `ap_test4` (id 208), character `Huntonia` (Orc Hunter, level
    1), provisioned this session specifically for clean pet testing.
    **Has a real, live, manually-tamed pet** (Mottled Boar, from the
    manual `tamebeast` test that surfaced `KNOWN_FAILURES.md` #15's
    `HasSpell` bug) -- not in `PetState::NoPet` anymore, so not useful
    for a fresh auto-tame test without a real dismiss/death first.
  - account `ap_test5` (id 209), character `Petulantia` (Orc Hunter,
    **level 2 now** -- leveled during the cave run). Used across the
    2026-07-01 session to verify auto-tame (with and without a guide
    running) and `abandonpet`'s permanent-delete behavior; then on
    2026-07-02 as the fixture for ADR-043 (two full dismiss ->
    `MissingAlive` -> automatic Call Pet cycles), the dense-cave run
    (~18 kills), ADR-044's ranged pulls, and the regression suite
    (replacing the dead `Grunttestbot`). Last seen at the boar fields
    `(-460, -4290, 49)` map 1, full health. **Currently has a real, live
    pet** (pet number 5988, Mottled Boar) -- not `PetState::NoPet`; for
    a fresh auto-tame test use `.autonomousplayer abandonpet` first
    (real delete, confirmed working), and for a `MissingAlive` fixture
    use `.autonomousplayer dismisspet` (recoverable, but note ambient
    recovery will call the pet back within ~1s unless the bot is in
    combat or mid-pursuit).
  - account `ap_test6` (id 210), character `Taurtestbot` (Tauren
    Warrior, level 1), provisioned 2026-07-02 for race breadth. Alive,
    full health, near the Red Cloud Mesa plainstrider fields
    (~`(-2955, -349, 55)` map 1). Two clean kill+loot cycles done;
    good clean melee-warrior fixture for future Mulgore work.
  - accounts `ap_test7`..`ap_test13` (ids 211-217), characters
    `Deathtestbot` (Undead, Deathknell map 0), `Draeneitest` (Draenei,
    Ammen Vale map 530 -- first expansion-map bot), `Trolltestbot`
    (Valley of Trials), `Belftestbot` (Blood Elf Hunter, Sunstrider
    Isle map 530 -- has a real ranged weapon, used for the second
    ADR-044 ranged-kill observation), `Dwarftestbot` + `Gnometestbot`
    (Coldridge map 0), `Nelftestbot` (Night Elf, Shadowglen) -- all
    Warriors except the Blood Elf Hunter, all level 1, all provisioned
    2026-07-02, all alive at full health after one clean kill+loot
    cycle each. Note: a worldserver container recreate resets any
    unsaved bot position to its last save (several bots snapped back
    to their spawn points mid-testing this session -- re-walk before
    re-running a positional test after any deploy).
  - **Provisioning note**: race/class ids matter -- `race=2` is Orc
    (not `race=1`, which is Human and produced a real, correctly-
    rejected "invalid race/class pair" error when combined with
    `class=3` Hunter this session). `class=3` is Hunter, confirmed
    correct throughout. The first `provision` attempt for a brand new
    account sometimes fails transiently ("Failed to create/find
    account") -- matches an already-documented Gate 2 finding; just
    retry once.
- Unrelated dirty files in the local working tree (idlebot/dashboard
  project, pre-existing) are unchanged.

## Known failures
21 Gate 3 entries in `KNOWN_FAILURES.md` (plus 6 in Gate 1, several
non-bug findings in Gate 2). #21 is 2026-07-02's real M2-LoS bug
(`IsSafeToEngage` stricter than the engine's own combat LoS, starving
target selection zone-wide on doodad-dense terrain -- found by the
all-races run, FIXED with `ModelIgnoreFlags::M2`, live-verified) and
carries a diagnosability recommendation (per-rejection-reason counters
for `Selecting`) worth doing. Open, non-blocking: #6 (ADR-029 timeout —
re-tested with a 13-trial sample, not reproduced, downgraded to
low-priority), #14 (user directly reported `Grunthunter`
underground/Z-clipping; the suspected mechanism was deliberately
reproduced twice and did NOT clip — real root cause still unknown,
honestly left open, NOT claimed fixed). **#3's last open remnant (the
bounded-blacklist path never exercised live) closed 2026-07-02** — it
fired for real in the cave run (`blacklisted=1`, retarget, engage).
**#8, #10, #12, #13, #15, #16, #17, #18, and #19 are all
FIXED/RESOLVED and live-verified** — #17 was closed 2026-07-02
(ADR-043): the missing `MissingAlive` mechanism was the Dismiss Pet
*spell* (2641), found by searching the effect table instead of the
pet-command vocabulary. #20 records the cave run's three non-bug
findings (whole-step budget exhaustion in a killed-out area;
`EncounterModel` counts bot-attackers only; `multipull`'s
multi-attacker limitation).

**#13, #16, and #19 are all worth reading regardless of being
"closed"**: #13 records two wrong theories (a stale `GetPetGUID()`,
then a wrongly-concluded "structural fork limitation") before the real
fix; #16 found that `Player::HasSpell` is the wrong gate for all three
Hunter pet-management spells (Tame Beast/Revive Pet/Call Pet -- none
are granted through the normal spellbook system) and **corrects an
over-confident "fully verified live" claim this same file made about
`RecoverPet`'s automatic firing in ADR-040** -- that earlier
verification only ever exercised the raw primitive via a manual debug
command, never the actual policy gate, which had the exact same bug
and would have silently never fired for any Hunter. #16 also surfaced
(and this same session, fixed -- ADR-042) a real architectural gap: pet
recovery/acquisition used to only fire while a guide was actively being
ticked (`GuideRuntime::Tick` doesn't run for a bot with no guide in
progress) -- `GuideRuntime::TickAmbient` now runs unconditionally for
every registered bot every tick, live-verified with zero guide commands
issued. **#18/#19 are two real bugs `TickAmbient` itself introduced,
both caught same-session via self-review/final testing rather than left
for a future session to find**: #18 was a narrow one-tick race where a
same-tick guide-step dispatch could interrupt a cast `TickAmbient` just
started; #19 was more serious -- a dead bot with a `MissingDead` pet
kept re-attempting a doomed, instantly-failing revive cast every tick
forever, which (combined with #18's own fix) permanently starved
`GuideRuntime::Tick()` and everything depending on it, including every
bounded-wait guarantee this project has built since ADR-028. Fixed both
at the source (`bot->IsAlive()` checks) and with a deliberately generic
systemic backstop (`BotSession::ConsecutiveAmbientSkips`, forces
`Tick()` to run after 10 consecutive ambient-only ticks regardless of
cause) against any other not-yet-found persistent-failure mode having
the same effect. #11 is a non-bug (`COMBAT_TOO_HARD` observed for real,
working as designed).

## Decisions made
- User's standing direction: "continue on your own until we get to gate
  5" / "continue on your own" / "continue on your own until gate 3 is
  completed and validated" (repeated, most recently 2026-07-01).
  Interpreted as: keep working autonomously, bounded-increment
  discipline, only pausing for a genuine blocker or a decision only the
  user can make.
- User explicitly confirmed (2026-07-01, `AskUserQuestion`) Gate 2's
  race-coverage bar is a representative sample (2 races/classes). This
  arc extends the same reasoning to Gate 3's race breadth (not user-
  reconfirmed for Gate 3 specifically — flagged as a reinterpretation,
  not a literal pass, in `ROADMAP.md`'s Week 4 entry).
- **When a fix doesn't hold up on re-test, don't declare success and
  don't blindly patch repeatedly without new evidence** — established
  with `KillNearest`'s stuck-target bug (`KNOWN_FAILURES.md` #3) and
  reaffirmed repeatedly since (the engagement-confirmation fix, the
  `IsHostileTo` catch, the ADR-029 re-test).
- **User provided `HONORBUDDY_SINGULAR_COMBAT_RESEARCH.md`** — Gate 3's
  design baseline for combat/pulling work (ADR-022).
- **An external code review (2026-07-01)** gave a 7-point priority list
  that became this project's operative NEXT TASK for a full arc; all 7
  now have real progress (see "Current milestone" above) — the review is
  closed out, but Gate 3's own literal bar is a separate, still-open
  thing (see above).

## 2026-07-02 follow-up session (same day, autonomous continuation)

Worked the NEXT TASK list top-down with live evidence for everything;
one new commit (`290ab7e`) built and deployed to zoidberg:

- **Engineered full bags: DONE** (the Gate 3 literal-bar delta (b)) --
  controlled A/B on `Petulantia` at the boar cluster: not-full run
  `lastLootVerified=true`; verifiably-100%-full run (real
  `CanStoreNewItem` refused even 1 more item, checked twice)
  `finished=true, failed=false, lastLootAttempted=true,
  lastLootVerified=false`, no hang, bot unharmed. ADR-030's contract,
  now deliberately engineered, not just organic.
- **`SelectionDiagnostics` (ADR-045): DONE** -- #21's diagnosability
  recommendation implemented; `guidestatus` now prints a per-reason
  `selection:` breakdown for the latest sweep. Root-caused two real
  anomalies its first hour live (a tamed player pet sharing the
  objective's creature entry read as `notAttackable=1`; cave terrain
  read `noLos=3`).
- **Teleport-ack synthesis (ADR-046, `KNOWN_FAILURES.md` #22): found
  AND fixed same session** -- server-initiated teleports of a bot
  silently never completed (no client to ack). Both paths
  live-verified post-deploy. Test-fixture repositioning is now
  instant via `game_tele` points `APBoarCluster` / `APFamiliarCamp` /
  `APFamiliarTriple` (added to the deployment's world DB directly, not
  a repo SQL change).
- **Two new open findings, documented honestly**: `.kick` of a bot is
  a silent no-op -- no runtime bot-logout mechanism exists at all
  (#23, since FIXED next session: `.autonomousplayer logout`);
  `Grunttestbot` is combat-inert after a cross-map guard-death
  + GM `.revive` -- moves/selects/confirms fine, never swings, control
  bot unaffected, not root-caused (#24).
- **#20 (organic `hasUnplannedAdd`): still open after a real attempt**
  -- assist confirmed enabled in config (radius 10/delay 2000ms), 17
  cycles anchored on 5.7yd and ~8yd spawn clusters (one after a full
  220s respawn) polling the correct guide-relative signal: zero
  organic adds. Also recorded #20's methodology trap:
  `encountersnapshot` passes an empty objective, so ANY attacker reads
  `hasUnplannedAdd=true` there -- an early "observation" this session
  was retracted as exactly that artifact.
- **#24 probe result**: a worldserver restart cleared the combat-inert
  wedge (clean kill+loot cycle post-restart) -- in-memory session
  state, root cause within it still unidentified, reproduction chain
  documented in #24.

**Same session, final arc (ADR-048): the first complete multi-kill
quest loops.** `RepeatUntilQuestComplete` grind steps +
`guidestartquestgrind` + idempotent accept/turn-in (= re-issuable
routes). Live-verified for BOTH quest archetypes on two more races:
collection quest 747 (Tauren) accept->grind->turn-in->REWARDED in one
run; kill-credit quest 788 (Troll) REWARDED across one bounded failure
plus one resumed route. Three real bugs found and fixed on the way
(`KNOWN_FAILURES.md` #26: quest drops were NEVER looted -- per-player
quest loot slots never requested, would have blocked every collection
objective ever; #26 second half: `LastLootVerified` could never read
true for any corpse with a real drop; #27: the first repeat gate used
`CanCompleteQuest`, which goes false at the exact moment of success).
Suite 5/5 on the final build. **This materially advances judgment
call (a)**: full multi-step routes now exist and complete on Tauren
and Troll, not just Orc/Human -- what remains of (a) is only how many
more quests per race the user wants, not whether the machinery works.
New tele points: `APCampNarache`, `APDenKaltunk` (ids 100004-5).

**Follow-up session (2026-07-02, ADR-048 widening): Undead + Draenei
full quest CHAINS, entry-gate fix, and the first OPEN bag-management
failure.** Two more races ran real quest content, this time as
two-quest chains, not single quests: `Deathtestbot` (Undead, map 0)
quest 364 The Mindless Ones (dual kill objective, composed as one
route issue per kill entry -- see #28's authoring lesson) -> REWARDED,
then 3901 Rattling the Rattlecages -> REWARDED (after #29's wedge: one
bag slot freed manually, the only manual intervention in all four
quests); `Draeneitest` (Draenei, map
530) quest 10302 Volatile Mutations -> REWARDED (turn-in auto-accepted
the follow-up) -> 9293 What Must Be Done... (collection, 10x 100%-drop
lasher samples) -> REWARDED. One code change this session, live-forced
by both races' geometry: the repeat-grind gate is now ALSO checked on
`Selecting` entry (#28, deployed 905a2fa), which is what makes
"re-issue the route with the waypoint at the giver" actually work when
the grind field lies beyond the 150yd turn-in radius -- verified live
by 9293's resume. One OPEN failure discovered (#29): choice-reward
turn-ins are silently refused forever with full bags (3901 wedged at
distance 0.0 from Sarvis, 8/8, twice); bounded and diagnosed, needs
real bag management (vendoring) as the durable Gate 3 fix. Judgment
call (a) evidence now: full routes REWARDED on Tauren, Troll, Undead,
Draenei + Orc/Human -- 4 races beyond the originals, both factions,
both maps, both quest archetypes, plus chains.

**Same follow-up session: #29 closed same-day** with the economy
slice above (NEXT TASK item 7), suite 5/5 on the deployed build
(94725ff; one 4/5 flake first was the documented slow-fight case --
the very bot it flaked on had 13 grays/0 free slots and became the
second SellJunk verification). Also this session: all Claude
co-author trailers stripped from the branch history at the user's
direction (16 commits rewritten, trees verified byte-identical,
force-pushed; commit hashes cited in these docs updated to the
rewritten ones).

## NEXT TASK
Gate 3's external-review debt is paid off, all safety/tooling bugs found
this arc are fixed and re-verified, and pets now has a real,
`GuideRuntime`-integrated slice with a genuinely-verified decision layer
(`Combat::CombatIntent`/`Combat::Execute`, `Recovery::PlanPetRecovery`,
`Recovery::PlanPetAcquisition`, `GuideRuntime::TickAmbient`, ADR-037
through ADR-042). Pet revival (dead pet, loaded or not) and pet
acquisition (no pet at all) are both confirmed working **fully
automatically** live -- no manual debug command in the loop for either,
`HasSpell` correctly is not used anywhere in this component anymore
(see `KNOWN_FAILURES.md` #15/#16 for why), and both now fire even for a
fully idle bot with zero guide commands issued (ADR-042). In rough
priority order:

1. ~~Find a real mechanism to construct `PetState::MissingAlive`~~
   **DONE (2026-07-02, ADR-043)** -- the mechanism is the real Dismiss
   Pet spell (2641); the full `MissingAlive` -> `Alive` chain was
   directly observed live, closing `KNOWN_FAILURES.md` #17.
2. ~~Dense camps / caves~~ **DONE (2026-07-02)** -- the Burning Blade
   cave NE of Valley of Trials (27 Vile Familiars around
   `(-178, -4329, 65)` map 1 + 8 Felstalkers, found via a
   spawn-cluster GROUP BY over `creature`): 15 fully-automatic kill
   cycles through camp + cave interior, LoS-safety and the blacklist
   path exercised on real terrain, zero deaths. Honest residual:
   `hasUnplannedAdd=true` never organically observed (see
   `KNOWN_FAILURES.md` #20) -- if a future session wants it, the
   reliable construction is probably a real assist-call (fight one
   familiar within ~10yd of a live same-faction ally), verified
   co-spawned first via fresh respawn timing (200s in this camp).
3. ~~Ranged pulls as a distinct behavior~~ **DONE (2026-07-02,
   ADR-044)** — `Combat::RequestAttackRanged`/`EngageTargetRanged`,
   mode decided from real `SpellInfo` range data, melee fallback at
   contact; live-verified twice. ~~Residuals: approach-then-hold
   unobserved; cast-time openers untested.~~ **Both residuals CLOSED
   (2026-07-02 follow-up session)**: approach-then-hold observed twice
   (48.3yd and 42.4yd starts, hold at range); the first real cast-time
   opener (Warlock Shadow Bolt) hit the predicted self-interrupt bug
   — found, fixed (`IsNonMeleeSpellCast(false,false,true)` guard,
   Auto Shot unchanged), and A/B live-verified (`KNOWN_FAILURES.md`
   #25). This was also the fourth class archetype, zero
   class-specific code.
4. ~~`KillNearest`'s bounded-blacklist path~~ **DONE (2026-07-02)** —
   fired live in the cave run (`blacklisted=1` on a
   LoS-flickering patroller, retarget, engage; `KNOWN_FAILURES.md` #3
   closed fully).
5. ~~Warlock demon summoning, first probe~~ **PROBED (2026-07-02,
   follow-up session): summoning itself works with ZERO new module
   code.** `Warlocktest` (Orc Warlock, ap_test14, provisioned at level
   1) cast Summon Imp (688) via the existing
   `Combat::RequestCastSpell` primitive -> `SPELL_CAST_OK`, real Imp
   pet (entry 416) alive -- the exact ADR-037 composition result
   taming had. Three calibrated residuals, all real: (a) spell 688 is
   NOT spellbook-tracked yet casts fine -- the `HasSpell`-is-invalid
   finding (#15/#16) now spans a second class, treat it as the norm
   for pet-management spells; (b) the imp stays `reactState=0`
   (passive) -- ambient pet maintenance (ADR-042) is Hunter-gated, so
   Warlock pets get no auto-correction/recovery at all yet, live-
   confirmed; (c) probing this surfaced and fixed a real second half
   of `KNOWN_FAILURES.md` #12 (self-cast spells re-triggered the
   SPELL_FAILED_MOVING loop -- range-0 spells never need the approach
   walk, commit `f3072ac`). **Later the same session: ambient demon
   maintenance landed too (ADR-047)** -- `PlanDemonMaintenance`
   re-summons a dead/dismissed/absent demon fully automatically,
   live-verified twice (pet gone ~10s while the ambient-issued Summon
   Imp cast completed, same pet identity back, zero commands after the
   dismiss). Remaining real scope: higher-level summons, soul shards,
   a Warlock combat-guide slice exercising `EnsurePetAssists` with a
   demon.
6. **`Grunthunter`'s underground/Z-clipping report** (`KNOWN_FAILURES.md`
   #14) — real, user-reported, investigated, NOT root-caused. Would need
   either the user's own in-game observation at the exact moment it
   recurs, or deeper terrain-inspection tooling this project doesn't
   have yet.
7. ~~Bag management, minimum viable: vendor gray items~~ **DONE
   (2026-07-02, same day #29 was found)** — `StepType::SellJunk` +
   `guidestartselljunk`, live-verified on two bots/continents (grays
   -> 0, real money received, idempotent re-issue no-op), plus
   `guidestatus` now shows `turnInEngineRefused`/`grayItems`/
   `freeBagSlots`. See #29's entry for the full evidence and residuals
   (`turnInEngineRefused=true` never yet observed live; SellJunk is
   composed by route authors, not auto-inserted).

(Ambient pet maintenance for a fully idle bot, `KNOWN_FAILURES.md` #16's
remaining point, is now FIXED and live-verified with zero guide
commands -- `GuideRuntime::TickAmbient`, ADR-042 -- dropped from this
list.)

**Real, important calibration note for whoever picks this up**: before
trusting `Player::HasSpell` as a gate for ANY Hunter pet-management
spell in this fork, check it live first the way this session finally
did (cast it via a debug command against a character whose spellbook
doesn't list it, read the real `SpellCastResult`) -- `HasSpell` was
wrong for all three of Tame Beast/Revive Pet/Call Pet, and it's
plausible other class abilities in this fork have the same
"innate, not spellbook-tracked" characteristic. Don't assume the normal
gated-ability pattern (which IS correct for trainer-taught spells
elsewhere in this project, e.g. Priest/Warrior offensive abilities)
applies uniformly.

**Architecture note for whoever picks this up:** combat/pet decisions
now go through `Combat::CombatIntent`/`Combat::Execute` rather than
`GuideRuntime` calling low-level primitives directly -- extend that
pattern for new behaviors (a new `IntentKind` + a case in
`Combat::Execute`) rather than adding another isolated call site.
Recovery-style "should this even run right now" decisions (like pet
revival/acquisition) belong in a `Recovery::Plan*` policy function
returning `std::optional<CombatIntent>`. As of ADR-042, there are two
real call sites for this pattern, and picking the right one matters: a
decision that should run **only while a specific guide step is
dispatching** (rare -- most decisions aren't actually step-specific)
belongs inside `GuideRuntime::Tick` before the step dispatches; a
decision that's genuinely **background bot maintenance, independent of
whether any guide is running at all** (pet recovery/acquisition are
the only examples so far) belongs in `GuideRuntime::TickAmbient`,
called unconditionally by `BotLifecycleMgr::Update` regardless of guide
state. Don't inline either kind directly into a step's own tick
function.

**Calibration note for whoever picks this up:** this arc's own
`IsHostileTo`→`IsValidAttackTarget` catch and the `FindNearestCreature`
alive-param footgun are good examples of why testing a diagnostic
command against real live state (not just reading the code) keeps
finding real bugs even in code that "looks right." Keep doing that
before declaring anything "Verified." Also: **run
`tools/live_regression_suite.py` before starting any new work and after
every change** — it's cheap, real, and would have caught this arc's own
regressions automatically instead of requiring a human/agent to notice.

## Next-session acceptance criteria
- Real progress on pets or dense-camp/cave validation — whichever is
  picked, with real live evidence, not just code review.
- `check_no_playerbots_dependency.sh`, `check_no_forbidden_apis.sh`, and
  `tools/live_regression_suite.py` all still pass.
- Docs updated with calibrated claims, committed.

## Recommended next-session prompt
Read docs/autonomous-player/{PROJECT,ROADMAP,ARCHITECTURE,HANDOFF,
KNOWN_FAILURES,TEST_MATRIX,HONORBUDDY_SINGULAR_COMBAT_RESEARCH}.md in
full, especially this file's "NEXT TASK" section and `KNOWN_FAILURES.md`
#13/#15/#16/#17 (a real, in-order chain of wrong theories before real
fixes, and one real gap -- `MissingAlive` -- that resisted every
approach tried; genuinely instructive about verifying the actual code
path something runs instead of the first plausible-looking result).
Also check agent memory for `autonomous-player-zoidberg-soap-access`
before doing any live testing -- it has the exact SOAP mechanism and
the container-recreate procedure needed to actually deploy new code
(`docker restart` alone does not pick up a rebuilt image).

Gate 3's external-review debt is fully paid off, all safety/tooling
bugs found across this whole arc are fixed and re-verified, and pets is
now substantially complete: tame/revive/call-pet primitives, the full
6-state `PetState` model, `Recovery::PlanPetRecovery`/
`PlanPetAcquisition` policies, and `GuideRuntime::TickAmbient` (ADR-037
through ADR-042) together mean pet revival (dead, loaded or not) and
pet acquisition (no pet at all) both fire **fully automatically, even
for a fully idle bot with zero guide commands issued** -- reproduced
live multiple times each. **`Player::HasSpell` is NOT a valid gate for
any Hunter pet-management spell in this fork** (Tame Beast/Revive Pet/
Call Pet are all innate, not spellbook-tracked) -- use
`IsClass(CLASS_HUNTER, CLASS_CONTEXT_ABILITY)` instead, and verify any
NEW gated-ability assumption live before trusting it, the same way this
arc caught three real instances of this exact bug.

**Top priority**: a Gate 3 completion assessment against `ROADMAP.md`'s
literal bar. As of the 2026-07-02 follow-up session (see this file's
session block above), everything on that bar with an autonomous path
is CLOSED with live evidence -- pets (ADR-037..043, plus Warlock
ADR-047), dense camps/caves, ranged pulls including both ADR-044
residuals (#25), the blacklist path, no-manual-step-advances, all 10
races with live kill+loot slices, engineered full bags, training
primitives, and a first automated regression suite (5/5 on the
currently-deployed build). **Only the two user judgment calls
remain**: (a) do single per-race slices satisfy "complete
starting-region routes," or do the other 8 races need Orc/Human-style
full multi-step routes? (b) does "guide validation" need more than
the current guides + regression suite? A question to this effect was
asked 2026-07-02 and timed out with the user away -- re-ask it. If
confirmed, Gate 3 is declarable and Gate 4 (levels 1-20: regional
travel, class growth, flights, transports, restart recovery) opens.
Real open items after that call: `KNOWN_FAILURES.md` #14
(underground/Z-clipping, needs the user's in-game observation), #20
(organic unplanned add, parked after 17 rigorous cycles), #24
(combat-inert session wedge -- restart clears it, root cause
unidentified; `.autonomousplayer logout` now exists as a
lighter-than-restart recycle tool to try on the next occurrence),
higher-level Warlock content (Voidwalker, soul shards), and a
Warlock/caster full class-controller beyond the single-opener slice.
(#23 runtime bot logout: FIXED 2026-07-02, live-verified
login->logout->relogin cycle -- see the entry.)

**Run `tools/live_regression_suite.py` before starting and after any
change that touches `GuideRuntime`/`Combat`/`Setup`/`Pets`/`Recovery`**
to catch regressions automatically -- this arc found real bugs this way
repeatedly. Re-derive the bot's current position and a genuinely nearby
creature entry fresh each time rather than reusing one from earlier in
the session -- heavy `guidestartmoveto`/`guidestartcombat` cycling
causes real, fast position drift, and an intermittent `4/5` or `3/5`
result is very often just that, not a regression (check `guidestatus`
and the bot's actual position before concluding otherwise). Design
briefly, implement the smallest testable increment, compile-check and
live-verify on zoidberg with real evidence (build-and-deploy is
pre-approved), update docs with calibrated (not overstated) claims,
commit. Keep going without stopping to check in, except for a genuine
blocker or an ambiguous decision only the user can make.
