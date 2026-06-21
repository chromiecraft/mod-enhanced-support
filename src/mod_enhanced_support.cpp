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
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Tokenize.h"
#include "WorldSession.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

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

    constexpr char const* MAIL_FILTER_BAN_AUTHOR = "SupportModule";
    constexpr char const* MAIL_FILTER_BAN_REASON = "Mail filter: prohibited content (advertising/scam)";

    bool _enabled = true;
    uint8 _mailFilterAction = MAIL_FILTER_DISABLED;
    std::string _mailFilterMessage;
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
        _enabled = sConfigMgr->GetOption<bool>("EnhancedSupport.Enable", true);
        _mailFilterAction = sConfigMgr->GetOption<uint8>("EnhancedSupport.MailFilter.Action", MAIL_FILTER_DISABLED);
        _mailFilterMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.MailFilter.Message",
            "Your mail was blocked because it contains a prohibited keyword.");
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
                sBan->BanAccountByPlayerName(player->GetName(), "0", MAIL_FILTER_BAN_REASON, MAIL_FILTER_BAN_AUTHOR);
                break;
            case MAIL_FILTER_BAN_IP:
                // Ban the account first, then the IP (BanIP also disconnects all sessions on it).
                sBan->BanAccountByPlayerName(player->GetName(), "0", MAIL_FILTER_BAN_REASON, MAIL_FILTER_BAN_AUTHOR);
                sBan->BanIP(player->GetSession()->GetRemoteAddress(), "0", MAIL_FILTER_BAN_REASON, MAIL_FILTER_BAN_AUTHOR);
                break;
            default:
                break;
        }

        return false;
    }
};

void AddEnhancedSupportScripts()
{
    new EnhancedSupportWorldScript();
    new EnhancedSupportMailFilter();
}
