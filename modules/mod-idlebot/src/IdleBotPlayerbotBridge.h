#ifndef MOD_IDLEBOT_PLAYERBOTBRIDGE_H
#define MOD_IDLEBOT_PLAYERBOTBRIDGE_H

#include <string>
#include <vector>
#include <cstdint>

// IdleBotPlayerbotBridge
// =============================================================================
// THE central abstraction. Everything above (manager, executor, decision engine)
// depends on THIS interface, never on mod-playerbots internals directly.
//
// Prototype implementation drives bots via mod-playerbots CHAT COMMANDS.
// A later implementation can issue direct internal calls. Neither change should
// require touching code above this seam.
//
// TODO(verify): These types are placeholders. Replace with AzerothCore's real
// types once confirmed in local source:
//   - ObjectGuid for guids (grep AzerothCore for ObjectGuid)
//   - Position / coordinates
// Using primitive stand-ins here so the interface compiles standalone before
// wiring; swap them as you integrate.

namespace idlebot
{
    using BotGuid    = uint64_t;   // TODO(verify): replace with ObjectGuid
    using PlayerGuid = uint64_t;   // TODO(verify): replace with ObjectGuid

    struct BotPosition
    {
        uint32_t mapId = 0;
        float x = 0.f, y = 0.f, z = 0.f, o = 0.f;
        bool valid = false;
    };

    struct InventoryStatus
    {
        uint32_t freeSlots = 0;
        uint32_t totalSlots = 0;
        uint32_t lowestDurabilityPct = 100;   // across equipped items (100 = pristine/none)
        bool needsRepair = false;             // any equipped item broken (0 durability)
        bool valid = false;
    };

    enum class QuestState
    {
        Unknown,
        NotStarted,
        InProgress,
        Complete,        // objectives done, not yet turned in
        Rewarded,        // turned in
        Failed
    };

    // Adaptive-combat context (smart engagement modes). Reads playerbots' own
    // combat values so idlebot decides WHAT mode to use, not the rotation.
    struct CombatContext
    {
        bool ranged = false;            // bot is a ranged class/spec (kite/stand-off)
        uint32_t possibleTargets = 0;   // attackable hostiles in sight (EXCLUDES tapped)
        uint32_t myAttackers = 0;       // mobs currently attacking the bot (pull size)
        uint32_t aoeCount = 0;          // size of the densest nearby mob cluster
        float hpPct = 100.f;
        float manaPct = 100.f;          // 100 for classes without mana
        uint32_t currentTargetEntry = 0;
        float currentTargetDistance = 0.f;
        std::string currentTargetName;
        bool inCombat = false;
        bool valid = false;
    };

    // Live snapshot used by `.idlebot status`. Populated from the online Player.
    struct BotLiveStatus
    {
        bool online = false;        // character is logged in / in world
        bool controlled = false;    // session is a playerbot (under bot control)
        uint32_t level = 0;
        uint32_t health = 0, maxHealth = 0;
        uint32_t mana = 0, maxMana = 0;
        uint32_t questCount = 0;     // active quests in the log
        BotPosition pos;
    };

    struct LootAttempt
    {
        bool acted = false;             // playerbots action/move/open was issued
        bool hasLoot = false;           // a lootable corpse/object is still visible
        bool inRange = false;           // current loot target is within interact range
        uint32_t lootableCorpses = 0;   // nearby corpses core says this bot can loot
        uint32_t corpseEntry = 0;       // entry of the nearest corpse we acted on
        uint64_t corpseGuid = 0;        // guid of the nearest corpse we acted on
        bool queuedMoney = false;       // direct loot opcode queued money this tick
        uint32_t queuedItemId = 0;       // direct loot opcode queued this item this tick
        uint32_t queuedItemCount = 0;
        uint8_t queuedSlot = 0;
        std::string queuedItemName;
        std::string debug;              // compact description for IdleBot.Debug.Enabled
    };

    // Abstract interface. The manager holds an IdleBotPlayerbotBridge*.
    class IdleBotPlayerbotBridge
    {
    public:
        virtual ~IdleBotPlayerbotBridge() = default;

        // --- lifecycle ---
        // Ensure the named bot is logged in / spawned and controllable.
        // Returns false if it could not be brought online.
        virtual bool EnsureBotOnline(const std::string& botName) = 0;

        // Release control: log the bot out / hand it back. Returns false if it
        // was not online or could not be released.
        virtual bool ReleaseBot(const std::string& botName) = 0;

        // Resolve a bot name to its guid (raw value; 0 if the character is
        // unknown). Note: resolves the character, not its online state.
        virtual BotGuid GetBotGuid(const std::string& botName) = 0;

        // Live snapshot for status display. Returns false if not resolvable.
        virtual bool GetLiveStatus(BotGuid bot, BotLiveStatus& out) = 0;

        // --- generic playerbots seam ---
        // Run a named playerbots action (DoSpecificAction) silently. This is the
        // stable surface through which idlebot triggers playerbot behaviour
        // (release, revive, repair, loot, maintenance, ...). Returns whatever the
        // action reports (true = ran/ok). All action-name coupling stays here.
        virtual bool DoBotAction(BotGuid bot, std::string const& actionName) = 0;

        // Toggle a playerbots strategy expression (e.g. "+grind", "-follow").
        virtual bool SetNonCombatStrategy(BotGuid bot, std::string const& strategyExpr) = 0;
        virtual bool SetCombatStrategy(BotGuid bot, std::string const& strategyExpr) = 0;

        // --- movement ---
        virtual bool MoveTo(BotGuid bot, uint32_t mapId, float x, float y, float z, float radius) = 0;
        virtual bool FollowPlayer(BotGuid bot, PlayerGuid player) = 0;

