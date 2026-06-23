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
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "Timer.h"

#include <algorithm>

using namespace Acore::ChatCommands;

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
            { "info",    HandleInfoCommand,   SEC_GAMEMASTER,    Console::Yes },
            { "action",  HandleActionCommand, SEC_ADMINISTRATOR, Console::Yes },
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

    static bool HandleInfoCommand(ChatHandler* handler)
    {
        handler->SendSysMessage("mod-enhanced-support - active settings:");
        handler->PSendSysMessage("  Module enabled: {}", EnhancedSupport::IsEnabled() ? "yes" : "no");
        handler->PSendSysMessage("  Mail filter action: {} ({})",
            static_cast<uint32>(EnhancedSupport::GetMailFilterAction()), EnhancedSupport::GetMailFilterActionName());
        handler->PSendSysMessage("  Chat filter action: {} ({})",
            static_cast<uint32>(EnhancedSupport::GetChatFilterAction()), EnhancedSupport::GetChatFilterActionName());

        uint8 const aggressiveMaxLevel = EnhancedSupport::GetAggressiveMaxLevel();
        if (aggressiveMaxLevel == 0)
            handler->PSendSysMessage("  Aggressive pass: disabled");
        else
            handler->PSendSysMessage("  Aggressive pass: enabled for level <= {}",
                static_cast<uint32>(aggressiveMaxLevel));

        handler->PSendSysMessage("  Keywords loaded: {}", EnhancedSupport::GetKeywords().size());

        uint32 const goldThreshold = EnhancedSupport::GetGoldFilterThreshold();
        handler->PSendSysMessage("  Gold filter threshold: {}",
            goldThreshold == 0 ? "disabled" : EnhancedSupport::FormatMoney(goldThreshold));

        uint32 const lootLevelGap = EnhancedSupport::GetLootFilterLevelGap();
        if (lootLevelGap == 0)
            handler->SendSysMessage("  Loot filter level gap: disabled");
        else
            handler->PSendSysMessage("  Loot filter level gap: {}", lootLevelGap);

        handler->PSendSysMessage("  Ban author: {}", EnhancedSupport::GetBanAuthor());

        std::string const& message = EnhancedSupport::GetMailFilterMessage();
        handler->PSendSysMessage("  Notify message: {}", message.empty() ? "(none)" : message);
        return true;
    }

    // Runtime-only override of the mail filter action; not saved, reverts on reload.
    static bool HandleActionCommand(ChatHandler* handler, uint8 action)
    {
        if (action > EnhancedSupport::GetMaxMailFilterAction())
        {
            handler->PSendSysMessage("Usage: .support action <0-{}> (0=off, 1=notify, 2=kick, 3=ban account, 4=ban account+IP)",
                EnhancedSupport::GetMaxMailFilterAction());
            handler->SetSentErrorMessage(true);
            return false;
        }

        EnhancedSupport::SetMailFilterAction(action);
        handler->PSendSysMessage("Mail filter action set to {} ({}) - runtime only, reverts on reload.",
            static_cast<uint32>(action), EnhancedSupport::GetMailFilterActionName());
        return true;
    }

    // Re-reads this module's options and keywords, independent of .reload config.
    static bool HandleReloadCommand(ChatHandler* handler)
    {
        sConfigMgr->LoadModulesConfigs(true, false);
        EnhancedSupport::LoadConfig();
        EnhancedSupport::LoadKeywords();
        handler->PSendSysMessage("mod-enhanced-support: configuration and keywords reloaded.");
        return true;
    }

    static bool HandleListKeywordsCommand(ChatHandler* handler)
    {
        std::vector<std::string> const& keywords = EnhancedSupport::GetKeywords();
        if (keywords.empty())
        {
            handler->SendSysMessage("mod-enhanced-support: no mail keywords configured.");
            return true;
        }

        handler->PSendSysMessage("mod-enhanced-support: {} mail keyword(s):", keywords.size());
        for (std::string const& keyword : keywords)
            handler->PSendSysMessage(" - {}", keyword);

        return true;
    }

    static bool HandleKeywordAddCommand(ChatHandler* handler, Tail keyword)
    {
        std::string normalized = EnhancedSupport::NormalizeKeyword(keyword);
        if (normalized.empty())
        {
            handler->SendSysMessage("Usage: .support keyword add <keyword>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (EnhancedSupport::HasKeyword(normalized))
        {
            handler->PSendSysMessage("Keyword already present: {}", normalized);
            return true;
        }

        EnhancedSupport::AddKeyword(normalized);
        handler->PSendSysMessage("Added mail keyword: {}", normalized);
        return true;
    }

    static bool HandleKeywordRemoveCommand(ChatHandler* handler, Tail keyword)
    {
        std::string normalized = EnhancedSupport::NormalizeKeyword(keyword);
        if (normalized.empty())
        {
            handler->SendSysMessage("Usage: .support keyword remove <keyword>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!EnhancedSupport::HasKeyword(normalized))
        {
            handler->PSendSysMessage("Keyword not found: {}", normalized);
            handler->SetSentErrorMessage(true);
            return false;
        }

        EnhancedSupport::RemoveKeyword(normalized);
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
            filter = EnhancedSupport::GetBanAuthor();

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

void AddEnhancedSupportCommandScripts()
{
    new EnhancedSupportCommandScript();
}
