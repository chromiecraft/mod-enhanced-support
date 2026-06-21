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
#include "Tokenize.h"
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

    void LoadMailFilterKeywords()
    {
        _mailFilterKeywords.clear();

        std::string const raw = sConfigMgr->GetOption<std::string>("EnhancedSupport.MailFilter.Keywords", "");
        for (std::string_view token : Acore::Tokenize(raw, ',', false))
        {
            // Trim surrounding whitespace so "gold, wowgold" works.
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
                token.remove_prefix(1);
            while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
                token.remove_suffix(1);

            if (!token.empty())
                _mailFilterKeywords.push_back(ToLowerAscii(token));
        }
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
        LoadMailFilterKeywords();
    }
}

// Reads config once on startup and on .reload config, so hot paths can read the
// cached value instead of calling sConfigMgr each time.
class EnhancedSupportWorldScript : public WorldScript
{
public:
    EnhancedSupportWorldScript() : WorldScript("EnhancedSupportWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadEnhancedSupportConfig();
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

        LOG_INFO("module.enhancedsupport", "MailFilter: blocked mail from {} ({}) to {} - matched keyword '{}', action {}",
            player->GetName(), player->GetGUID().ToString(), receiverGuid.ToString(), matched, static_cast<uint32>(_mailFilterAction));

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
        static ChatCommandTable enhancedSupportTable =
        {
            { "reload", HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "bans",   HandleBansCommand,   SEC_GAMEMASTER,    Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "enhancedsupport", enhancedSupportTable },
        };

        return commandTable;
    }

    // Re-reads only this module's options from disk, independent of .reload config.
    static bool HandleReloadCommand(ChatHandler* handler)
    {
        sConfigMgr->LoadModulesConfigs(true, false);
        LoadEnhancedSupportConfig();
        handler->PSendSysMessage("mod-enhanced-support: configuration reloaded.");
        return true;
    }

    // Lists the most recent account bans, newest first. The author substring defaults
    // to the module's configured ban author, so a bare call shows the module's own bans.
    static bool HandleBansCommand(ChatHandler* handler, Optional<uint32> count, Tail author)
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