        // --- combat ---
        virtual bool AttackCreature(BotGuid bot, uint64_t creatureGuid) = 0;
        virtual bool CastSpell(BotGuid bot, uint32_t spellId, uint64_t targetGuid) = 0;

        // --- interaction / quests ---
        virtual bool InteractWithNpc(BotGuid bot, uint64_t npcGuid) = 0;
        virtual bool AcceptQuest(BotGuid bot, uint32_t questId, uint64_t npcGuid) = 0;
        virtual bool TurnInQuest(BotGuid bot, uint32_t questId, uint64_t npcGuid) = 0;
        virtual QuestState GetQuestStatus(BotGuid bot, uint32_t questId) = 0;

        // Nearest world-object lookups (raw guid value; 0 if none in range).
        virtual uint64_t FindNearestCreatureEntry(BotGuid bot, uint32_t entry, float radius) = 0;
        // Nearest ALIVE creature among `entries` inside the step area; writes its
        // position to `out` and returns true if one was found. Used by KillMobs to
        // home onto actual quest mobs instead of grinding whatever is nearest.
        virtual bool FindNearestQuestCreature(BotGuid bot, std::vector<uint32_t> const& entries, BotPosition const& center, float radius, BotPosition& out, uint64_t& outGuid) = 0;
        virtual uint64_t FindNearestGameObjectEntry(BotGuid bot, uint32_t entry, float radius) = 0;
        // True if the bot is within interaction range of a gameobject of `entry`.
        virtual bool IsNearGameObject(BotGuid bot, uint32_t entry, float radius) = 0;
        // Use the gameobject (right-click). Private-server-direct GameObject::Use.
        // Returns false if not in range / not found.
        virtual bool UseGameObject(BotGuid bot, uint32_t entry, float radius) = 0;

        // --- maintenance (routed through playerbots actions) ---
        virtual LootAttempt LootNearby(BotGuid bot) = 0;     // "loot" + target/move/open diagnostics
        virtual bool VendorTrash(BotGuid bot) = 0;    // "sell"
        virtual bool Repair(BotGuid bot) = 0;         // "repair" (needs repair NPC in range)
        virtual bool Train(BotGuid bot) = 0;          // "trainer" (needs trainer in range)
        virtual bool Maintenance(BotGuid bot) = 0;    // "maintenance" (learn/repair/restock)

        // True while the bot is in combat (used to hold position so it can loot).
        virtual bool IsInCombat(BotGuid bot) = 0;

        // --- adaptive combat (smart engagement modes) ---
        // Populate the combat context from playerbots' own combat values. False if
        // the bot is offline / has no AI.
        virtual bool GetCombatContext(BotGuid bot, CombatContext& out) = 0;
        // Eat/drink to restore health/mana (playerbots food/drink actions).
        virtual bool Recover(BotGuid bot) = 0;

        // --- death / recovery (routed through playerbots dead-state actions) ---
        virtual bool IsGhost(BotGuid bot) = 0;
        virtual bool RequestReleaseSpirit(BotGuid bot) = 0;       // "release"
        virtual bool RequestReviveFromCorpse(BotGuid bot) = 0;    // "revive from corpse"
        virtual bool RequestSpiritHealerRevive(BotGuid bot) = 0;  // "spirit healer"
        // Private-server convenience fallback: direct core resurrect. Returns
        // false if not applicable (not dead/ghost). Caller must gate on config.
        virtual bool DirectResurrect(BotGuid bot) = 0;

        // --- state reads ---
        virtual InventoryStatus GetInventoryStatus(BotGuid bot) = 0;
        virtual BotPosition GetPosition(BotGuid bot) = 0;
        virtual bool IsDead(BotGuid bot) = 0;
        virtual bool ReviveOrCorpseRun(BotGuid bot) = 0;
        virtual uint32_t GetLevel(BotGuid bot) = 0;
        virtual uint32_t GetItemCount(BotGuid bot, uint32_t itemId, bool inBankAlso) = 0;
        virtual bool GetQuestObjectiveProgress(BotGuid bot, uint32_t questId, uint8_t objectiveIndex, uint32_t& outCurrent, uint32_t& outRequired) = 0;
        // XP progress within the current level (for summary display). 0/0 if offline.
        virtual void GetXp(BotGuid bot, uint32_t& outXp, uint32_t& outXpForNextLevel) = 0;
        // Carried money in copper (real-time loot signal for debug logging). 0 if offline.
        virtual uint32_t GetMoney(BotGuid bot) = 0;

        // --- nearby world (decision engine / social; later milestones) ---
        virtual std::vector<PlayerGuid> GetNearbyPlayers(BotGuid bot, float radius) = 0;
        virtual std::vector<uint64_t> GetNearbyCreatures(BotGuid bot, float radius) = 0;
        virtual std::vector<uint64_t> GetNearbyGameObjects(BotGuid bot, float radius) = 0;

        // --- social (M8+; bridge exposes, policy lives in IdleBotSocial) ---
        virtual bool InvitePlayer(BotGuid bot, PlayerGuid player) = 0;
        virtual bool AcceptGroupInvite(BotGuid bot, PlayerGuid inviter) = 0;
        virtual bool LeaveGroup(BotGuid bot) = 0;
    };

    // Factory: returns the configured implementation (chat vs internal).
    // TODO: implement in IdleBotPlayerbotBridge.cpp once mod-playerbots command
    // syntax is confirmed.
    IdleBotPlayerbotBridge* CreateBridge(const std::string& controlMode);
}

#endif // MOD_IDLEBOT_PLAYERBOTBRIDGE_H
