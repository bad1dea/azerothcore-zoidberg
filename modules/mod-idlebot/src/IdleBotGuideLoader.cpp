#include "IdleBotGuideLoader.h"
#include "SharedDefines.h"
#include "Log.h"
#include <fkYAML/node.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <system_error>

namespace idlebot
{
    namespace
    {
        using YamlNode = fkyaml::node;

        std::optional<YamlNode> GetNode(YamlNode const& node, char const* key)
        {
            if (!node.is_mapping() || !node.contains(key))
                return std::nullopt;
            return node[key];
        }

        std::optional<std::string> GetString(YamlNode const& node, char const* key)
        {
            auto child = GetNode(node, key);
            if (!child || !child->is_string())
                return std::nullopt;
            return child->get_value<std::string>();
        }

        std::optional<uint32_t> GetUInt(YamlNode const& node, char const* key)
        {
            auto child = GetNode(node, key);
            if (!child)
                return std::nullopt;

            try
            {
                if (child->is_integer())
                    return static_cast<uint32_t>(child->get_value<int64_t>());
                if (child->is_string())
                    return static_cast<uint32_t>(std::stoul(child->get_value<std::string>()));
            }
            catch (...)
            {
            }

            return std::nullopt;
        }

        std::optional<float> GetFloat(YamlNode const& node, char const* key)
        {
            auto child = GetNode(node, key);
            if (!child)
                return std::nullopt;

            try
            {
                if (child->is_float_number())
                    return child->get_value<float>();
                if (child->is_integer())
                    return static_cast<float>(child->get_value<int64_t>());
                if (child->is_string())
                    return std::stof(child->get_value<std::string>());
            }
            catch (...)
            {
            }

            return std::nullopt;
        }

        std::optional<bool> GetBool(YamlNode const& node, char const* key)
        {
            auto child = GetNode(node, key);
            if (!child)
                return std::nullopt;

            try
            {
                if (child->is_boolean())
                    return child->get_value<bool>();
                if (child->is_string())
                {
                    std::string value = child->get_value<std::string>();
                    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (value == "true" || value == "1" || value == "yes")
                        return true;
                    if (value == "false" || value == "0" || value == "no")
                        return false;
                }
            }
            catch (...)
            {
            }

            return std::nullopt;
        }

        std::vector<std::string> GetStringArray(YamlNode const& node, char const* key)
        {
            std::vector<std::string> values;
            auto child = GetNode(node, key);
            if (!child)
                return values;

            if (child->is_string())
            {
                values.push_back(child->get_value<std::string>());
                return values;
            }

            if (!child->is_sequence())
                return values;

            for (auto const& item : child->as_seq())
            {
                if (item.is_string())
                    values.push_back(item.get_value<std::string>());
            }

            return values;
        }

        StepType ParseStepType(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (value == "move_to") return StepType::MoveTo;
            if (value == "accept_quest") return StepType::AcceptQuest;
            if (value == "turn_in_quest") return StepType::TurnInQuest;
            if (value == "kill_mobs") return StepType::KillMobs;
            if (value == "loot_items") return StepType::LootItems;
            if (value == "interact_gameobject") return StepType::InteractGameobject;
            if (value == "collect_items") return StepType::CollectItems;
            if (value == "use_item_on_npc") return StepType::UseItemOnNpc;
            if (value == "talk_to_npc") return StepType::TalkToNpc;
            if (value == "train_class_skills" || value == "train_class") return StepType::TrainClassSkills;
            if (value == "vendor") return StepType::Vendor;
            if (value == "repair") return StepType::Repair;
            if (value == "equip_upgrade") return StepType::EquipUpgrade;
            if (value == "set_hearthstone") return StepType::SetHearthstone;
            if (value == "use_hearthstone") return StepType::UseHearthstone;
            if (value == "grind_until_level") return StepType::GrindUntilLevel;
            if (value == "discover_flight_path") return StepType::DiscoverFlightPath;
            if (value == "escort_quest" || value == "escort") return StepType::EscortQuest;
            if (value == "taxi_ride" || value == "fly_to") return StepType::TaxiRide;
            if (value == "gossip_interact" || value == "gossip") return StepType::GossipInteract;
            if (value == "use_item_at_location" || value == "use_item") return StepType::UseItemAtLocation;
            if (value == "conditional") return StepType::Conditional;
            if (value == "checkpoint") return StepType::Checkpoint;
            if (value == "fallback") return StepType::Fallback;

            return StepType::Unknown;
        }

