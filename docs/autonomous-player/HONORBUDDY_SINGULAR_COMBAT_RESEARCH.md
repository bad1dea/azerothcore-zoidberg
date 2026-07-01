# Honorbuddy/Singular Combat Research and Clean-Room Design

Status: research baseline for Gate 3 and later. This document describes
observable behavior and a new AzerothCore-native design. It is not an import
plan and contains no copied implementation.

## Legal and technical boundary

The reviewed public repositories do not expose a usable license in their
repository metadata or root trees. Therefore, do **not** copy, translate, or
derive code from them. Treat them as behavioral references only, then implement
against AzerothCore APIs and independently written tests.

This is also the better technical boundary. Honorbuddy routines operated from
a client-facing object model and issued client-like actions. Our module runs
inside AzerothCore and has different lifecycle, movement, threat, spell, and
session APIs. Literal copying would preserve assumptions that do not hold here.

Reviewed sources (snapshots inspected 2026-07-01):

- [Bossland Singular](https://github.com/BosslandGmbH/Singular), commit
  `3ad0a11a7f81b9b26ba753092b6a754936ce3723` (Legion-era architecture).
- [Bossland Honorbuddy Quest Behaviors](https://github.com/BosslandGmbH/Honorbuddy-Quest-Behaviors),
  commit `4627b8d31d341ef3d896bf62045b34f90f4c06f5`.
- [Likon69 Singular-wotlk](https://github.com/Likon69/Singular-wotlk), commit
  `5ee163211250af6442b76fa1b587bbb0a754f151` (WotLK behavioral cross-check,
  not an authoritative implementation).
- Historical local note `mod-idlebot/docs/HONORBUDDY_QB_IMPORT_PLAN.md`, read
  from Git history only. It covered quest-behavior semantics, not a complete
  combat engine.

## What Singular actually contributed

Singular was not primarily a quest planner. Honorbuddy or its quest behavior
selected a kill target/point of interest; Singular validated that target,
approached it, opened combat, and ran the class behavior. This division is
important:

1. The guide/objective planner decides *what progress is wanted*.
2. The engagement planner decides *whether and how to pull it safely*.
3. The combat controller decides *what ability to use now*.
4. The executor performs and verifies the authoritative game action.
5. Immediate threats may temporarily override the guide-selected target.

Its behavior phases were broadly: death, rest, pre-combat buffs, pull buffs,
pull, heal, combat buffs, combat, and loss-of-control. Class/spec/context
behaviors were composed by priority, while shared managers handled movement,
pets, talents, healing, tanking, and spell immunity. The useful idea is phase
separation and central action validation—not its C# behavior-tree syntax.

## Pulling behavior worth preserving

A pull is a transaction, not `move toward nearest creature`:

```text
Select -> Validate -> AssessRisk -> PlanApproach -> Approach -> Prepare
       -> Open -> ConfirmEngagement -> Stabilize -> Combat
       -> Finish -> Loot -> Recover
```

Each transition must have an observable success condition, deadline, and
failure classification.

### Select and validate

A candidate must be alive, attackable, relevant to the current objective,
reachable, not temporarily blacklisted, and not already tagged/engaged by an
unrelated player. Validation repeats during approach because all of these facts
can change.

Honorbuddy's `KillUntilComplete` additionally modeled hunting grounds,
waypoints, blackspots, pursuit entries, respawn waiting, and over-grind
prevention. Those belong in objective search, not inside a class rotation.

### Assess risk before committing

Build a prospective encounter set from the target, the route to the target,
nearby social hostiles, patrols, casters, elites, and likely respawns. Score at
least:

- enemy count, level delta, elite/boss rank, current health and mana;
- melee/ranged/caster mix, crowd-control capability, pet availability;
- distance between social neighbors and the planned approach corridor;
- nearby neutral/hostile units likely to be body-pulled;
- escape-path availability and recent deaths at this location.

The default leveling policy is one desired target and zero desired adds.
Singular's optional “Pull More” throughput behavior is not an appropriate
default. It was guarded by health, maximum mob count, level, elite, player-PvP,
quest relevance, and timeout checks. We should implement it only after safe
single pulls work across classes, and keep it disabled by default.

### Approach and open

The plan selects a pull position, desired combat range, line of sight, facing,
and opener. Re-evaluate continuously; stop movement at the correct range.
Casters need line-of-sight acquisition, melee needs a navigable contact point,
and hunters need dead-zone handling.

The current project's live finding is decisive: a bare `MoveChase(target)` did
not produce movement in the failing `KillNearest` path. The real attack request,
which establishes the combat relationship and then chases, did. Therefore an
approach cannot be considered active merely because a movement order was
issued. An opener is an action with authoritative acknowledgement.

### Confirm engagement

Distance reduction is not success. Confirmation should use authoritative
signals such as attack state, combat references, victim/threat ownership,
successful spell result, damage event, or tag ownership. If no confirmation is
received before the deadline, cancel, classify the cause, and temporarily
blacklist the candidate or approach point.

## Aggro and target management

Maintain two targets:

- **Objective target:** selected for quest or grind progress.
- **Combat target:** the unit requiring the next combat action.

The combat target may override the objective target when a unit is already
attacking the player/pet, the objective target is unreachable/out of line of
sight, a caster must be interrupted, or an add is a more immediate lethal
threat. When stable, return to the objective target.

The encounter snapshot should contain:

- all units attacking the player or owned pet;
- each unit's victim, distance, line of sight, cast, level/rank and health;
- threat/taunt state where AzerothCore exposes it through normal mechanics;
- crowd-control and immunity state;
- objective relevance and tag/loot eligibility;
- units close enough to join due to movement or social aggro.

Pet threat counts as the bot's encounter. A pet tanking a mob is not “no
aggro.” Conversely, a mob fighting another player is not ours and should not be
stolen merely because it is nearby.

Target priority should be explainable rather than a hidden nearest-unit rule:

```text
lethal/controlling caster > loose add attacking self > interruptible caster
> current engaged target > loose add attacking pet > objective target
```

Class policy may adjust this order. Tanks prioritize units with inadequate
threat; healers prioritize survival while preserving their current attacker
model. Solo leveling never needs a group-tank abstraction to choose its basic
target.

## Combat decision order

Every class controller should return an intent from a common priority model:

1. Loss-of-control escape or encounter abort.
2. Emergency heal, immunity, defensive cooldown, potion, or flee decision.
3. Interrupt a dangerous cast.
4. Taunt, threat drop, crowd control, or add stabilization.
5. Maintain essential form, stance, aura, seal, pet, weapon enchant, or buff.
6. Maintain high-value target debuffs/damage-over-time effects.
7. Use safe AoE only when the encounter model permits it.
8. Execute the normal single-target priority.
9. Reposition for range, facing, line of sight, dead zone, or kiting.

The executor—not each class script—must enforce known spell, cooldown, global
cooldown, resources, range, facing, line of sight, movement restrictions,
target validity, and duplicate-aura prevention. Controllers express intent;
the shared executor decides whether the action can legally start and reports
why it could not.

## AzerothCore-native component design

### `EncounterModel`

Produces an immutable per-tick snapshot of attackers, likely adds, casts,
threat relationships, pet state, escape paths, and risk. It consumes only
normal server mechanics needed to simulate a player; policy must not use hidden
future spawn information or teleport-like shortcuts.

### `EngagementPlanner`

Creates an `EngagementPlan` containing target GUID, approach/pull point,
opener, desired range band, permitted add count, risk score, deadlines, and
abort conditions. It owns the pull state machine and temporary blacklists.

### `CombatController`

One implementation per class, optionally split by spec after level 10. Given
the character capability snapshot and encounter snapshot, it returns a ranked
list of `CombatIntent`s. It must not call `Attack`, cast, move, or mutate threat
directly.

### `CombatExecutor`

Validates and starts one intent using AzerothCore's authoritative APIs. It
tracks start/acknowledgement/completion/failure and exposes structured failure
reasons. Movement and attack are coordinated here so a “chase” cannot be
mistaken for a successful pull.

### `RecoveryPolicy`

Decides whether to continue, kite, flee, use a consumable, reset, eat/drink,
resurrect a pet, or recover after death. Rest ends immediately on hostile
engagement and should combine food/drink with efficient self-healing.

### `AbilityCatalog`

Derives currently usable abilities from the actual spellbook, form/stance,
equipment, level, talents, and AzerothCore spell data. Never assume a spell
solely because the class normally learns it at a given level.

## WotLK class coverage hypotheses

The following are implementation targets to validate against AzerothCore spell
data and live tests. They are not rotations to copy.

| Class | Pull/positioning | Early survival and control | Special state |
|---|---|---|---|
| Warrior | Charge when legal; ranged weapon fallback; close to melee | Victory Rush, defensive stance/tools, interrupt, Hamstring/flee handling | rage, stance, weapon requirements |
| Paladin | Judgement/ranged opener then melee | self-heal, bubble/defensive, stun, cleanse | seal, aura, mana |
| Hunter | ranged opener, maintain range, dead-zone recovery | pet tanking, Mend Pet, traps, disengage/kite when learned | ammo, pet alive/happy, aspect |
| Rogue | stealth approach when safe, melee opener | kick, evasion, gouge/stun, vanish/escape policy | energy, combo points, poison |
| Priest | ranged spell opener and hold casting range | shield without Weakened Soul, heal thresholds, fear/add control, Fade | mana, wand fallback, shadowform later |
| Shaman | Lightning Bolt/ranged opener or melee plan | heal, interrupt, slows, defensive totems | mana, weapon imbue, shield, totems |
| Mage | ranged opener, LoS and kiting | Frost Nova, Polymorph one add, Counterspell, barrier | mana, conjured food/water, armor |
| Warlock | DoT/ranged opener with pet engagement | Drain Life, Fear policy, healthstone, threat drop | mana/health conversion, shard, demon |
| Druid | spell pull or form-specific approach | heal before/after form, roots, Bash/interrupt, travel escape | mana plus rage/energy, form, stealth |
| Death Knight | ranged disease/Death Grip where safe, then melee | interrupt, self-heal, defensive cooldowns, chains/kite | runes, runic power, presence |

Each class needs level-band behavior because the available toolkit changes
substantially: 1–5, 6–9, 10–19, 20–39, 40–59, and 60–80. A controller passes
only when it works with the character's actual learned abilities, including
missing trainers, imperfect gear, and empty consumable slots.

## AoE, crowd control, and fleeing

AoE requires an explicit safety query. Before casting, estimate all affected
units and reject the cast if it would hit neutral, crowd-controlled, tagged, or
otherwise unwanted targets. Cone and ground-targeted spells also require
orientation/placement validation.

Crowd control is encounter state, not just another damaging spell. Track its
expected break conditions and prevent AoE/target switching from immediately
breaking it. Re-plan when it expires early or the target is immune.

Fleeing mobs require pursuit risk assessment. Do not blindly follow a low-health
mob into another pack. Use a slow, root, stun, ranged execute, or abandon/reset
when the route risk exceeds the current plan.

## Failure handling and observability

Temporary blacklists must be scoped by reason, target, location, and expiry.
Useful categories include unreachable, no line of sight, opener rejected, no
engagement acknowledgement, evade, immune, tagged by other player, unsafe pack,
and repeated death. A target blacklist must not silently become a permanent
quest failure; the guide runtime needs a distinct “all candidates exhausted”
result with hunting-ground/respawn behavior.

Emit one structured event per state transition and action result with:

- bot, class/spec/level, objective and combat target GUID/entry;
- pull state, risk score and risk contributors;
- position, path distance, range band, facing and line-of-sight result;
- attackers/add candidates and their victims;
- selected intent and every rejected higher-priority intent with reason;
- action start, acknowledgement, completion/failure, elapsed time;
- blacklist creation/expiry and recovery decision.

This is required to avoid repeating the recent pattern where plausible movement
fixes were declared before proving that the target distance changed.

## Gate 3 implementation sequence

1. Replace `KillNearest`'s implicit flow with the explicit pull state machine,
   retaining the verified real attack-request path.
2. Add `EncounterModel` and structured pull diagnostics before adding more
   class rotations.
3. Implement conservative single-pull Warrior and Priest controllers through
   level 12 using actual learned spell snapshots.
4. Prove add override, caster line of sight, fleeing target, unsafe pack reject,
   opener timeout, and temporary blacklist behavior live.
5. Add pet ownership/threat and Hunter/Warlock controllers.
6. Add the remaining classes by level band, with one mechanics test and one
   dense-camp test per class.
7. Enable safe AoE and crowd control only after affected-target prediction is
   tested. Keep proactive multi-pull disabled.

## Required live regression scenarios

- Target is reachable but outside aggro range; opener causes real approach.
- Target is unreachable or separated by bad line of sight; alternate point or
  timed blacklist is selected without an infinite chase.
- One add attacks during approach; combat target changes and later returns.
- Two nearby mobs would body-pull; planner rejects or selects a safer angle.
- Caster target holds range and reacquires line of sight without oscillation.
- Hunter enters dead zone; pet/range policy recovers.
- Pet is missing, dead, or loses threat.
- Bot starts low health/mana and refuses an unsafe pull.
- Spell is interrupted, immune, on cooldown, or lacks resources.
- Mob flees toward a pack; pursuit is stopped or controlled.
- Mob evades/resets; encounter ends and receives a bounded blacklist.
- Death and corpse recovery do not preserve stale target/engagement state.

Acceptance is measured by state transitions and authoritative outcomes, not by
the absence of errors in logs. No Gate 3 combat claim is complete while a test
needs a manual target selection, step advance, teleport, or database repair.
