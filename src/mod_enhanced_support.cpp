/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "EnhancedSupport.h"
#include "BanMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TaskScheduler.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <unordered_map>

// Optional integration: relay blocked-mail events to Discord via mod-chat-transmitter.
#if __has_include("mod-chat-transmitter/src/ChatTransmitter.h")
#include "mod-chat-transmitter/src/ChatTransmitter.h"
#define HAS_CHAT_TRANSMITTER 1
#endif

namespace
{
    // What to do when content matches a filtered keyword. Higher values block
    // the content and escalate the punishment. Shared by the mail and chat filters.
    enum MailFilterAction : uint8
    {
        MAIL_FILTER_DISABLED    = 0,
        MAIL_FILTER_NOTIFY      = 1,
        MAIL_FILTER_KICK        = 2,
        MAIL_FILTER_BAN_ACCOUNT = 3,
        MAIL_FILTER_BAN_IP      = 4,
    };

    constexpr char const* MAIL_FILTER_BAN_REASON = "Mail filter: prohibited content (advertising/scam)";
    constexpr char const* CHAT_FILTER_BAN_REASON = "Chat filter: prohibited content (advertising/scam)";

    bool _enabled = true;
    uint8 _mailFilterAction = MAIL_FILTER_DISABLED;
    std::string _mailFilterMessage;
    std::string _mailFilterBanAuthor;
    std::vector<std::string> _mailFilterKeywords;

    // Same keyword list and action scale as the mail filter, applied to SAY/YELL/EMOTE.
    uint8 _chatFilterAction = MAIL_FILTER_DISABLED;
    std::string _chatFilterMessage;

    // Aggressive (whitespace-collapsed) matching for mail and chat only runs for
    // senders at or below this level whose text also carries a URL marker. 0
    // disables it. Matches reuse each filter's own action.
    uint8 _aggressiveMaxLevel = 0;

    // Cross-message (windowed) chat matching joins a sender's recent SAY/YELL/
    // EMOTE lines and matches the combined text, to catch ads split over several
    // messages so that no single line carries a complete URL. Size is the max
    // number of lines kept per sender; Seconds is how long a line stays in the window.
    // Both must be > 0 (and Size >= 2) for the pass to run; it is additionally
    // gated by _aggressiveMaxLevel, so only low-level senders are joined.
    uint32 _chatWindowSize = 0;
    uint32 _chatWindowSeconds = 0;

    // Mail carrying at least this much money (in copper) is logged (and relayed
    // to Discord). 0 disables the check. Parsed from a g/s/c money string.
    uint32 _goldFilterThresholdCopper = 0;

    // Looting an item whose RequiredLevel exceeds the looter's level by at least
    // this many levels is logged (and relayed to Discord). 0 disables the check.
    // Surfaces low-level characters pulling high-level gear from world chests etc.
    uint32 _lootFilterLevelGap = 0;

    // Restricts the loot check to looters at or below this level. 0 means no cap
    // (applies to every level). Lets the check target only low-level characters.
    uint8 _lootFilterMaxLevel = 0;

    bool _startupNoticeEnabled = false;
    std::string _startupNoticeMessage;
    uint32 _startupNoticeDelaySeconds = 5;