        uint32_t RaceMaskFromName(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (value == "human") return 1u << (RACE_HUMAN - 1);
            if (value == "orc") return 1u << (RACE_ORC - 1);
            if (value == "dwarf") return 1u << (RACE_DWARF - 1);
            if (value == "night elf") return 1u << (RACE_NIGHTELF - 1);
            if (value == "undead") return 1u << (RACE_UNDEAD_PLAYER - 1);
            if (value == "tauren") return 1u << (RACE_TAUREN - 1);
            if (value == "gnome") return 1u << (RACE_GNOME - 1);
            if (value == "troll") return 1u << (RACE_TROLL - 1);
            if (value == "blood elf") return 1u << (RACE_BLOODELF - 1);
            if (value == "draenei") return 1u << (RACE_DRAENEI - 1);
            return 0;
        }

        uint32_t ClassMaskFromName(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (value == "warrior") return 1u << (CLASS_WARRIOR - 1);
            if (value == "paladin") return 1u << (CLASS_PALADIN - 1);
            if (value == "hunter") return 1u << (CLASS_HUNTER - 1);
            if (value == "rogue") return 1u << (CLASS_ROGUE - 1);
            if (value == "priest") return 1u << (CLASS_PRIEST - 1);
            if (value == "death knight") return 1u << (CLASS_DEATH_KNIGHT - 1);
            if (value == "shaman") return 1u << (CLASS_SHAMAN - 1);
            if (value == "mage") return 1u << (CLASS_MAGE - 1);
            if (value == "warlock") return 1u << (CLASS_WARLOCK - 1);
            if (value == "druid") return 1u << (CLASS_DRUID - 1);
            return 0;
        }

        uint32_t FactionMaskFromName(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (value == "alliance")
                return 0x1u;
            if (value == "horde")
                return 0x2u;
            return 0;
        }

        uint32_t BuildMask(std::vector<std::string> const& values, uint32_t (*mapper)(std::string))
        {
            uint32_t mask = 0;
            for (std::string const& value : values)
                mask |= mapper(value);
            return mask;
        }

        void ParseCoordinates(GuideStep& step, YamlNode const& stepNode)
        {
            if (auto mapId = GetUInt(stepNode, "map_id"))
                step.coords.mapId = *mapId;

            std::optional<YamlNode> container = GetNode(stepNode, "coordinates");
            if (!container)
                container = GetNode(stepNode, "position");
            if (!container || !container->is_mapping())
                return;

            if (auto mapId = GetUInt(*container, "map_id"))
                step.coords.mapId = *mapId;
            else if (auto mapId = GetUInt(*container, "map"))
                step.coords.mapId = *mapId;

            if (auto value = GetFloat(*container, "x"))
                step.coords.x = *value;
            if (auto value = GetFloat(*container, "y"))
                step.coords.y = *value;
            if (auto value = GetFloat(*container, "z"))
                step.coords.z = *value;
            if (auto value = GetFloat(*container, "radius"))
                step.coords.radius = *value;
            if (auto value = GetBool(*container, "todo"))
                step.coords.isTodoPlaceholder = *value;
        }

