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
#include "AccountMgr.h"
#include "BanMgr.h"
#include "Chat.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Optional integration: relay dormant-login alerts to Discord via mod-chat-transmitter.
#if __has_include("mod-chat-transmitter/src/ChatTransmitter.h")
#include "mod-chat-transmitter/src/ChatTransmitter.h"
#define HAS_CHAT_TRANSMITTER 1
#endif

// Dormant-login detection: leaked credentials (from other servers' breaches)
// are typically tried on accounts whose owner stopped playing months ago, from
// an IP unrelated to the owner's. When an account logs in after at least
// DormantLogin.Days of inactivity from an IP outside its last-known
// /IpMaskBits range, the login is logged and relayed to Discord.
//
// With DormantLogin.BanMinutes at 0 that is all: detection only, nothing is
// blocked or restricted. Above 0 the account is additionally locked on the
// spot - outgoing mail, trades and the auction house are refused, so no value
// can be moved off the account - the player is warned in red to open a ticket,
// and the account is permanently banned once the minutes run out. A GM who
// verifies the owner through the ticket lifts the ban afterwards with the
// regular unban command. The pending-ban list is in-memory: a server restart
// clears pending locks (the next login from the foreign IP re-triggers them).
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
    constexpr char const* DORMANT_BAN_REASON = "Dormant login: connection from a new IP range";

    // OnLastIpUpdate fires on a network thread while config reloads happen on
    // the world thread, hence atomics.
    std::atomic<uint32> _dormantDays{0};
    std::atomic<uint32> _dormantIpMaskBits{24};
    std::atomic<uint32> _dormantBanMinutes{0};

    // Written on config (re)load and read by player hooks, all world thread.
    std::string _dormantLockMessage;

    // Accounts locked pending their ban: account id -> epoch seconds when the
    // ban fires. Inserted from the auth network thread, consumed on the world
    // thread; the count mirrors the map size so the per-tick and per-action
    // checks can skip the mutex while the map is empty.
    std::mutex _pendingBansMutex;
    std::unordered_map<uint32, uint64> _pendingBans;
    std::atomic<uint32> _pendingBanCount{0};

    bool IsAccountLocked(uint32 accountId)
    {
        if (_pendingBanCount.load(std::memory_order_relaxed) == 0)
            return false;

        std::lock_guard<std::mutex> guard(_pendingBansMutex);
        return _pendingBans.find(accountId) != _pendingBans.end();
    }

    void SendLockWarning(Player* player)
    {
        if (_dormantLockMessage.empty())
            return;

        ChatHandler(player->GetSession()).SendSysMessage("|cffff0000" + _dormantLockMessage + "|r");
    }

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
        _dormantBanMinutes.store(sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.BanMinutes", 0));
        _dormantLockMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.DormantLogin.LockMessage",
            "Suspicious activity has been detected on your account. The account will be locked - please open a Discord ticket to verify your identity and restore access.");
    }

    uint32 GetDormantLoginDays()
    {
        return _dormantDays.load(std::memory_order_relaxed);
    }

    uint32 GetDormantLoginIpMaskBits()
    {
        return _dormantIpMaskBits.load(std::memory_order_relaxed);
    }

    uint32 GetDormantLoginBanMinutes()
    {
        return _dormantBanMinutes.load(std::memory_order_relaxed);
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
                    uint32 const banMinutes = EnhancedSupport::GetDormantLoginBanMinutes();
                    if (banMinutes > 0)
                    {
                        std::lock_guard<std::mutex> guard(_pendingBansMutex);
                        // emplace: a relog while locked keeps the original deadline
                        _pendingBans.emplace(accountId, now + uint64(banMinutes) * MINUTE);
                        _pendingBanCount.store(static_cast<uint32>(_pendingBans.size()), std::memory_order_relaxed);
                    }

                    uint64 const inactiveDays = (now - lastSeen) / DAY;
                    LOG_INFO("module.enhancedsupport",
                        "DormantLogin: account {} ({}) logged in from {} after {} day(s) inactive - last seen {} from {}{}",
                        username, accountId, ip, inactiveDays,
                        lastSeenText.empty() ? "unknown" : lastSeenText,
                        lastIp.empty() ? "unknown IP" : lastIp,
                        banMinutes > 0
                            ? Acore::StringFormat(" - account locked, ban in {} minute(s)", banMinutes)
                            : "");

#ifdef HAS_CHAT_TRANSMITTER
                    std::string const note = Acore::StringFormat(
                        "⚠️ **Dormant account login** — **{}** (account {})\n"
                        "🌐 New IP: {} | inactive for {} day(s)\n"
                        "🕓 Last seen: {} from {}\n"
                        "{}",
                        username, accountId, ip, inactiveDays,
                        lastSeenText.empty() ? "unknown" : lastSeenText,
                        lastIp.empty() ? "unknown IP" : lastIp,
                        banMinutes > 0
                            ? Acore::StringFormat(
                                "🔒 Mail, trades and auction house are locked; the account will be "
                                "permanently banned in {} minute(s). Unban after verifying the owner.", banMinutes)
                            : "🔎 Detection only, nothing was blocked. Verify whether the owner returned.");
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

// Enforces the lock on flagged accounts: warns at login and refuses the ways
// value leaves an account (mail carries items/gold, trade carries both, the
// auction house launders through sales).
class EnhancedSupportDormantLock : public PlayerScript
{
public:
    EnhancedSupportDormantLock() : PlayerScript("EnhancedSupportDormantLock", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_CAN_SEND_MAIL,
        PLAYERHOOK_CAN_INIT_TRADE
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (EnhancedSupport::IsEnabled() && IsAccountLocked(player->GetSession()->GetAccountId()))
            SendLockWarning(player);
    }

    bool OnPlayerCanSendMail(Player* player, ObjectGuid /*receiverGuid*/, ObjectGuid /*mailbox*/,
        std::string& /*subject*/, std::string& /*body*/, uint32 /*money*/, uint32 /*COD*/, Item* /*item*/) override
    {
        if (!EnhancedSupport::IsEnabled() || !IsAccountLocked(player->GetSession()->GetAccountId()))
            return true;

        SendLockWarning(player);
        return false;
    }

    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        if (!EnhancedSupport::IsEnabled())
            return true;

        if (IsAccountLocked(player->GetSession()->GetAccountId()))
        {
            SendLockWarning(player);
            return false;
        }

        // Both directions are closed: a locked account must not receive goods
        // to fence any more than it may send them.
        if (target && IsAccountLocked(target->GetSession()->GetAccountId()))
        {
            ChatHandler(player->GetSession()).SendSysMessage("|cffff0000That player cannot trade right now.|r");
            return false;
        }

        return true;
    }
};