    std::string ToLowerAscii(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());
        for (char c : input)
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        return out;
    }

    // Returns the first keyword contained in the text (case-insensitive), or
    // empty if the text is clean.
    std::string FindMatchingKeyword(std::string const& text)
    {
        std::string const haystack = ToLowerAscii(text);
        for (std::string const& keyword : _mailFilterKeywords)
        {
            if (haystack.find(keyword) != std::string::npos)
                return keyword;
        }

        return {};
    }

    std::string StripWhitespace(std::string const& input)
    {
        std::string out;
        out.reserve(input.size());
        for (char c : input)
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
                out.push_back(c);
        }

        return out;
    }

    // True if the (lowercased, whitespace-stripped) text carries a web/contact
    // marker. Used as the second signal for aggressive matching so that a
    // collapsed keyword hit alone (a despaced phrase can fuse into a real word)
    // isn't enough.
    bool HasUrlMarker(std::string const& collapsed)
    {
        static constexpr std::array<std::string_view, 9> markers =
        {
            "http", "www", "wvvw", ".com", ".net", ".org", "dotcom", "dotnet", "dotorg"
        };

        for (std::string_view marker : markers)
        {
            if (collapsed.find(marker) != std::string::npos)
                return true;
        }

        return false;
    }

    // Aggressive pass shared by the mail and chat filters: catches space-evaded
    // keywords, but only for a low-level sender whose text also carries a URL
    // marker. Both signals are required to keep despaced normal phrases from
    // matching. Returns the matched keyword, or empty.
    std::string FindAggressiveMatch(Player* player, std::string const& text)
    {
        if (_aggressiveMaxLevel == 0 || player->GetLevel() > _aggressiveMaxLevel)
            return {};

        std::string const collapsed = StripWhitespace(ToLowerAscii(text));
        if (!HasUrlMarker(collapsed))
            return {};

        return FindMatchingKeyword(collapsed);
    }

    // Cross-message chat history for the windowed pass: recent chat lines per
    // sender, pruned by age/size on insert and cleared on logout, so it stays small.
    struct TimedMessage
    {
        time_t time;
        std::string text;
    };
    std::unordered_map<ObjectGuid, std::deque<TimedMessage>> _recentChatMessages;

    void ClearChatWindow(ObjectGuid guid)
    {
        _recentChatMessages.erase(guid);
    }

    // Layer 3: reconstructs an ad split over several messages. Appends the new
    // line to the sender's window, prunes by age and size, then matches the
    // joined text with the strict and aggressive passes. Gated by the aggressive
    // level cap (only low-level senders are joined) to limit false positives.
    // Returns the matched keyword, or empty.
    std::string FindWindowedMatch(Player* player, std::string const& msg)
    {
        if (_chatWindowSize < 2 || _chatWindowSeconds == 0)
            return {};

        if (_aggressiveMaxLevel == 0 || player->GetLevel() > _aggressiveMaxLevel)
            return {};

        time_t const now = GameTime::GetGameTime().count();
        std::deque<TimedMessage>& history = _recentChatMessages[player->GetGUID()];

        while (!history.empty() && now - history.front().time > static_cast<time_t>(_chatWindowSeconds))
            history.pop_front();

        history.push_back({ now, msg });
        while (history.size() > _chatWindowSize)
            history.pop_front();

        // A single line was already checked by the strict and aggressive passes.
        if (history.size() < 2)
            return {};

        std::string joined;
        for (TimedMessage const& entry : history)
        {
            if (!joined.empty())
                joined.push_back(' ');
            joined += entry.text;
        }

        std::string matched = FindMatchingKeyword(joined);
        if (matched.empty())
            matched = FindAggressiveMatch(player, joined);

        return matched;
    }

    // Carries out the escalating punishment for a keyword match. The caller is
    // responsible for suppressing the offending content itself.
    void ApplyFilterAction(Player* player, uint8 action, std::string const& notifyMessage,
        char const* kickReason, char const* banReason)
    {
        switch (action)
        {
            case MAIL_FILTER_NOTIFY:
                if (!notifyMessage.empty())
                    ChatHandler(player->GetSession()).SendSysMessage(notifyMessage);
                break;
            case MAIL_FILTER_KICK:
                player->GetSession()->KickPlayer(kickReason);
                break;
            case MAIL_FILTER_BAN_ACCOUNT:
                sBan->BanAccountByPlayerName(player->GetName(), "0", banReason, _mailFilterBanAuthor);
                break;
            case MAIL_FILTER_BAN_IP:
                // Ban the account first, then the IP (BanIP also disconnects all sessions on it).
                sBan->BanAccountByPlayerName(player->GetName(), "0", banReason, _mailFilterBanAuthor);
                sBan->BanIP(player->GetSession()->GetRemoteAddress(), "0", banReason, _mailFilterBanAuthor);
                break;
            default:
                break;
        }
    }
}

namespace EnhancedSupport
{
    std::string NormalizeKeyword(std::string_view input)
    {
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
            input.remove_prefix(1);
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
            input.remove_suffix(1);

        return ToLowerAscii(input);
    }

    // Keywords live in the auth DB (enhanced_support_mail_keywords) and are
    // cached here so the mail hook never hits the DB on the hot path.
    void LoadKeywords()
    {
        _mailFilterKeywords.clear();

        QueryResult result = LoginDatabase.Query("SELECT keyword FROM enhanced_support_mail_keywords");
        if (!result)
            return;

        do
        {
            std::string keyword = NormalizeKeyword(result->Fetch()[0].Get<std::string>());
            if (!keyword.empty())
                _mailFilterKeywords.push_back(keyword);
        } while (result->NextRow());
    }

