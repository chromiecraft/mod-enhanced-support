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

#include "BanMgr.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

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

    std::string ToLowerAscii(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());
        for (char c : input)
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        return out;
    }

    // Trims surrounding whitespace and lowercases, giving the canonical form
    // used for both storage and matching.
    std::string NormalizeKeyword(std::string_view input)
    {
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())))
            input.remove_prefix(1);
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back())))
            input.remove_suffix(1);

        return ToLowerAscii(input);
    }

    // Keywords live in the characters DB (enhanced_support_mail_keywords) and
    // are cached here so the mail hook never hits the DB on the hot path.
    void LoadMailFilterKeywords()
    {
        _mailFilterKeywords.clear();

        QueryResult result = CharacterDatabase.Query("SELECT keyword FROM enhanced_support_mail_keywords");
        if (!result)
            return;

        do
        {
            std::string keyword = NormalizeKeyword(result->Fetch()[0].Get<std::string>());
            if (!keyword.empty())
                _mailFilterKeywords.push_back(keyword);
        } while (result->NextRow());
    }

    bool HasMailFilterKeyword(std::string const& normalized)
    {
        return std::find(_mailFilterKeywords.begin(), _mailFilterKeywords.end(), normalized) != _mailFilterKeywords.end();
    }

    void AddMailFilterKeyword(std::string const& normalized)
    {
        std::string escaped = normalized;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.Execute("INSERT IGNORE INTO enhanced_support_mail_keywords (keyword) VALUES ('{}')", escaped);
        LoadMailFilterKeywords();
    }

    void RemoveMailFilterKeyword(std::string const& normalized)
    {
        std::string escaped = normalized;
        CharacterDatabase.EscapeString(escaped);
        CharacterDatabase.Execute("DELETE FROM enhanced_support_mail_keywords WHERE keyword = '{}'", escaped);
        LoadMailFilterKeywords();
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

    void LoadEnhancedSupportConfig()
    {
        _enabled = sConfigMgr->GetOption<bool>("EnhancedSupport.Enable", true);
        _mailFilterAction = sConfigMgr->GetOption<uint8>("EnhancedSupport.MailFilter.Action", MAIL_FILTER_DISABLED);
        _mailFilterMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.MailFilter.Message",
            "Your mail was blocked because it contains a prohibited keyword.");
        _mailFilterBanAuthor = sConfigMgr->GetOption<std::string>("EnhancedSupport.MailFilter.BanAuthor", "SupportModule");
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
        WORLDHOOK_ON_STARTUP
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadEnhancedSupportConfig();
    }

    void OnStartup() override
    {
        LoadMailFilterKeywords();
    }
};

// Blocks player-sent mail whose subject/body match a configured keyword.
class EnhancedSupportMailFilter : public PlayerScript
{
public:
    EnhancedSupportMailFilter() : PlayerScript("EnhancedSupportMailFilter", {
        PLAYERHOOK_CAN_SEND_MAIL
    }) { }