class EnhancedSupportDormantAuctionLock : public MiscScript
{
public:
    EnhancedSupportDormantAuctionLock() : MiscScript("EnhancedSupportDormantAuctionLock", {
        MISCHOOK_CAN_SEND_AUCTIONHELLO
    }) { }

    bool CanSendAuctionHello(WorldSession const* session, ObjectGuid /*guid*/, Creature* /*creature*/) override
    {
        if (!EnhancedSupport::IsEnabled() || !IsAccountLocked(session->GetAccountId()))
            return true;

        if (Player* player = session->GetPlayer())
            SendLockWarning(player);
        return false;
    }
};

// Executes the pending bans once their timer runs out. Runs on the world
// update loop; the account is banned by name (works whether or not the player
// is still online - BanAccount kicks any live session itself).
class EnhancedSupportDormantBanWorker : public WorldScript
{
public:
    EnhancedSupportDormantBanWorker() : WorldScript("EnhancedSupportDormantBanWorker", {
        WORLDHOOK_ON_UPDATE
    }) { }

    void OnUpdate(uint32 diff) override
    {
        if (_pendingBanCount.load(std::memory_order_relaxed) == 0)
            return;

        _timerMs += diff;
        if (_timerMs < 1000)
            return;
        _timerMs = 0;

        if (!EnhancedSupport::IsEnabled())
            return;

        uint64 const now = static_cast<uint64>(GameTime::GetGameTime().count());
        std::vector<uint32> due;
        {
            std::lock_guard<std::mutex> guard(_pendingBansMutex);
            for (auto it = _pendingBans.begin(); it != _pendingBans.end();)
            {
                if (now >= it->second)
                {
                    due.push_back(it->first);
                    it = _pendingBans.erase(it);
                }
                else
                    ++it;
            }
            _pendingBanCount.store(static_cast<uint32>(_pendingBans.size()), std::memory_order_relaxed);
        }

        for (uint32 accountId : due)
        {
            std::string accountName;
            if (!AccountMgr::GetName(accountId, accountName))
            {
                LOG_ERROR("module.enhancedsupport",
                    "DormantLogin: cannot ban account {} - account no longer exists", accountId);
                continue;
            }

            sBan->BanAccount(accountName, "0", DORMANT_BAN_REASON, EnhancedSupport::GetBanAuthor());
            LOG_INFO("module.enhancedsupport",
                "DormantLogin: account {} ({}) permanently banned - lock timer expired",
                accountName, accountId);

#ifdef HAS_CHAT_TRANSMITTER
            std::string const note = Acore::StringFormat(
                "⛔ **Dormant account banned** — **{}** (account {})\n"
                "The lock timer expired without GM intervention. Unban after verifying the owner.",
                accountName, accountId);
            sChatTransmitter->QueueNotification("ChatFilter", note);
#endif
        }
    }

private:
    uint32 _timerMs = 0;
};

void AddEnhancedSupportDormantLoginScripts()
{
    new EnhancedSupportDormantLogin();
    new EnhancedSupportDormantLock();
    new EnhancedSupportDormantAuctionLock();
    new EnhancedSupportDormantBanWorker();
}
