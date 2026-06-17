#include "IdleBotLog.h"
#include "Configuration/Config.h"
#include "Log.h"

#include <filesystem>
#include <ctime>

namespace idlebot
{
    IdleBotLog* IdleBotLog::instance()
    {
        static IdleBotLog mgr;
        return &mgr;
    }

    void IdleBotLog::Initialize()
    {
        _enabled     = sConfigMgr->GetOption<bool>("IdleBot.Log.PerBot.Enabled", true);
        _captureChat = sConfigMgr->GetOption<bool>("IdleBot.Log.CaptureChat", true);

        // Empty config => "<LogsDir>/idlebot". Otherwise use the configured path
        // verbatim (e.g. a host-bind-mounted dir so logs are visible outside the
        // container).
        std::string dir = sConfigMgr->GetOption<std::string>("IdleBot.Log.Directory", "");
        if (dir.empty())
            dir = sLog->GetLogsDir() + "idlebot";

        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
            dir.push_back('/');
        _dir = dir;

        if (!_enabled)
        {
            LOG_INFO("module.idlebot", "[IdleBot] per-bot logging disabled by config.");
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(_dir, ec);
        if (ec)
        {
            LOG_ERROR("module.idlebot", "[IdleBot] could not create log dir '{}': {}. Per-bot logging off.",
                _dir, ec.message());
            _enabled = false;
            return;
        }

        LOG_INFO("module.idlebot", "[IdleBot] per-bot logs at '{}' (captureChat={}).",
            _dir, _captureChat);
    }

    void IdleBotLog::Shutdown()
    {
        _streams.clear();   // ofstream dtor flushes + closes
    }

    std::string IdleBotLog::SanitizeFileName(std::string const& name)
    {
        std::string out;
        out.reserve(name.size());
        for (char c : name)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_')
                out.push_back(c);
            else
                out.push_back('_');
        }
        if (out.empty())
            out = "_";
        return out;
    }

    std::ofstream* IdleBotLog::StreamFor(std::string const& botName)
    {
        auto it = _streams.find(botName);
        if (it != _streams.end())
            return &it->second;

        std::string path = _dir + SanitizeFileName(botName) + ".log";
        std::ofstream file(path, std::ios::out | std::ios::app);
        if (!file.is_open())
        {
            LOG_ERROR("module.idlebot", "[IdleBot] failed to open bot log '{}'.", path);
            return nullptr;
        }

        auto [pos, ok] = _streams.emplace(botName, std::move(file));
        return ok ? &pos->second : nullptr;
    }

    void IdleBotLog::Write(std::string const& botName, std::string const& tag, std::string const& message)
    {
        if (!_enabled)
            return;

        std::ofstream* out = StreamFor(botName);
        if (!out)
            return;

        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

        (*out) << '[' << ts << "] [" << tag << "] " << message << '\n';
        out->flush();
    }

    std::vector<std::string> IdleBotLog::Tail(std::string const& botName, uint32_t lines) const
    {
        std::vector<std::string> result;
        if (!_enabled || lines == 0)
            return result;

        std::string path = _dir + SanitizeFileName(botName) + ".log";
        std::ifstream in(path);
        if (!in.is_open())
            return result;

        // Small per-bot logs: read all lines, keep a trailing window of `lines`.
        std::vector<std::string> all;
        std::string line;
        while (std::getline(in, line))
            all.push_back(line);

        std::size_t start = (all.size() > lines) ? all.size() - lines : 0;
        for (std::size_t i = start; i < all.size(); ++i)
            result.push_back(all[i]);
        return result;
    }
}
