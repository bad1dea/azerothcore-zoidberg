#ifndef MOD_IDLEBOT_LOG_H
#define MOD_IDLEBOT_LOG_H

#include <string>
#include <unordered_map>
#include <fstream>

namespace idlebot
{
    // IdleBotLog
    // -------------------------------------------------------------------------
    // Per-bot file logger: one "<botName>.log" per bot, capturing idlebot's own
    // activity (lifecycle, decisions, executor actions in later milestones) plus,
    // optionally, the bot's in-world chat (say/yell/emote/whisper/party/channel).
    //
    // World-thread only — both callers (IdleBotManager and the chat PlayerScript)
    // run on the world thread, so no locking is needed.
    //
    // TODO(perf): writes are synchronous appends with a per-line flush (kept for
    // tail -f freshness; bot chatter/actions are low volume). If volume grows,
    // move to AzerothCore's async AppenderFile / an IoContext strand.
    class IdleBotLog
    {
    public:
        static IdleBotLog* instance();

        void Initialize();   // read config, resolve + create the log directory
        void Shutdown();     // flush + close all open streams

        bool Enabled() const { return _enabled; }
        bool CaptureChat() const { return _enabled && _captureChat; }

        // Append "[YYYY-MM-DD HH:MM:SS] [tag] message" to <botName>.log.
        // No-op when disabled or if the file cannot be opened.
        void Write(std::string const& botName, std::string const& tag, std::string const& message);

    private:
        IdleBotLog() = default;

        std::ofstream* StreamFor(std::string const& botName);  // lazy-open; nullptr on failure
        static std::string SanitizeFileName(std::string const& name);

        bool _enabled = false;
        bool _captureChat = true;
        std::string _dir;   // resolved directory, with trailing '/'
        std::unordered_map<std::string, std::ofstream> _streams;
    };
}

#define sIdleBotLog idlebot::IdleBotLog::instance()

#endif // MOD_IDLEBOT_LOG_H
