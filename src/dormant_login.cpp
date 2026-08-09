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
#include "IPLocation.h"
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

    // How far the new IP's location has to match the last-known one for the lock
    // and ban to be waived (the report itself always goes out).
    enum DormantBanSkipLocation : uint32
    {
        BAN_SKIP_LOCATION_NONE    = 0,
        BAN_SKIP_LOCATION_COUNTRY = 1,
        BAN_SKIP_LOCATION_REGION  = 2
    };

    // acore_string entries shipped in data/sql/db-world/updates/enhanced_support_strings.sql.
    enum DormantLoginStrings : uint32
    {
        LANG_ES_DORMANT_LOCK_WARNING    = 90101,
        LANG_ES_DORMANT_TARGET_NO_TRADE = 90102
    };

    // OnLastIpUpdate fires on a network thread while config reloads happen on
    // the world thread, hence atomics.
    std::atomic<uint32> _dormantDays{0};
    std::atomic<uint32> _dormantIpMaskBits{24};
    std::atomic<uint32> _dormantBanMinutes{0};
    std::atomic<bool> _dormantShowCountry{true};
    std::atomic<uint32> _dormantBanSkipLocation{0};

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
        ChatHandler handler(player->GetSession());
        handler.SendSysMessage("|cffff0000" + handler.GetAcoreString(LANG_ES_DORMANT_LOCK_WARNING) + "|r");
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

    // Splits the country field back into the CSV columns it was built from.
    // IP2Location quotes every field, so once the core's reader has stripped the
    // quotes a separator is a comma with nothing after it, while a comma inside
    // a name always has a space ("Palestine, State of").
    std::vector<std::string> SplitCsvFields(std::string const& text)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == ',' && (i + 1 == text.size() || text[i + 1] != ' '))
            {
                parts.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        parts.push_back(text.substr(start));
        return parts;
    }

    // The core's IP2Location reader keeps the first three CSV columns and glues
    // everything after them into CountryName: just the country for a DB1 file,
    // "Australia,Queensland,Brisbane" for DB3. Index 0 is the country, 1 the
    // region and 2 the city; the last two need a DB3 database.
    std::string LocationField(std::string const& countryName, size_t index)
    {
        std::vector<std::string> const parts = SplitCsvFields(countryName);
        if (index >= parts.size())
            return "";

        // IP2Location writes "-" where it has no data.
        return parts[index] == "-" ? "" : parts[index];
    }

    // Whether both IPs sit in the same place, to the depth the config asks for.
    // Only a positive match waives the lock, so anything unknown - no database,
    // IPv6, an unassigned range, or a region asked of a DB1 database that has
    // none - counts as a mismatch and leaves the escalation in place.
    bool SameLocation(std::string const& a, std::string const& b, uint32 level)
    {
        if (level == BAN_SKIP_LOCATION_NONE)
            return false;

        IpLocationRecord const* locA = sIPLocation->GetLocationRecord(a);
        IpLocationRecord const* locB = sIPLocation->GetLocationRecord(b);
        if (!locA || !locB)
            return false;

        std::string const country = LocationField(locA->CountryName, 0);
        if (country.empty() || country != LocationField(locB->CountryName, 0))
            return false;

        if (level < BAN_SKIP_LOCATION_REGION)
            return true;

        std::string const region = LocationField(locA->CountryName, 1);
        return !region.empty() && region == LocationField(locB->CountryName, 1);
    }

// The flag and the place name only feed the Discord note; the server log
// stays IP-only.
#ifdef HAS_CHAT_TRANSMITTER
    // Flag emoji for a 2-letter ISO country code ("br" -> BR regional indicator
    // pair), empty for anything else. IP2Location stores the code lowercase.
    std::string CountryFlag(std::string const& code)
    {
        if (code.size() != 2)
            return "";

        std::string flag;
        for (char c : code)
        {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c < 'a' || c > 'z')
                return "";

            // U+1F1E6 (regional indicator A) + offset, encoded as UTF-8.
            uint32 const cp = 0x1F1E6 + static_cast<uint32>(c - 'a');
            flag += static_cast<char>(0xF0 | (cp >> 18));
            flag += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            flag += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            flag += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return flag;
    }

    // "Australia,Queensland,Brisbane" -> "Brisbane, Australia".
    std::string PlaceName(std::string const& countryName)
    {
        std::string const country = LocationField(countryName, 0);
        if (country.empty())
            return "";

        std::string const city = LocationField(countryName, 2);
        if (city.empty() || city == country)
            return country;

        return city + ", " + country;
    }

    // "1.2.3.4" -> "1.2.3.4 <flag> Brisbane, Australia" for the Discord note;
    // the server log keeps the bare IP, since a log file is grepped by address
    // and the emoji do not survive every viewer. The bare IP is also returned
    // when the location cannot be resolved: geolocation needs IPLocationFile
    // pointed at an IP2Location CSV, covers IPv4 only, and leaves reserved
    // ranges unassigned.
    std::string DescribeIp(std::string const& ip)
    {
        if (!EnhancedSupport::GetDormantLoginShowCountry())
            return ip;

        IpLocationRecord const* location = sIPLocation->GetLocationRecord(ip);
        if (!location)
            return ip;

        std::string const place = PlaceName(location->CountryName);
        if (place.empty())
            return ip;

        std::string const flag = CountryFlag(location->CountryCode);
        if (flag.empty())
            return Acore::StringFormat("{} ({})", ip, place);

        return Acore::StringFormat("{} {} {}", ip, flag, place);
    }