        void ParseHotspots(GuideStep& step, YamlNode const& stepNode)
        {
            auto hsNode = GetNode(stepNode, "hotspots");
            if (!hsNode || !hsNode->is_sequence())
                return;

            for (auto const& hs : hsNode->as_seq())
            {
                if (!hs.is_mapping())
                    continue;
                Coordinates c;
                c.mapId = step.coords.mapId;
                if (auto v = GetFloat(hs, "x")) c.x = *v;
                if (auto v = GetFloat(hs, "y")) c.y = *v;
                if (auto v = GetFloat(hs, "z")) c.z = *v;
                if (auto v = GetFloat(hs, "radius")) c.radius = *v;
                else c.radius = step.coords.radius;
                step.hotspots.push_back(c);
            }
        }

        void ParseAdaptive(GuideStep& step, YamlNode const& stepNode)
        {
            auto adaptiveNode = GetNode(stepNode, "adaptive");
            if (!adaptiveNode || !adaptiveNode->is_mapping())
                return;

            if (auto value = GetBool(*adaptiveNode, "optional"))
                step.adaptive.optional = *value;
            if (auto value = GetBool(*adaptiveNode, "skippable"))
                step.adaptive.skippable = *value;
            if (auto value = GetBool(*adaptiveNode, "required_for_chain"))
                step.adaptive.requiredForChain = *value;
            if (auto value = GetUInt(*adaptiveNode, "max_attempt_minutes"))
                step.adaptive.maxAttemptMinutes = *value;
            if (auto value = GetUInt(*adaptiveNode, "max_deaths"))
                step.adaptive.maxDeaths = *value;
            if (auto value = GetBool(*adaptiveNode, "allow_alternate_areas"))
                step.adaptive.allowAlternateAreas = *value;
            if (auto value = GetBool(*adaptiveNode, "allow_grouping"))
                step.adaptive.allowGrouping = *value;
            if (auto value = GetBool(*adaptiveNode, "allow_grind_fallback"))
                step.adaptive.allowGrindFallback = *value;
            step.adaptive.fallbackSteps = GetStringArray(*adaptiveNode, "fallback_steps");
        }

        void ParseRestrictions(GuideStep& step, YamlNode const& stepNode)
        {
            auto restrictionsNode = GetNode(stepNode, "restrictions");
            if (!restrictionsNode || !restrictionsNode->is_mapping())
                return;

            uint32_t const raceMask = BuildMask(GetStringArray(*restrictionsNode, "races"), RaceMaskFromName);
            if (raceMask != 0)
                step.raceMask = raceMask;

            uint32_t classMask = BuildMask(GetStringArray(*restrictionsNode, "classes"), ClassMaskFromName);
            if (classMask == 0)
                if (auto v = GetUInt(*restrictionsNode, "class_mask"))
                    classMask = *v;
            if (classMask != 0)
                step.classMask = classMask;

            if (auto faction = GetString(*restrictionsNode, "faction"))
            {
                uint32_t const factionMask = FactionMaskFromName(*faction);
                if (factionMask != 0)
                    step.factionMask = factionMask;
            }
        }