    std::vector<std::string> const& GetKeywords()
    {
        return _mailFilterKeywords;
    }

    bool HasKeyword(std::string const& normalized)
    {
        return std::find(_mailFilterKeywords.begin(), _mailFilterKeywords.end(), normalized) != _mailFilterKeywords.end();
    }

    void AddKeyword(std::string const& normalized)
    {
        std::string escaped = normalized;
        LoginDatabase.EscapeString(escaped);
        LoginDatabase.Execute("INSERT IGNORE INTO enhanced_support_mail_keywords (keyword) VALUES ('{}')", escaped);
        LoadKeywords();
    }

    void RemoveKeyword(std::string const& normalized)
    {
        std::string escaped = normalized;
        LoginDatabase.EscapeString(escaped);
        LoginDatabase.Execute("DELETE FROM enhanced_support_mail_keywords WHERE keyword = '{}'", escaped);
        LoadKeywords();
    }

    std::string const& GetBanAuthor()
    {
        return _mailFilterBanAuthor;
    }

    bool IsEnabled()
    {
        return _enabled;
    }

    uint8 GetMailFilterAction()
    {
        return _mailFilterAction;
    }

    uint8 GetMaxMailFilterAction()
    {
        return MAIL_FILTER_BAN_IP;
    }

    void SetMailFilterAction(uint8 action)
    {
        _mailFilterAction = action;
    }

    std::string_view GetFilterActionName(uint8 action)
    {
        switch (action)
        {
            case MAIL_FILTER_DISABLED:    return "disabled";
            case MAIL_FILTER_NOTIFY:      return "notify";
            case MAIL_FILTER_KICK:        return "kick";
            case MAIL_FILTER_BAN_ACCOUNT: return "ban account";
            case MAIL_FILTER_BAN_IP:      return "ban account + IP";
            default:                      return "unknown";
        }
    }

    std::string_view GetMailFilterActionName()
    {
        return GetFilterActionName(_mailFilterAction);
    }

    std::string const& GetMailFilterMessage()
    {
        return _mailFilterMessage;
    }

    uint8 GetChatFilterAction()
    {
        return _chatFilterAction;
    }

    std::string_view GetChatFilterActionName()
    {
        return GetFilterActionName(_chatFilterAction);
    }

    uint8 GetAggressiveMaxLevel()
    {
        return _aggressiveMaxLevel;
    }

    uint32 GetChatWindowSize()
    {
        return _chatWindowSize;
    }

    uint32 GetChatWindowSeconds()
    {
        return _chatWindowSeconds;
    }

    uint32 GetGoldFilterThreshold()
    {
        return _goldFilterThresholdCopper;
    }

    uint32 GetLootFilterLevelGap()
    {
        return _lootFilterLevelGap;
    }

    uint8 GetLootFilterMaxLevel()
    {
        return _lootFilterMaxLevel;
    }

    std::string FormatMoney(uint32 copper)
    {
        return Acore::StringFormat("{}g {}s {}c", copper / GOLD, (copper % GOLD) / SILVER, copper % SILVER);
    }

    void LoadConfig()
    {
        _enabled = sConfigMgr->GetOption<bool>("EnhancedSupport.Enable", true);
        _mailFilterAction = sConfigMgr->GetOption<uint8>("EnhancedSupport.MailFilter.Action", MAIL_FILTER_DISABLED);
        _mailFilterMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.MailFilter.Message",
            "Your mail was blocked because it contains a prohibited keyword.");
        _mailFilterBanAuthor = sConfigMgr->GetOption<std::string>("EnhancedSupport.MailFilter.BanAuthor", "SupportModule");

