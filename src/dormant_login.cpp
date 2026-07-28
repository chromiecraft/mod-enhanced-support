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
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

#include <atomic>
#include <string>

// Optional integration: relay dormant-login alerts to Discord via mod-chat-transmitter.
#if __has_include("mod-chat-transmitter/src/ChatTransmitter.h")
#include "mod-chat-transmitter/src/ChatTransmitter.h"
#define HAS_CHAT_TRANSMITTER 1
#endif

// Dormant-login detection: leaked credentials (from other servers' breaches)
// are typically tried on accounts whose owner stopped playing months ago, from
// an IP unrelated to the owner's. When an account logs in after at least
// DormantLogin.Days of inactivity from an IP outside its last-known
// /IpMaskBits range, the login is logged and relayed to Discord. Detection
// only: nothing is blocked or restricted.
//
// The account table cannot serve as the baseline: the authserver overwrites
// account.last_ip and last_login at logon proof, before any worldserver hook
// runs. The module therefore keeps its own last-world-login row per account in
// the auth DB (enhanced_support_account_activity, seeded from the account
// table when the SQL update is applied). The row is updated on every world
// login even while the alert is disabled, so the baseline is fresh if
// DormantLogin.Days is raised above 0 later.

namespace
{
    // OnLastIpUpdate fires on a network thread while config reloads happen on
    // the world thread, hence atomics.
    std::atomic<uint32> _dormantDays{0};
    std::atomic<uint32> _dormantIpMaskBits{24};

    bool ParseIPv4(std::string const& text, uint32& out)
    {
        uint32 value = 0;
        size_t pos = 0;
        for (int octetIndex = 0; octetIndex < 4; ++octetIndex)
        {
            uint32 octet = 0;
            size_t digits = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9' && digits < 4)
            {
                octet = octet * 10 + (text[pos] - '0');
                ++pos;
                ++digits;
            }
            if (digits == 0 || digits > 3 || octet > 255)
                return false;
            value = (value << 8) | octet;
            if (octetIndex < 3)
            {
                if (pos >= text.size() || text[pos] != '.')
                    return false;
                ++pos;
            }
        }
        if (pos != text.size())
            return false;
        out = value;
        return true;
    }

    // maskBits is 1..32 here (0 is handled by the caller as "ignore the IP").
    // Non-IPv4 addresses (IPv6) fall back to exact-match comparison.
    bool SameIpRange(std::string const& a, std::string const& b, uint32 maskBits)
    {
        uint32 ipA = 0;
        uint32 ipB = 0;
        if (!ParseIPv4(a, ipA) || !ParseIPv4(b, ipB))
            return a == b;

        uint32 const mask = maskBits >= 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> maskBits);
        return (ipA & mask) == (ipB & mask);
    }
}

namespace EnhancedSupport
{
    void LoadDormantLoginConfig()
    {
        _dormantDays.store(sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.Days", 0));
        _dormantIpMaskBits.store(std::min<uint32>(sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.IpMaskBits", 24), 32));
    }

    uint32 GetDormantLoginDays()
    {
        return _dormantDays.load(std::memory_order_relaxed);
    }

    uint32 GetDormantLoginIpMaskBits()
    {
        return _dormantIpMaskBits.load(std::memory_order_relaxed);
    }
}

class EnhancedSupportDormantLogin : public AccountScript
{
public:
    EnhancedSupportDormantLogin() : AccountScript("EnhancedSupportDormantLogin", {
        ACCOUNTHOOK_ON_LAST_IP_UPDATE
    }) { }

    void OnLastIpUpdate(uint32 accountId, std::string ip) override
    {
        if (!EnhancedSupport::IsEnabled())
            return;

        // The DATE_FORMAT specifiers contain no braces, so they pass through
        // the fmt-style formatting untouched.
        QueryResult result = LoginDatabase.Query(
            "SELECT a.username, act.last_ip, UNIX_TIMESTAMP(act.last_seen), DATE_FORMAT(act.last_seen, '%Y-%m-%d %H:%i') "
            "FROM account a LEFT JOIN enhanced_support_account_activity act ON act.account_id = a.id "
            "WHERE a.id = {}", accountId);

        if (result)
        {
            Field* fields = result->Fetch();
            std::string const username = fields[0].Get<std::string>();
            std::string const lastIp = fields[1].IsNull() ? "" : fields[1].Get<std::string>();
            uint64 const lastSeen = fields[2].IsNull() ? 0 : fields[2].Get<uint64>();
            std::string const lastSeenText = fields[3].IsNull() ? "" : fields[3].Get<std::string>();

            uint32 const days = EnhancedSupport::GetDormantLoginDays();
            uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());

            if (days > 0 && lastSeen > 0 && now >= lastSeen + uint64(days) * DAY)
            {
                uint32 const maskBits = EnhancedSupport::GetDormantLoginIpMaskBits();
                // An empty stored IP gives no baseline to compare against, so
                // only the maskBits = 0 mode (dormancy alone) can fire then.
                bool const newIpRange = maskBits == 0
                    || (!lastIp.empty() && !SameIpRange(lastIp, ip, maskBits));

                if (newIpRange)
                {
                    uint64 const inactiveDays = (now - lastSeen) / DAY;
                    LOG_INFO("module.enhancedsupport",
                        "DormantLogin: account {} ({}) logged in from {} after {} day(s) inactive - last seen {} from {}",
                        username, accountId, ip, inactiveDays,
                        lastSeenText.empty() ? "unknown" : lastSeenText,
                        lastIp.empty() ? "unknown IP" : lastIp);

#ifdef HAS_CHAT_TRANSMITTER
                    std::string const note = Acore::StringFormat(
                        "⚠️ **Dormant account login** — **{}** (account {})\n"
                        "🌐 New IP: {} | inactive for {} day(s)\n"
                        "🕓 Last seen: {} from {}\n"
                        "🔎 Detection only, nothing was blocked. Verify whether the owner returned.",
                        username, accountId, ip, inactiveDays,
                        lastSeenText.empty() ? "unknown" : lastSeenText,
                        lastIp.empty() ? "unknown IP" : lastIp);
                    sChatTransmitter->QueueNotification("ChatFilter", note);
#endif
                }
            }
        }

        std::string escapedIp = ip;
        LoginDatabase.EscapeString(escapedIp);
        LoginDatabase.Execute(
            "REPLACE INTO enhanced_support_account_activity (account_id, last_seen, last_ip) VALUES ({}, NOW(), '{}')",
            accountId, escapedIp);
    }
};

void AddEnhancedSupportDormantLoginScripts()
{
    new EnhancedSupportDormantLogin();
}
