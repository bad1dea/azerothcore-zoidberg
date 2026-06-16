#ifndef MOD_IDLEBOT_GUIDELOADER_H
#define MOD_IDLEBOT_GUIDELOADER_H

#include "IdleBotGuide.h"
#include <string>
#include <unordered_map>
#include <optional>

namespace idlebot
{
    // Loads guide YAML files from IdleBot.GuideDirectory into memory.
    // Source of truth for IDs is the AzerothCore world DB — the loader may later
    // cross-check questId/npcId/creatureIds against the DB and warn on mismatch.
    class IdleBotGuideLoader
    {
    public:
        // Recursively load all *.yaml under directory. Returns count loaded.
        // Parse/validation errors are logged per-file and skipped, not fatal.
        size_t LoadDirectory(const std::string& directory);

        std::optional<Guide> Get(const std::string& guideId) const;
        std::vector<std::string> ListIds() const;

        // Validate without loading into the active set (for `.idlebot guide validate`).
        bool ValidateFile(const std::string& path, std::string& outErr) const;

    private:
        // TODO(M3): implement with yaml-cpp. Map StepType strings -> enum.
        std::unordered_map<std::string, Guide> _guides;
    };
}

#endif // MOD_IDLEBOT_GUIDELOADER_H