        _chatFilterAction = sConfigMgr->GetOption<uint8>("EnhancedSupport.ChatFilter.Action", MAIL_FILTER_DISABLED);
        _chatFilterMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.ChatFilter.Message",
            "Your message was blocked because it contains a prohibited keyword.");
        _aggressiveMaxLevel = sConfigMgr->GetOption<uint8>("EnhancedSupport.AggressiveMaxLevel", 0);

        _chatWindowSize = sConfigMgr->GetOption<uint32>("EnhancedSupport.ChatFilter.WindowSize", 0);
        _chatWindowSeconds = sConfigMgr->GetOption<uint32>("EnhancedSupport.ChatFilter.WindowSeconds", 0);

        // Accepts a g/s/c money string ("100g", "50g 30s") like the .send money
        // command; a bare number is copper. 0 (or unparseable) disables the check.
        std::string const goldThreshold = ToLowerAscii(sConfigMgr->GetOption<std::string>("EnhancedSupport.GoldFilter.Threshold", "0"));
        Optional<int32> const parsed = goldThreshold.find_first_of("gsc") != std::string::npos
            ? MoneyStringToMoney(goldThreshold)
            : Acore::StringTo<int32>(goldThreshold);
        if (!parsed)
        {
            LOG_WARN("module.enhancedsupport",
                "GoldFilter: could not parse EnhancedSupport.GoldFilter.Threshold = \"{}\"; gold check disabled.", goldThreshold);
            _goldFilterThresholdCopper = 0;
        }
        else
            _goldFilterThresholdCopper = *parsed > 0 ? static_cast<uint32>(*parsed) : 0;

        _lootFilterLevelGap = sConfigMgr->GetOption<uint32>("EnhancedSupport.LootFilter.LevelGap", 0);
        _lootFilterMaxLevel = sConfigMgr->GetOption<uint8>("EnhancedSupport.LootFilter.MaxLevel", 0);

        _startupNoticeEnabled = sConfigMgr->GetOption<bool>("EnhancedSupport.StartupNotice.Enable", false);
        _startupNoticeMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.StartupNotice.Message", "Server restarted!");
        _startupNoticeDelaySeconds = sConfigMgr->GetOption<uint32>("EnhancedSupport.StartupNotice.DelaySeconds", 5);
    }
}

// Reads config once on startup and on .reload config, so hot paths can read the
// cached value instead of calling sConfigMgr each time. Keywords are loaded from
// the DB on startup (config load runs before the DB is ready).
class EnhancedSupportWorldScript : public WorldScript
{
public:
    EnhancedSupportWorldScript() : WorldScript("EnhancedSupportWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        EnhancedSupport::LoadConfig();
    }

    void OnStartup() override
    {
        EnhancedSupport::LoadKeywords();

        if (!_enabled || !_startupNoticeEnabled)
            return;

        // The Discord relay (mod-chat-transmitter) connects its WebSocket on a
        // background thread during startup, so sending here directly is racy.
        // Defer a few seconds: by then the relay's client exists and its own
        // queue buffers the request until the handshake completes.
        _scheduler.Schedule(std::chrono::seconds(_startupNoticeDelaySeconds), [](TaskContext /*context*/)
        {
            SendStartupNotice();
        });
    }

    void OnUpdate(uint32 diff) override
    {
        _scheduler.Update(diff);
    }

private:
    static void SendStartupNotice()
    {
        // Re-check: an admin may have disabled it via .reload config during the delay.
        if (!_enabled || !_startupNoticeEnabled)
            return;

#ifdef HAS_CHAT_TRANSMITTER
        std::string note = Acore::StringFormat(
            "🔄 **{}**\n```\n{}\n```",
            _startupNoticeMessage, GitRevision::GetFullVersion());
        sChatTransmitter->QueueNotification("ServerStatus", note);
        LOG_INFO("module.enhancedsupport", "StartupNotice: queued Discord server-start notice (revision {})", GitRevision::GetHash());
#else
        LOG_WARN("module.enhancedsupport", "StartupNotice is enabled but mod-chat-transmitter is not available; no Discord notice will be sent.");
#endif
    }

    TaskScheduler _scheduler;
};

// Blocks player-sent mail whose subject/body match a configured keyword, and
// logs (without blocking) mail carrying gold above the configured threshold.
class EnhancedSupportMailFilter : public PlayerScript
{
public:
    EnhancedSupportMailFilter() : PlayerScript("EnhancedSupportMailFilter", {
        PLAYERHOOK_CAN_SEND_MAIL
    }) { }