    bool OnPlayerCanSendMail(Player* player, ObjectGuid receiverGuid, ObjectGuid /*mailbox*/,
        std::string& subject, std::string& body, uint32 /*money*/, uint32 /*COD*/, Item* /*item*/) override
    {
        if (!_enabled || _mailFilterAction == MAIL_FILTER_DISABLED || _mailFilterKeywords.empty())
            return true;

        std::string const matched = FindMatchingKeyword(subject, body);
        if (matched.empty())
            return true;

        LOG_INFO("module.enhancedsupport",
            "MailFilter: blocked mail from {} ({}) to {} - matched keyword '{}', action {} | subject: \"{}\" | body: \"{}\"",
            player->GetName(), player->GetGUID().ToString(), receiverGuid.ToString(), matched,
            static_cast<uint32>(_mailFilterAction), subject, body);

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
};

class EnhancedSupportCommandScript : public CommandScript
{
public:
    EnhancedSupportCommandScript() : CommandScript("EnhancedSupportCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable listTable =
        {
            { "bans",     HandleListBansCommand,     SEC_GAMEMASTER, Console::Yes },
            { "keywords", HandleListKeywordsCommand, SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable keywordTable =
        {
            { "add",    HandleKeywordAddCommand,    SEC_ADMINISTRATOR, Console::Yes },
            { "remove", HandleKeywordRemoveCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable supportTable =
        {
            { "reload",  HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "list",    listTable },
            { "keyword", keywordTable },
        };

        static ChatCommandTable commandTable =
        {
            { "support", supportTable },
        };

        return commandTable;
    }

    // Re-reads this module's options and keywords, independent of .reload config.
    static bool HandleReloadCommand(ChatHandler* handler)
    {
        sConfigMgr->LoadModulesConfigs(true, false);
        LoadEnhancedSupportConfig();
        LoadMailFilterKeywords();
        handler->PSendSysMessage("mod-enhanced-support: configuration and keywords reloaded.");
        return true;
    }

    static bool HandleListKeywordsCommand(ChatHandler* handler)
    {
        if (_mailFilterKeywords.empty())
        {
            handler->SendSysMessage("mod-enhanced-support: no mail keywords configured.");
            return true;
        }

        handler->PSendSysMessage("mod-enhanced-support: {} mail keyword(s):", _mailFilterKeywords.size());
        for (std::string const& keyword : _mailFilterKeywords)
            handler->PSendSysMessage(" - {}", keyword);

        return true;
    }

    static bool HandleKeywordAddCommand(ChatHandler* handler, Tail keyword)
    {
        std::string normalized = NormalizeKeyword(std::string{ keyword });
        if (normalized.empty())
        {
            handler->SendSysMessage("Usage: .support keyword add <keyword>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (HasMailFilterKeyword(normalized))
        {
            handler->PSendSysMessage("Keyword already present: {}", normalized);
            return true;
        }

        AddMailFilterKeyword(normalized);
        handler->PSendSysMessage("Added mail keyword: {}", normalized);
        return true;
    }

    static bool HandleKeywordRemoveCommand(ChatHandler* handler, Tail keyword)
    {
        std::string normalized = NormalizeKeyword(std::string{ keyword });
        if (normalized.empty())
        {
            handler->SendSysMessage("Usage: .support keyword remove <keyword>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!HasMailFilterKeyword(normalized))
        {
            handler->PSendSysMessage("Keyword not found: {}", normalized);
            handler->SetSentErrorMessage(true);
            return false;
        }

        RemoveMailFilterKeyword(normalized);
        handler->PSendSysMessage("Removed mail keyword: {}", normalized);
        return true;
    }

    // Lists the most recent account bans, newest first. The author substring defaults
    // to the module's configured ban author, so a bare call shows the module's own bans.
    static bool HandleListBansCommand(ChatHandler* handler, Optional<uint32> count, Tail author)
    {
        uint32 limit = count ? *count : 10;
        limit = std::clamp<uint32>(limit, 1, 50);

        std::string filter{ author };
        if (filter.empty())
            filter = _mailFilterBanAuthor;

        std::string where;
        if (!filter.empty())
        {
            std::string escaped = filter;
            LoginDatabase.EscapeString(escaped);
            where = Acore::StringFormat(" WHERE ab.bannedby LIKE '%{}%'", escaped);
        }

        QueryResult result = LoginDatabase.Query(
            "SELECT a.username, ab.bandate, ab.unbandate, ab.bannedby, ab.banreason, ab.active "
            "FROM account_banned ab JOIN account a ON a.id = ab.id{} "
            "ORDER BY ab.bandate DESC LIMIT {}", where, limit);

        if (!result)
        {
            handler->PSendSysMessage("No account bans found{}.",
                filter.empty() ? "" : Acore::StringFormat(" by author matching \"{}\"", filter));
            return true;
        }

        handler->PSendSysMessage("Last account ban(s){}:",
            filter.empty() ? "" : Acore::StringFormat(" by author matching \"{}\"", filter));

        uint32 shown = 0;
        do
        {
            Field* fields = result->Fetch();
            std::string username = fields[0].Get<std::string>();
            uint32 bandate = fields[1].Get<uint32>();
            uint32 unbandate = fields[2].Get<uint32>();
            std::string bannedby = fields[3].Get<std::string>();
            std::string banreason = fields[4].Get<std::string>();
            bool active = fields[5].Get<bool>();

            // unbandate == bandate is the convention for a permanent ban.
            std::string expiry = (unbandate == bandate)
                ? "permanent"
                : Acore::StringFormat("until {}", Acore::Time::TimeToTimestampStr(Seconds(unbandate)));

            handler->PSendSysMessage("{}. {} | {} | by {} | {} | {} | {}",
                ++shown, username, Acore::Time::TimeToTimestampStr(Seconds(bandate)),
                bannedby, active ? "active" : "expired", expiry, banreason);
        } while (result->NextRow());

        return true;
    }
};

void AddEnhancedSupportScripts()
{
    new EnhancedSupportWorldScript();
    new EnhancedSupportMailFilter();
    new EnhancedSupportCommandScript();
}