        bool ParseStep(YamlNode const& stepNode, GuideStep& outStep, std::string& outErr)
        {
            auto type = GetString(stepNode, "type");
            if (!type)
            {
                outErr = "step missing type";
                return false;
            }

            outStep.type = ParseStepType(*type);
            if (outStep.type == StepType::Unknown)
            {
                outErr = "step has unknown type '" + *type + "'";
                return false;
            }

            outStep.id = GetString(stepNode, "id").value_or("");
            outStep.name = GetString(stepNode, "name").value_or(outStep.id);
            outStep.levelMin = GetUInt(stepNode, "level_min").value_or(0);
            outStep.levelMax = GetUInt(stepNode, "level_max").value_or(0);
            outStep.questId = GetUInt(stepNode, "quest_id");
            outStep.npcId = GetUInt(stepNode, "npc_id");
            outStep.gameobjectId = GetUInt(stepNode, "gameobject_id");
            outStep.itemId = GetUInt(stepNode, "item_id");
            outStep.gossipOption = GetUInt(stepNode, "gossip_option");
            outStep.taxiNodeId = GetUInt(stepNode, "taxi_node_id");
            outStep.timeoutSeconds = GetUInt(stepNode, "timeout_seconds").value_or(0);
            outStep.retryCount = GetUInt(stepNode, "retry_count").value_or(0);
            outStep.notes = GetString(stepNode, "notes").value_or("");
            outStep.completionCondition =
                GetString(stepNode, "completion_condition")
                    .value_or(GetString(stepNode, "completionCondition").value_or(""));

            if (auto creatureIdsNode = GetNode(stepNode, "creature_ids"); creatureIdsNode && creatureIdsNode->is_sequence())
            {
                for (auto const& creatureNode : creatureIdsNode->as_seq())
                {
                    if (creatureNode.is_integer())
                        outStep.creatureIds.push_back(static_cast<uint32_t>(creatureNode.get_value<int64_t>()));
                }
            }
            else if (auto creatureId = GetUInt(stepNode, "creature_id"))
                outStep.creatureIds.push_back(*creatureId);

            // collect_items sources
            if (auto goEntriesNode = GetNode(stepNode, "source_gameobject_entries"); goEntriesNode && goEntriesNode->is_sequence())
            {
                for (auto const& e : goEntriesNode->as_seq())
                    if (e.is_integer()) outStep.sourceGameobjectEntries.push_back(static_cast<uint32_t>(e.get_value<int64_t>()));
            }
            if (auto crEntriesNode = GetNode(stepNode, "source_creature_entries"); crEntriesNode && crEntriesNode->is_sequence())
            {
                for (auto const& e : crEntriesNode->as_seq())
                    if (e.is_integer()) outStep.sourceCreatureEntries.push_back(static_cast<uint32_t>(e.get_value<int64_t>()));
            }
            if (auto v = GetUInt(stepNode, "item_count")) outStep.itemCount = *v;

            ParseCoordinates(outStep, stepNode);
            ParseHotspots(outStep, stepNode);

            // Parse embedded vendor coords (from generate_vendor_coords.py).
            if (auto vendorNode = GetNode(stepNode, "nearest_vendor"); vendorNode && vendorNode->is_mapping())
            {
                outStep.vendorEntry = GetUInt(*vendorNode, "entry");
                if (auto vx = GetFloat(*vendorNode, "x")) outStep.vendorCoords.x = *vx;
                if (auto vy = GetFloat(*vendorNode, "y")) outStep.vendorCoords.y = *vy;
                if (auto vz = GetFloat(*vendorNode, "z")) outStep.vendorCoords.z = *vz;
                outStep.vendorCoords.mapId = outStep.coords.mapId;
            }
            ParseAdaptive(outStep, stepNode);
            ParseRestrictions(outStep, stepNode);
            return true;
        }

        bool ParseGuide(YamlNode const& root, Guide& outGuide, std::string& outErr)
        {
            outGuide.id = GetString(root, "id").value_or("");
            outGuide.name = GetString(root, "name").value_or("");
            outGuide.faction = GetString(root, "faction").value_or("");
            outGuide.race = GetString(root, "race").value_or("");
            outGuide.klass = GetString(root, "class").value_or(GetString(root, "klass").value_or(""));
            outGuide.levelMin = GetUInt(root, "level_min").value_or(1);
            outGuide.levelMax = GetUInt(root, "level_max").value_or(outGuide.levelMin);
            outGuide.nextGuide = GetString(root, "next_guide").value_or("");

            if (outGuide.race.empty())
            {
                std::vector<std::string> const races = GetStringArray(root, "races");
                if (races.size() == 1)
                    outGuide.race = races.front();
            }

            if (outGuide.klass.empty())
            {
                std::vector<std::string> const classes = GetStringArray(root, "classes");
                if (classes.size() == 1)
                    outGuide.klass = classes.front();
            }

            auto stepsNode = GetNode(root, "steps");
            if (!stepsNode || !stepsNode->is_sequence())
            {
                outErr = "guide missing steps array";
                return false;
            }

            outGuide.steps.clear();
            outGuide.steps.reserve(stepsNode->size());
            for (auto const& stepNode : stepsNode->as_seq())
            {
                GuideStep step;
                if (!ParseStep(stepNode, step, outErr))
                    return false;
                outGuide.steps.push_back(std::move(step));
            }

            return outGuide.Valid(outErr);
        }
    }