    bool OnPlayerCanSendMail(Player* player, ObjectGuid receiverGuid, ObjectGuid /*mailbox*/,
        std::string& subject, std::string& body, uint32 money, uint32 /*COD*/, Item* /*item*/) override
    {
        if (!_enabled)
            return true;

        // Log-only: flag unusually large gold transfers without blocking them.
        LogHighValueMail(player, receiverGuid, money);

        if (_mailFilterAction == MAIL_FILTER_DISABLED || _mailFilterKeywords.empty())
            return true;

        std::string const text = subject + '\n' + body;
        std::string matched = FindMatchingKeyword(text);
        bool aggressive = false;

        // Aggressive collapsed match for low-level senders (see helper).
        if (matched.empty())
        {
            matched = FindAggressiveMatch(player, text);
            if (!matched.empty())
                aggressive = true;
        }

        if (matched.empty())
            return true;

        std::string const receiverName = ResolveCharacterName(receiverGuid);

        LOG_INFO("module.enhancedsupport",
            "MailFilter: blocked mail from {} ({}, level {}) to {} ({}) - matched keyword '{}', layer {}, action {} | subject: \"{}\" | body: \"{}\"",
            player->GetName(), player->GetGUID().GetCounter(), static_cast<uint32>(player->GetLevel()),
            receiverName, receiverGuid.GetCounter(), matched,
            aggressive ? "aggressive" : "strict", static_cast<uint32>(_mailFilterAction), subject, body);

#ifdef HAS_CHAT_TRANSMITTER
        {
            std::string note = Acore::StringFormat(
                "🚫 **Mail blocked** — keyword `{}`, {} layer, action `{}`\n"
                "👤 From: **{}** (GUID {}, level {}) | Account {} | IP {}\n"
                "📬 To: **{}** (GUID {})\n"
                "✉️ Subject: {}\n"
                "📝 Body: {}",
                matched, aggressive ? "aggressive" : "strict", EnhancedSupport::GetMailFilterActionName(),
                player->GetName(), player->GetGUID().GetCounter(), static_cast<uint32>(player->GetLevel()),
                player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
                receiverName, receiverGuid.GetCounter(),
                subject, body);
            sChatTransmitter->QueueNotification("MailFilter", note);
        }
#endif

        ApplyFilterAction(player, _mailFilterAction, _mailFilterMessage,
            "EnhancedSupport: prohibited mail content", MAIL_FILTER_BAN_REASON);

        return false;
    }

private:
    static std::string ResolveCharacterName(ObjectGuid guid)
    {
        std::string name;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, name) || name.empty())
            name = "Unknown";
        return name;
    }

    static void LogHighValueMail(Player* player, ObjectGuid receiverGuid, uint32 money)
    {
        if (_goldFilterThresholdCopper == 0 || money < _goldFilterThresholdCopper)
            return;

        std::string const amount = EnhancedSupport::FormatMoney(money);
        std::string const threshold = EnhancedSupport::FormatMoney(_goldFilterThresholdCopper);
        std::string const receiverName = ResolveCharacterName(receiverGuid);

        LOG_INFO("module.enhancedsupport",
            "MailFilter: high-value mail from {} ({}) to {} ({}) - {} (threshold {}) | Account {} | IP {}",
            player->GetName(), player->GetGUID().GetCounter(),
            receiverName, receiverGuid.GetCounter(),
            amount, threshold,
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress());

#ifdef HAS_CHAT_TRANSMITTER
        std::string note = Acore::StringFormat(
            "💰 **High-value mail** — {} (threshold {})\n"
            "👤 From: **{}** (GUID {}) | Account {} | IP {}\n"
            "📬 To: **{}** (GUID {})",
            amount, threshold,
            player->GetName(), player->GetGUID().GetCounter(),
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
            receiverName, receiverGuid.GetCounter());
        sChatTransmitter->QueueNotification("MailFilter", note);
#endif
    }
};