#endif
}

namespace EnhancedSupport
{
    void LoadDormantLoginConfig()
    {
        _dormantDays.store(sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.Days", 0));
        _dormantIpMaskBits.store(std::min<uint32>(sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.IpMaskBits", 24), 32));
        _dormantBanMinutes.store(sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.BanMinutes", 0));
        _dormantShowCountry.store(sConfigMgr->GetOption<bool>("EnhancedSupport.DormantLogin.ShowCountry", true));
        _dormantBanSkipLocation.store(std::min<uint32>(
            sConfigMgr->GetOption<uint32>("EnhancedSupport.DormantLogin.BanSkipLocation", 0), BAN_SKIP_LOCATION_REGION));
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

    bool GetDormantLoginShowCountry()
    {
        return _dormantShowCountry.load(std::memory_order_relaxed);
    }

    uint32 GetDormantLoginBanSkipLocation()
    {
        return _dormantBanSkipLocation.load(std::memory_order_relaxed);
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
                    // A returning owner usually reconnects from the same country
                    // on a new ISP address, a thief rarely does. Where the
                    // config trusts that, the report still goes out but the
                    // account is left alone.
                    uint32 const skipLevel = EnhancedSupport::GetDormantLoginBanSkipLocation();
                    bool const sameLocation = SameLocation(lastIp, ip, skipLevel);
                    uint32 const banMinutes = sameLocation ? 0 : EnhancedSupport::GetDormantLoginBanMinutes();

                    if (banMinutes > 0)
                    {
                        std::lock_guard<std::mutex> guard(_pendingBansMutex);
                        // emplace: a relog while locked keeps the original deadline
                        _pendingBans.emplace(accountId, now + uint64(banMinutes) * MINUTE);
                        _pendingBanCount.store(static_cast<uint32>(_pendingBans.size()), std::memory_order_relaxed);
                    }

                    // Named in both reports so a GM can tell a waived lock from
                    // a realm that never had the escalation enabled.
                    std::string const matchedOn = skipLevel >= BAN_SKIP_LOCATION_REGION
                        ? "country and region" : "country";

                    uint64 const inactiveDays = (now - lastSeen) / DAY;
                    LOG_INFO("module.enhancedsupport",
                        "DormantLogin: account {} ({}) logged in from {} after {} day(s) inactive - last seen {} from {}{}",
                        username, accountId, ip, inactiveDays,
                        lastSeenText.empty() ? "unknown" : lastSeenText,
                        lastIp.empty() ? "unknown IP" : lastIp,
                        banMinutes > 0
                            ? Acore::StringFormat(" - account locked, ban in {} minute(s)", banMinutes)
                            : sameLocation
                                ? Acore::StringFormat(" - lock skipped, same {} as the last-known IP", matchedOn)
                                : "");

#ifdef HAS_CHAT_TRANSMITTER
                    std::string const note = Acore::StringFormat(
                        "⚠️ **Dormant account login** — **{}** (account {})\n"
                        "🌐 New IP: {} | inactive for {} day(s)\n"
                        "🕓 Last seen: {} from {}\n"
                        "{}",
                        username, accountId, DescribeIp(ip), inactiveDays,
                        lastSeenText.empty() ? "unknown" : lastSeenText,
                        lastIp.empty() ? "unknown IP" : DescribeIp(lastIp),
                        banMinutes > 0
                            ? Acore::StringFormat(
                                "🔒 Mail, trades and auction house are locked; the account will be "
                                "permanently banned in {} minute(s). Unban after verifying the owner.", banMinutes)
                            : sameLocation
                                ? Acore::StringFormat(
                                    "🔎 Detection only: same {} as the last-known IP, so the lock was "
                                    "skipped. Verify whether the owner returned.", matchedOn)
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
            ChatHandler handler(player->GetSession());
            handler.SendSysMessage("|cffff0000" + handler.GetAcoreString(LANG_ES_DORMANT_TARGET_NO_TRADE) + "|r");
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
