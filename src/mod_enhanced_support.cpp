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
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GitRevision.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "TaskScheduler.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <chrono>

// Optional integration: relay blocked-mail events to Discord via mod-chat-transmitter.
#if __has_include("mod-chat-transmitter/src/ChatTransmitter.h")
#include "mod-chat-transmitter/src/ChatTransmitter.h"
#define HAS_CHAT_TRANSMITTER 1
#endif

namespace
{
    // What to do when mail matches a filtered keyword. Higher values block the
    // mail and escalate the punishment.
    enum MailFilterAction : uint8
    {
        MAIL_FILTER_DISABLED    = 0,
        MAIL_FILTER_NOTIFY      = 1,
        MAIL_FILTER_KICK        = 2,
        MAIL_FILTER_BAN_ACCOUNT = 3,
        MAIL_FILTER_BAN_IP      = 4,
    };

    constexpr char const* MAIL_FILTER_BAN_REASON = "Mail filter: prohibited content (advertising/scam)";

    bool _enabled = true;
    uint8 _mailFilterAction = MAIL_FILTER_DISABLED;
    std::string _mailFilterMessage;
    std::string _mailFilterBanAuthor;
    std::vector<std::string> _mailFilterKeywords;

    // Mail carrying at least this much money (in copper) is logged (and relayed
    // to Discord). 0 disables the check. Parsed from a g/s/c money string.
    uint32 _goldFilterThresholdCopper = 0;

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

    // Returns the first matching keyword, or empty if the text is clean.
    std::string FindMatchingKeyword(std::string const& subject, std::string const& body)
    {
        std::string const haystack = ToLowerAscii(subject) + '\n' + ToLowerAscii(body);
        for (std::string const& keyword : _mailFilterKeywords)
        {
            if (haystack.find(keyword) != std::string::npos)
                return keyword;
        }

        return {};
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

    std::string_view GetMailFilterActionName()
    {
        switch (_mailFilterAction)
        {
            case MAIL_FILTER_DISABLED:    return "disabled";
            case MAIL_FILTER_NOTIFY:      return "notify";
            case MAIL_FILTER_KICK:        return "kick";
            case MAIL_FILTER_BAN_ACCOUNT: return "ban account";
            case MAIL_FILTER_BAN_IP:      return "ban account + IP";
            default:                      return "unknown";
        }
    }

    std::string const& GetMailFilterMessage()
    {
        return _mailFilterMessage;
    }

    uint32 GetGoldFilterThreshold()
    {
        return _goldFilterThresholdCopper;
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

        std::string const matched = FindMatchingKeyword(subject, body);
        if (matched.empty())
            return true;

        LOG_INFO("module.enhancedsupport",
            "MailFilter: blocked mail from {} ({}) to {} - matched keyword '{}', action {} | subject: \"{}\" | body: \"{}\"",
            player->GetName(), player->GetGUID().ToString(), receiverGuid.ToString(), matched,
            static_cast<uint32>(_mailFilterAction), subject, body);

#ifdef HAS_CHAT_TRANSMITTER
        {
            std::string note = Acore::StringFormat(
                "**{}** ({}) | Account {} | IP {}\nBlocked - keyword `{}`, action `{}`\nSubject: {}\nBody: {}",
                player->GetName(), player->GetGUID().ToString(),
                player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
                matched, EnhancedSupport::GetMailFilterActionName(), subject, body);
            sChatTransmitter->QueueNotification("MailFilter", note);
        }
#endif

        switch (_mailFilterAction)
        {
            case MAIL_FILTER_NOTIFY:
                if (!_mailFilterMessage.empty())
                    ChatHandler(player->GetSession()).SendSysMessage(_mailFilterMessage);
                break;
            case MAIL_FILTER_KICK:
                player->GetSession()->KickPlayer("EnhancedSupport: prohibited mail content");
                break;
            case MAIL_FILTER_BAN_ACCOUNT:
                sBan->BanAccountByPlayerName(player->GetName(), "0", MAIL_FILTER_BAN_REASON, _mailFilterBanAuthor);
                break;
            case MAIL_FILTER_BAN_IP:
                // Ban the account first, then the IP (BanIP also disconnects all sessions on it).
                sBan->BanAccountByPlayerName(player->GetName(), "0", MAIL_FILTER_BAN_REASON, _mailFilterBanAuthor);
                sBan->BanIP(player->GetSession()->GetRemoteAddress(), "0", MAIL_FILTER_BAN_REASON, _mailFilterBanAuthor);
                break;
            default:
                break;
        }

        return false;
    }

private:
    static void LogHighValueMail(Player* player, ObjectGuid receiverGuid, uint32 money)
    {
        if (_goldFilterThresholdCopper == 0 || money < _goldFilterThresholdCopper)
            return;

        std::string const amount = EnhancedSupport::FormatMoney(money);
        std::string const threshold = EnhancedSupport::FormatMoney(_goldFilterThresholdCopper);

        LOG_INFO("module.enhancedsupport",
            "GoldFilter: high-value mail from {} ({}) to {} - {} (threshold {}) | Account {} | IP {}",
            player->GetName(), player->GetGUID().ToString(), receiverGuid.ToString(),
            amount, threshold,
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress());

#ifdef HAS_CHAT_TRANSMITTER
        std::string note = Acore::StringFormat(
            "**{}** ({}) | Account {} | IP {}\nHigh-value mail - {} (threshold {})\nTo: {}",
            player->GetName(), player->GetGUID().ToString(),
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
            amount, threshold, receiverGuid.ToString());
        sChatTransmitter->QueueNotification("GoldFilter", note);
#endif
    }
};

void AddEnhancedSupportScripts()
{
    new EnhancedSupportWorldScript();
    new EnhancedSupportMailFilter();
}