// Filters player chat against the same keyword list as the mail filter. SAY/YELL/
// EMOTE arrive via OnPlayerBeforeSendChatMessage, which can't abort the broadcast,
// so a matched message is blanked. Party chat arrives via OnPlayerCanUseChat, a
// boolean hook that aborts the broadcast outright when we return false.
class EnhancedSupportChatFilter : public PlayerScript
{
public:
    EnhancedSupportChatFilter() : PlayerScript("EnhancedSupportChatFilter", {
        PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& /*lang*/, std::string& msg) override
    {
        if (!FilterEnabled() || (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL && type != CHAT_MSG_EMOTE))
            return;

        // This hook runs before the message is broadcast but can't stop it, so
        // clear the text to keep the prohibited content from reaching others.
        if (FilterMessage(player, type, msg))
            msg.clear();
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* /*group*/) override
    {
        if (!FilterEnabled() || (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER))
            return true;

        // Returning false aborts the broadcast, so no need to blank the text.
        return !FilterMessage(player, type, msg);
    }

    void OnPlayerLogout(Player* player) override
    {
        ClearChatWindow(player->GetGUID());
    }

private:
    static bool FilterEnabled()
    {
        return _enabled && _chatFilterAction != MAIL_FILTER_DISABLED && !_mailFilterKeywords.empty();
    }

    // Runs both match layers over a chat message; on a hit it logs, relays to
    // Discord and applies the configured action. Returns true if the message
    // matched and must be suppressed by the caller.
    static bool FilterMessage(Player* player, uint32 type, std::string const& msg)
    {
        // Layer 1: strict contiguous match, applies to every sender.
        std::string matched = FindMatchingKeyword(msg);
        char const* layer = "strict";

        // Layer 2: aggressive collapsed match for low-level senders (see helper).
        if (matched.empty())
        {
            matched = FindAggressiveMatch(player, msg);
            if (!matched.empty())
                layer = "aggressive";
        }

        // Layer 3: windowed match across the sender's recent lines (see helper),
        // for ads split over several messages.
        if (matched.empty())
        {
            matched = FindWindowedMatch(player, msg);
            if (!matched.empty())
                layer = "windowed";
        }

        if (matched.empty())
            return false;

        // A match means the buffered lines have been acted on; drop the history
        // so the same window doesn't re-fire on the sender's next message.
        ClearChatWindow(player->GetGUID());

        LOG_INFO("module.enhancedsupport",
            "ChatFilter: blocked {} from {} ({}, level {}) - matched keyword '{}', layer {}, action {} | message: \"{}\"",
            ChatTypeName(type), player->GetName(), player->GetGUID().GetCounter(),
            static_cast<uint32>(player->GetLevel()), matched,
            layer, static_cast<uint32>(_chatFilterAction), msg);

#ifdef HAS_CHAT_TRANSMITTER
        {
            std::string note = Acore::StringFormat(
                "🚫 **Chat blocked** — keyword `{}`, {} layer, action `{}`\n"
                "👤 From: **{}** (GUID {}, level {}) | Account {} | IP {}\n"
                "💬 Channel: {}\n"
                "📝 Message: {}",
                matched, layer, EnhancedSupport::GetChatFilterActionName(),
                player->GetName(), player->GetGUID().GetCounter(), static_cast<uint32>(player->GetLevel()),
                player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
                ChatTypeName(type), msg);
            sChatTransmitter->QueueNotification("ChatFilter", note);
        }
#endif

        ApplyFilterAction(player, _chatFilterAction, _chatFilterMessage,
            "EnhancedSupport: prohibited chat content", CHAT_FILTER_BAN_REASON);

        return true;
    }

    static char const* ChatTypeName(uint32 type)
    {
        switch (type)
        {
            case CHAT_MSG_SAY:          return "say";
            case CHAT_MSG_YELL:         return "yell";
            case CHAT_MSG_EMOTE:        return "emote";
            case CHAT_MSG_PARTY:        return "party";
            case CHAT_MSG_PARTY_LEADER: return "party leader";
            default:                    return "chat";
        }
    }
};

// Logs (without blocking) when a character loots an item whose required level
// sits far above their own - e.g. a level-5 character pulling high-level gear
// from a world chest. Log-only; intended to surface boosting or exploits.
class EnhancedSupportLootFilter : public PlayerScript
{
public:
    EnhancedSupportLootFilter() : PlayerScript("EnhancedSupportLootFilter", {
        PLAYERHOOK_ON_LOOT_ITEM
    }) { }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootguid) override
    {
        if (!_enabled || _lootFilterLevelGap == 0 || !item)
            return;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->RequiredLevel == 0)
            return;

        uint32 const playerLevel = player->GetLevel();
        if (_lootFilterMaxLevel != 0 && playerLevel > _lootFilterMaxLevel)
            return;

        if (proto->RequiredLevel <= playerLevel)
            return;

        uint32 const gap = proto->RequiredLevel - playerLevel;
        if (gap < _lootFilterLevelGap)
            return;

        LootSource const src = ResolveLootSource(player, lootguid);

        // Fish are caught well above the looter's level by design, so skip loot
        // pulled from a fishing bobber or fishing hole.
        if (src.isFishing)
            return;