    bool Guide::Valid(std::string& outErr) const
    {
        if (id.empty())
        {
            outErr = "guide missing id";
            return false;
        }
        if (name.empty())
        {
            outErr = "guide '" + id + "' missing name";
            return false;
        }
        if (steps.empty())
        {
            outErr = "guide '" + id + "' has no steps";
            return false;
        }

        for (std::size_t i = 0; i < steps.size(); ++i)
        {
            GuideStep const& step = steps[i];
            if (step.type == StepType::Unknown)
            {
                outErr = "guide '" + id + "' step " + std::to_string(i + 1) + " has unknown type";
                return false;
            }
            if (step.id.empty())
            {
                outErr = "guide '" + id + "' step " + std::to_string(i + 1) + " missing id";
                return false;
            }
            if (step.name.empty())
            {
                outErr = "guide '" + id + "' step " + std::to_string(i + 1) + " missing name";
                return false;
            }
        }

        outErr.clear();
        return true;
    }

    size_t IdleBotGuideLoader::LoadDirectory(const std::string& directory)
    {
        _guides.clear();

        std::filesystem::path const rootPath(directory);
        std::error_code ec;
        if (!std::filesystem::exists(rootPath, ec) || !std::filesystem::is_directory(rootPath, ec))
            return 0;

        size_t loaded = 0;
        for (std::filesystem::recursive_directory_iterator it(rootPath, ec), end; it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (!it->is_regular_file())
                continue;

            std::string const ext = it->path().extension().string();
            if (ext != ".yaml" && ext != ".yml")
                continue;

            std::string err;
            FILE* file = std::fopen(it->path().string().c_str(), "r");
            if (!file)
            {
                LOG_WARN("module.idlebot", "[IdleBot] guide loader: failed to open '{}'.", it->path().string());
                continue;
            }

            try
            {
                YamlNode const root = YamlNode::deserialize(file);
                std::fclose(file);

                Guide guide;
                if (!ParseGuide(root, guide, err))
                {
                    LOG_WARN("module.idlebot", "[IdleBot] guide loader: invalid guide '{}': {}.", it->path().string(), err);
                    continue;
                }

                _guides[guide.id] = std::move(guide);
                ++loaded;
            }
            catch (std::exception const& ex)
            {
                std::fclose(file);
                LOG_WARN("module.idlebot", "[IdleBot] guide loader: parse failed for '{}': {}.", it->path().string(), ex.what());
            }
        }

        return loaded;
    }

    std::optional<Guide> IdleBotGuideLoader::Get(const std::string& guideId) const
    {
        auto it = _guides.find(guideId);
        if (it == _guides.end())
            return std::nullopt;
        return it->second;
    }

    std::vector<std::string> IdleBotGuideLoader::ListIds() const
    {
        std::vector<std::string> ids;
        ids.reserve(_guides.size());
        for (auto const& [id, _] : _guides)
            ids.push_back(id);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    bool IdleBotGuideLoader::ValidateFile(const std::string& path, std::string& outErr) const
    {
        FILE* file = std::fopen(path.c_str(), "r");
        if (!file)
        {
            outErr = "failed to open guide file";
            return false;
        }

        try
        {
            YamlNode const root = YamlNode::deserialize(file);
            std::fclose(file);

            Guide guide;
            return ParseGuide(root, guide, outErr);
        }
        catch (std::exception const& ex)
        {
            std::fclose(file);
            outErr = ex.what();
            return false;
        }
    }
}