        LOG_INFO("module.enhancedsupport",
            "LootFilter: {} ({}, level {}) looted {}x [{}] ({}, requires level {}, gap {}) from {} {} (entry {}, spawn {}) | Account {} | IP {}",
            player->GetName(), player->GetGUID().GetCounter(), playerLevel,
            count, proto->Name1, proto->ItemId, proto->RequiredLevel, gap,
            src.label, src.name, src.entry, src.spawnId,
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress());

#ifdef HAS_CHAT_TRANSMITTER
        std::string sourceLine = FormatDiscordSource(src);

        // No entry means no aowow link (despawned source, container, corpse,
        // player); give the GM a ready-to-paste teleport to the looter instead.
        if (!src.entry)
            sourceLine += Acore::StringFormat(
                "\n🧭 Looter at: `.go xyz {:.3f} {:.3f} {:.3f} {} {:.3f}`",
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                player->GetMapId(), player->GetOrientation());

        std::string note = Acore::StringFormat(
            "📦 **Underlevel loot** — requires level {}, looter level {} (gap {})\n"
            "👤 **{}** (GUID {}) | Account {} | IP {}\n"
            "🎁 Item: [{}](https://wowgaming.altervista.org/aowow/?item={}) (id {}) x{}\n"
            "📍 Source: {}",
            proto->RequiredLevel, playerLevel, gap,
            player->GetName(), player->GetGUID().GetCounter(),
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
            proto->Name1, proto->ItemId, proto->ItemId, count,
            sourceLine);
        sChatTransmitter->QueueNotification("ItemLoot", note);
#endif
    }

private:
    struct LootSource
    {
        char const* label = "unknown source";
        std::string name;                // creature/object name; empty if unresolved
        uint32 entry = 0;                // template entry, for the aowow link; 0 if unresolved
        ObjectGuid::LowType spawnId = 0; // DB spawn id; 0 for temporary objects (e.g. fishing bobbers)
        bool isCreature = false;
        bool isGameObject = false;
        bool isFishing = false;
    };

    // Resolves the live loot source on the player's map to recover its template
    // entry and DB spawn id - the loot GUID itself is a transient runtime GUID,
    // not the stable spawn id.
    static LootSource ResolveLootSource(Player* player, ObjectGuid lootguid)
    {
        LootSource src;
        Map* map = player->GetMap();

        if (lootguid.IsGameObject())
        {
            src.isGameObject = true;
            src.label = "object/chest";
            if (GameObject* go = map->GetGameObject(lootguid))
            {
                src.entry = go->GetEntry();
                src.spawnId = go->GetSpawnId();
                src.name = go->GetName();
                GameobjectTypes const type = go->GetGoType();
                src.isFishing = type == GAMEOBJECT_TYPE_FISHINGNODE || type == GAMEOBJECT_TYPE_FISHINGHOLE;
            }
        }
        else if (lootguid.IsCreature())
        {
            src.isCreature = true;
            src.label = "creature";
            if (Creature* creature = map->GetCreature(lootguid))
            {
                src.entry = creature->GetEntry();
                src.spawnId = creature->GetSpawnId();
                src.name = creature->GetName();
            }
        }
        else if (lootguid.IsItem())
            src.label = "container";
        else if (lootguid.IsCorpse())
            src.label = "corpse";
        else if (lootguid.IsPlayer())
            src.label = "player";

        return src;
    }

#ifdef HAS_CHAT_TRANSMITTER
    // Discord "Source" line, linking creatures/objects to their aowow page by
    // template entry. Other sources (containers, corpses) carry no link.
    static std::string FormatDiscordSource(LootSource const& src)
    {
        if (src.isCreature && src.entry)
            return Acore::StringFormat(
                "{} [{} #{}](https://wowgaming.altervista.org/aowow/?npc={}) (spawn {})",
                src.label, src.name, src.entry, src.entry, src.spawnId);

        if (src.isGameObject && src.entry)
            return Acore::StringFormat(
                "{} [{} #{}](https://wowgaming.altervista.org/aowow/?object={}) (spawn {})",
                src.label, src.name, src.entry, src.entry, src.spawnId);

        return std::string(src.label);
    }
#endif
};

void AddEnhancedSupportScripts()
{
    new EnhancedSupportWorldScript();
    new EnhancedSupportMailFilter();
    new EnhancedSupportChatFilter();
    new EnhancedSupportLootFilter();
}
