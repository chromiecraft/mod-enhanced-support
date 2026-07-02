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
#include "AuctionHouseMgr.h"
#include "BanMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "Group.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SocialMgr.h"
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
    constexpr char const* INVITE_FILTER_BAN_REASON = "Invite filter: party invite spam";

    bool _enabled = true;
    uint8 _mailFilterAction = MAIL_FILTER_DISABLED;
    std::string _mailFilterMessage;
    std::string _mailFilterBanAuthor;
    std::vector<std::string> _mailFilterKeywords;

    // Email substrings matched against an account's email when one of its characters
    // is created. Log-only; surfaces bot/gold-seller accounts that register with
    // recognizable email patterns. Stored lowercased, like the mail keywords.
    std::vector<std::string> _emailPatterns;

    // Master switch for the email-pattern check. Defaults on, so the check is governed
    // by the pattern list unless explicitly disabled here.
    bool _emailFilterEnabled = true;

    // When set, mail between two characters on the same account (sending to your
    // own alt) skips the keyword and high-value checks entirely.
    bool _mailSkipSameAccount = false;

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

    // Party-invite spam filter: blocks a sender's group invites once more than
    // RateCount of them are fired within RateSeconds. Has its own action scale.
    // Only inviters at or below MaxLevel are watched (0 = every level). Invites to
    // a guildmate or to someone who has the inviter friended are never counted.
    uint8 _inviteFilterAction = MAIL_FILTER_DISABLED;
    std::string _inviteFilterMessage;
    uint8 _inviteMaxLevel = 0;
    uint32 _inviteRateCount = 0;
    uint32 _inviteRateSeconds = 0;

    // Mail carrying at least this much money (in copper) is logged (and relayed
    // to Discord). 0 disables the check. Parsed from a g/s/c money string.
    uint32 _goldFilterThresholdCopper = 0;

    // Which loot sources the loot level-gap check watches. A bitmask of the
    // LootSourceFlag bits below; set bits combine. 0 watches every source.
    enum LootSourceFlag : uint32
    {
        LOOT_SOURCE_CREATURE   = 0x01,
        LOOT_SOURCE_GAMEOBJECT = 0x02,
        LOOT_SOURCE_CONTAINER  = 0x04,
        LOOT_SOURCE_CORPSE     = 0x08,
        LOOT_SOURCE_PLAYER     = 0x10,
    };

    // Looting an item whose RequiredLevel exceeds the looter's level by at least
    // this many levels is logged (and relayed to Discord). 0 disables the check.
    // Surfaces low-level characters pulling high-level gear from world chests etc.
    uint32 _lootFilterLevelGap = 0;

    // Bitmask of LootSourceFlag values restricting which loot sources the check
    // watches. 0 watches every source (including sources of an unrecognised type).
    uint32 _lootFilterSources = 0;

    // Restricts the loot check to looters at or below this level. 0 means no cap
    // (applies to every level). Lets the check target only low-level characters.
    uint8 _lootFilterMaxLevel = 0;

    // Coalesce a looter's flagged loot into one Discord notification, flushed once
    // no further item has arrived for this many seconds (so a multi-item container
    // sends a single message). 0 sends one notification per item. Discord-only; the
    // server log keeps one line per item regardless.
    uint32 _lootBatchSeconds = 0;

    // Surfaces low-quality items auctioned for an unreasonable price - a channel
    // sometimes used to move value between characters under the guise of a normal
    // sale. Log-only; the auction is never blocked. An auction is flagged when its
    // item quality is at or below MaxQuality and its price is at or above MinPrice.
    // MaxQuality is an item quality (0 grey, 1 white, 2 green, ...); < 0 disables it.
    // Grey items have their own rules, independent of MaxQuality/MinPrice: AlwaysLogGrey
    // flags every grey item at any price, and GreyMinPrice flags grey items at/above a
    // dedicated price. Either works even when MaxQuality is off.
    int32 _auctionFilterMaxQuality = -1;
    uint32 _auctionFilterMinPriceCopper = 0;     // price (copper) at/above which a match is flagged
    bool _auctionFilterAlwaysLogGrey = false;    // flag every grey item, ignoring price
    uint32 _auctionFilterGreyMinPriceCopper = 0; // grey-specific price (copper) at/above which to flag
    bool _auctionFilterOnListing = false;        // also flag suspicious listings, not just completed sales
    uint32 _auctionBatchSeconds = 0;             // coalesce a seller's flagged auctions into one Discord notice; 0 = one per item

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

    // Reads a config option as a money amount: a g/s/c string ("100g", "50g 30s")
    // like the .send money command, or a bare number of copper. Returns 0 for a
    // non-positive or unparseable value.
    uint32 ParseMoneyOption(std::string const& name)
    {
        std::string const raw = ToLowerAscii(sConfigMgr->GetOption<std::string>(name, "0"));
        Optional<int32> const parsed = raw.find_first_of("gsc") != std::string::npos
            ? MoneyStringToMoney(raw)
            : Acore::StringTo<int32>(raw);
        if (!parsed)
        {
            LOG_WARN("module.enhancedsupport", "Could not parse {} = \"{}\"; treating as 0.", name, raw);
            return 0;
        }

        return *parsed > 0 ? static_cast<uint32>(*parsed) : 0;
    }

    // Folds common leetspeak/look-alike substitutions back to letters so that
    // digit-evaded spam (".c0m", "g0ld", "dotn3t") reads as its plain form. Input
    // is expected lowercased. Used only by the level-gated aggressive pass, where
    // the extra false-positive risk of digit folding is bounded; the strict pass
    // keeps matching the text verbatim.
    std::string FoldConfusables(std::string const& input)
    {
        std::string out;
        out.reserve(input.size());
        for (char c : input)
        {
            switch (c)
            {
                case '0': out.push_back('o'); break;
                case '1': out.push_back('i'); break;
                case '3': out.push_back('e'); break;
                case '4': out.push_back('a'); break;
                case '5': out.push_back('s'); break;
                case '7': out.push_back('t'); break;
                case '8': out.push_back('b'); break;
                case '9': out.push_back('g'); break;
                case '$': out.push_back('s'); break;
                case '@': out.push_back('a'); break;
                case '|': out.push_back('i'); break;
                default:  out.push_back(c);   break;
            }
        }

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

    // Returns the first email pattern contained in the address (case-insensitive),
    // or empty if none match.
    std::string FindMatchingEmailPattern(std::string const& email)
    {
        std::string const haystack = ToLowerAscii(email);
        for (std::string const& pattern : _emailPatterns)
        {
            if (haystack.find(pattern) != std::string::npos)
                return pattern;
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
        static constexpr std::array<std::string_view, 10> markers =
        {
            "http", "www", "wvvw", "web", ".com", ".net", ".org", "dotcom", "dotnet", "dotorg"
        };

        for (std::string_view marker : markers)
        {
            if (collapsed.find(marker) != std::string::npos)
                return true;
        }

        return false;
    }

    // Aggressive pass shared by the mail and chat filters: catches keywords evaded
    // by spacing and by look-alike character substitutions, but only for a low-level
    // sender whose text also carries a URL marker. Both signals are required to keep
    // despaced normal phrases from matching. Returns the matched keyword, or empty.
    std::string FindAggressiveMatch(Player* player, std::string const& text)
    {
        if (_aggressiveMaxLevel == 0 || player->GetLevel() > _aggressiveMaxLevel)
            return {};

        std::string const collapsed = FoldConfusables(StripWhitespace(ToLowerAscii(text)));
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

    // Per-inviter timestamps of recent group invites, for the rate check. Pruned
    // on each invite and cleared on logout, so it stays small.
    std::unordered_map<ObjectGuid, std::deque<time_t>> _recentInvites;

    void ClearInviteWindow(ObjectGuid guid)
    {
        _recentInvites.erase(guid);
    }

    // Records an invite from `guid` now, drops entries older than the rate window,
    // and returns how many invites remain in the window (including this one).
    uint32 RecordInviteAndCount(ObjectGuid guid)
    {
        time_t const now = GameTime::GetGameTime().count();
        std::deque<time_t>& history = _recentInvites[guid];

        while (!history.empty() && now - history.front() > static_cast<time_t>(_inviteRateSeconds))
            history.pop_front();

        history.push_back(now);
        return static_cast<uint32>(history.size());
    }

    char const* QualityName(uint32 quality)
    {
        switch (quality)
        {
            case ITEM_QUALITY_POOR:     return "grey";
            case ITEM_QUALITY_NORMAL:   return "white";
            case ITEM_QUALITY_UNCOMMON: return "green";
            case ITEM_QUALITY_RARE:     return "blue";
            case ITEM_QUALITY_EPIC:     return "purple";
            default:                    return "other";
        }
    }

#ifdef HAS_CHAT_TRANSMITTER
    // Buffered underlevel-loot items for one looter, coalesced into a single
    // Discord notification. Holds only captured values (no live Player*), since
    // it is flushed up to _lootBatchSeconds after the loot happened.
    struct LootBatchItem
    {
        std::string name;
        uint32 itemId;
        uint32 count;
        uint32 requiredLevel;
        uint32 gap;
    };

    struct LootBatch
    {
        ObjectGuid sourceGuid;   // loot source; a change flushes the prior batch
        std::string header;      // looter line (name/account/IP/group)
        std::string sourceLine;  // source + location (+ teleport)
        uint32 playerLevel = 0;
        std::vector<LootBatchItem> items;
        time_t lastUpdate = 0;
    };

    std::unordered_map<ObjectGuid /*looter*/, LootBatch> _lootBatches;

    void EmitLootBatch(LootBatch const& batch)
    {
        if (batch.items.empty())
            return;

        bool const multi = batch.items.size() > 1;

        std::string itemsBlock;
        for (LootBatchItem const& it : batch.items)
        {
            if (!itemsBlock.empty())
                itemsBlock.push_back('\n');

            itemsBlock += Acore::StringFormat(
                "🎁 [{}](https://wowgaming.altervista.org/aowow/?item={}) (id {}) x{}",
                it.name, it.itemId, it.itemId, it.count);

            // For a single item the levels live in the headline; per-item gaps
            // only make sense when several items share one notification.
            if (multi)
                itemsBlock += Acore::StringFormat(" — requires {} (gap {})", it.requiredLevel, it.gap);
        }

        std::string headline;
        if (multi)
            headline = Acore::StringFormat(
                "📦 **Underlevel loot** — looter level {}, {} items",
                batch.playerLevel, batch.items.size());
        else
            headline = Acore::StringFormat(
                "📦 **Underlevel loot** — requires level {}, looter level {} (gap {})",
                batch.items.front().requiredLevel, batch.playerLevel, batch.items.front().gap);

        std::string note = Acore::StringFormat(
            "{}\n{}\n📍 Source: {}\n{}",
            headline, batch.header, batch.sourceLine, itemsBlock);
        sChatTransmitter->QueueNotification("ItemLoot", note);
    }

    // Adds one flagged item to the looter's batch (or emits immediately when
    // batching is off). A change of loot source flushes the previous batch first.
    void QueueOrBatchLoot(ObjectGuid looter, ObjectGuid sourceGuid, std::string header,
        std::string sourceLine, uint32 playerLevel, LootBatchItem item)
    {
        if (_lootBatchSeconds == 0)
        {
            LootBatch single;
            single.header = std::move(header);
            single.sourceLine = std::move(sourceLine);
            single.playerLevel = playerLevel;
            single.items.push_back(std::move(item));
            EmitLootBatch(single);
            return;
        }

        LootBatch& batch = _lootBatches[looter];
        if (!batch.items.empty() && batch.sourceGuid != sourceGuid)
        {
            EmitLootBatch(batch);
            batch = LootBatch{};
        }

        batch.sourceGuid = sourceGuid;
        batch.header = std::move(header);
        batch.sourceLine = std::move(sourceLine);
        batch.playerLevel = playerLevel;
        batch.items.push_back(std::move(item));
        batch.lastUpdate = GameTime::GetGameTime().count();
    }

    void FlushLooterLoot(ObjectGuid looter)
    {
        auto it = _lootBatches.find(looter);
        if (it == _lootBatches.end())
            return;

        EmitLootBatch(it->second);
        _lootBatches.erase(it);
    }

    // Emits and drops batches that have gone _lootBatchSeconds without a new item.
    void FlushDueLootBatches(time_t now)
    {
        for (auto it = _lootBatches.begin(); it != _lootBatches.end(); )
        {
            if (now - it->second.lastUpdate >= static_cast<time_t>(_lootBatchSeconds))
            {
                EmitLootBatch(it->second);
                it = _lootBatches.erase(it);
            }
            else
                ++it;
        }
    }

    // Buffered flagged auctions for one seller, coalesced into a single Discord
    // notification (e.g. when a seller posts many items at once). Holds only
    // captured values, since it is flushed up to _auctionBatchSeconds later.
    struct AuctionBatchItem
    {
        std::string itemName;
        uint32 itemId;
        uint32 count;
        uint32 quality;
        uint32 price;
        uint32 vendorValue;
        std::string buyer;  // formatted buyer line; empty for listings
    };

    struct AuctionBatch
    {
        std::string event;   // "listing"/"sale"; a change flushes the prior batch
        std::string header;  // seller line (name/GUID/account/IP)
        std::vector<AuctionBatchItem> items;
        time_t lastUpdate = 0;
    };

    std::unordered_map<ObjectGuid /*seller*/, AuctionBatch> _auctionBatches;

    void EmitAuctionBatch(AuctionBatch const& batch)
    {
        if (batch.items.empty())
            return;

        bool const multi = batch.items.size() > 1;

        std::string itemsBlock;
        for (AuctionBatchItem const& it : batch.items)
        {
            if (!itemsBlock.empty())
                itemsBlock.push_back('\n');

            itemsBlock += Acore::StringFormat(
                "🏷️ [{}](https://wowgaming.altervista.org/aowow/?item={}) (id {}, {}) x{} — {} (vendor value {})",
                it.itemName, it.itemId, it.itemId, QualityName(it.quality), it.count,
                EnhancedSupport::FormatMoney(it.price), EnhancedSupport::FormatMoney(it.vendorValue));

            if (!it.buyer.empty())
                itemsBlock += Acore::StringFormat("\n   🛒 Buyer: {}", it.buyer);
        }

        std::string const headline = multi
            ? Acore::StringFormat("🪙 **Suspicious auctions ({})** — {} items", batch.event, batch.items.size())
            : Acore::StringFormat("🪙 **Suspicious auction ({})**", batch.event);

        std::string note = Acore::StringFormat("{}\n{}\n{}", headline, batch.header, itemsBlock);
        // Reuse the ChatFilter notification alias so this routes to an already-configured
        // Discord channel without needing a new mod-chat-transmitter config entry.
        sChatTransmitter->QueueNotification("ChatFilter", note);
    }

    // Adds one flagged auction to the seller's batch (or emits immediately when
    // batching is off). A change of event kind flushes the previous batch first, so
    // listings and sales never share a notification.
    void QueueOrBatchAuction(ObjectGuid seller, std::string event, std::string header, AuctionBatchItem item)
    {
        if (_auctionBatchSeconds == 0)
        {
            AuctionBatch single;
            single.event = std::move(event);
            single.header = std::move(header);
            single.items.push_back(std::move(item));
            EmitAuctionBatch(single);
            return;
        }

        AuctionBatch& batch = _auctionBatches[seller];
        if (!batch.items.empty() && batch.event != event)
        {
            EmitAuctionBatch(batch);
            batch = AuctionBatch{};
        }

        batch.event = std::move(event);
        batch.header = std::move(header);
        batch.items.push_back(std::move(item));
        batch.lastUpdate = GameTime::GetGameTime().count();
    }

    // Emits and drops batches that have gone _auctionBatchSeconds without a new item.
    void FlushDueAuctionBatches(time_t now)
    {
        for (auto it = _auctionBatches.begin(); it != _auctionBatches.end(); )
        {
            if (now - it->second.lastUpdate >= static_cast<time_t>(_auctionBatchSeconds))
            {
                EmitAuctionBatch(it->second);
                it = _auctionBatches.erase(it);
            }
            else
                ++it;
        }
    }
#endif

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

    // True when the auction filter is configured to flag anything at all.
    bool AuctionFilterActive()
    {
        return _auctionFilterMaxQuality >= 0 || _auctionFilterAlwaysLogGrey || _auctionFilterGreyMinPriceCopper != 0;
    }

    // True when an auction of `proto` at `price` should be flagged. Grey items are
    // checked against their own rules first (the always-on switch and the grey-specific
    // price), then any item is checked against the general MaxQuality / MinPrice.
    bool IsSuspiciousAuction(ItemTemplate const* proto, uint32 price)
    {
        if (!proto)
            return false;

        if (proto->Quality == ITEM_QUALITY_POOR)
        {
            if (_auctionFilterAlwaysLogGrey)
                return true;

            if (_auctionFilterGreyMinPriceCopper != 0 && price >= _auctionFilterGreyMinPriceCopper)
                return true;
        }

        if (_auctionFilterMaxQuality < 0)
            return false;

        if (static_cast<int32>(proto->Quality) > _auctionFilterMaxQuality)
            return false;

        return price >= _auctionFilterMinPriceCopper;
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

    // Email patterns live in the auth DB (enhanced_support_email_patterns) and are
    // cached here so the character-create hook doesn't hit the DB for the list.
    void LoadEmailPatterns()
    {
        _emailPatterns.clear();

        QueryResult result = LoginDatabase.Query("SELECT pattern FROM enhanced_support_email_patterns");
        if (!result)
            return;

        do
        {
            std::string pattern = NormalizeKeyword(result->Fetch()[0].Get<std::string>());
            if (!pattern.empty())
                _emailPatterns.push_back(pattern);
        } while (result->NextRow());
    }

    std::vector<std::string> const& GetEmailPatterns()
    {
        return _emailPatterns;
    }

    bool GetEmailFilterEnabled()
    {
        return _emailFilterEnabled;
    }

    bool HasEmailPattern(std::string const& normalized)
    {
        return std::find(_emailPatterns.begin(), _emailPatterns.end(), normalized) != _emailPatterns.end();
    }

    void AddEmailPattern(std::string const& normalized)
    {
        std::string escaped = normalized;
        LoginDatabase.EscapeString(escaped);
        LoginDatabase.Execute("INSERT IGNORE INTO enhanced_support_email_patterns (pattern) VALUES ('{}')", escaped);
        LoadEmailPatterns();
    }

    void RemoveEmailPattern(std::string const& normalized)
    {
        std::string escaped = normalized;
        LoginDatabase.EscapeString(escaped);
        LoginDatabase.Execute("DELETE FROM enhanced_support_email_patterns WHERE pattern = '{}'", escaped);
        LoadEmailPatterns();
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

    bool GetMailSkipSameAccount()
    {
        return _mailSkipSameAccount;
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

    uint8 GetInviteFilterAction()
    {
        return _inviteFilterAction;
    }

    std::string_view GetInviteFilterActionName()
    {
        return GetFilterActionName(_inviteFilterAction);
    }

    uint8 GetInviteMaxLevel()
    {
        return _inviteMaxLevel;
    }

    uint32 GetInviteRateCount()
    {
        return _inviteRateCount;
    }

    uint32 GetInviteRateSeconds()
    {
        return _inviteRateSeconds;
    }

    uint32 GetGoldFilterThreshold()
    {
        return _goldFilterThresholdCopper;
    }

    uint32 GetLootFilterLevelGap()
    {
        return _lootFilterLevelGap;
    }

    uint32 GetLootFilterSources()
    {
        return _lootFilterSources;
    }

    uint8 GetLootFilterMaxLevel()
    {
        return _lootFilterMaxLevel;
    }

    uint32 GetLootBatchSeconds()
    {
        return _lootBatchSeconds;
    }

    int32 GetAuctionFilterMaxQuality()
    {
        return _auctionFilterMaxQuality;
    }

    uint32 GetAuctionFilterMinPrice()
    {
        return _auctionFilterMinPriceCopper;
    }

    bool GetAuctionFilterAlwaysLogGrey()
    {
        return _auctionFilterAlwaysLogGrey;
    }

    uint32 GetAuctionFilterGreyMinPrice()
    {
        return _auctionFilterGreyMinPriceCopper;
    }

    bool GetAuctionFilterOnListing()
    {
        return _auctionFilterOnListing;
    }

    uint32 GetAuctionBatchSeconds()
    {
        return _auctionBatchSeconds;
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
        _mailSkipSameAccount = sConfigMgr->GetOption<bool>("EnhancedSupport.MailFilter.SkipSameAccount", false);

        _chatFilterAction = sConfigMgr->GetOption<uint8>("EnhancedSupport.ChatFilter.Action", MAIL_FILTER_DISABLED);
        _chatFilterMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.ChatFilter.Message",
            "Your message was blocked because it contains a prohibited keyword.");
        _aggressiveMaxLevel = sConfigMgr->GetOption<uint8>("EnhancedSupport.AggressiveMaxLevel", 0);

        _chatWindowSize = sConfigMgr->GetOption<uint32>("EnhancedSupport.ChatFilter.WindowSize", 0);
        _chatWindowSeconds = sConfigMgr->GetOption<uint32>("EnhancedSupport.ChatFilter.WindowSeconds", 0);

        _inviteFilterAction = sConfigMgr->GetOption<uint8>("EnhancedSupport.InviteFilter.Action", MAIL_FILTER_DISABLED);
        _inviteFilterMessage = sConfigMgr->GetOption<std::string>("EnhancedSupport.InviteFilter.Message",
            "Your party invite was blocked.");
        _inviteMaxLevel = sConfigMgr->GetOption<uint8>("EnhancedSupport.InviteFilter.MaxLevel", 0);
        _inviteRateCount = sConfigMgr->GetOption<uint32>("EnhancedSupport.InviteFilter.RateCount", 0);
        _inviteRateSeconds = sConfigMgr->GetOption<uint32>("EnhancedSupport.InviteFilter.RateSeconds", 0);

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
        _lootFilterSources = sConfigMgr->GetOption<uint32>("EnhancedSupport.LootFilter.Sources", 0);
        _lootFilterMaxLevel = sConfigMgr->GetOption<uint8>("EnhancedSupport.LootFilter.MaxLevel", 0);
        _lootBatchSeconds = sConfigMgr->GetOption<uint32>("EnhancedSupport.LootFilter.BatchSeconds", 0);

        _auctionFilterMaxQuality = sConfigMgr->GetOption<int32>("EnhancedSupport.AuctionFilter.MaxQuality", -1);
        _auctionFilterAlwaysLogGrey = sConfigMgr->GetOption<bool>("EnhancedSupport.AuctionFilter.AlwaysLogGrey", false);
        _auctionFilterOnListing = sConfigMgr->GetOption<bool>("EnhancedSupport.AuctionFilter.OnListing", false);
        _auctionFilterMinPriceCopper = ParseMoneyOption("EnhancedSupport.AuctionFilter.MinPrice");
        _auctionFilterGreyMinPriceCopper = ParseMoneyOption("EnhancedSupport.AuctionFilter.GreyMinPrice");

        _emailFilterEnabled = sConfigMgr->GetOption<bool>("EnhancedSupport.EmailFilter.Enable", true);
        _auctionBatchSeconds = sConfigMgr->GetOption<uint32>("EnhancedSupport.AuctionFilter.BatchSeconds", 0);

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
        EnhancedSupport::LoadEmailPatterns();

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

#ifdef HAS_CHAT_TRANSMITTER
        time_t const now = GameTime::GetGameTime().count();
        if (!_lootBatches.empty())
            FlushDueLootBatches(now);
        if (!_auctionBatches.empty())
            FlushDueAuctionBatches(now);
#endif
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

        // Sending to your own alt (same account) is never advertising or gold
        // selling, so skip both the keyword and high-value checks when enabled.
        if (_mailSkipSameAccount && IsSameAccountTransfer(player, receiverGuid))
            return true;

        // Log-only: flag unusually large gold transfers without blocking them.
        LogHighValueMail(player, receiverGuid, money, subject);

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

    // True if the receiver character belongs to the sender's account.
    static bool IsSameAccountTransfer(Player* sender, ObjectGuid receiverGuid)
    {
        uint32 const receiverAccountId = sCharacterCache->GetCharacterAccountIdByGuid(receiverGuid);
        return receiverAccountId != 0 && receiverAccountId == sender->GetSession()->GetAccountId();
    }

    static void LogHighValueMail(Player* player, ObjectGuid receiverGuid, uint32 money, std::string const& subject)
    {
        if (_goldFilterThresholdCopper == 0 || money < _goldFilterThresholdCopper)
            return;

        std::string const amount = EnhancedSupport::FormatMoney(money);
        std::string const threshold = EnhancedSupport::FormatMoney(_goldFilterThresholdCopper);
        std::string const receiverName = ResolveCharacterName(receiverGuid);

        LOG_INFO("module.enhancedsupport",
            "MailFilter: high-value mail from {} ({}) to {} ({}) - {} (threshold {}) | Account {} | IP {} | subject: \"{}\"",
            player->GetName(), player->GetGUID().GetCounter(),
            receiverName, receiverGuid.GetCounter(),
            amount, threshold,
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(), subject);

#ifdef HAS_CHAT_TRANSMITTER
        std::string note = Acore::StringFormat(
            "💰 **High-value mail** — {} (threshold {})\n"
            "👤 From: **{}** (GUID {}) | Account {} | IP {}\n"
            "📬 To: **{}** (GUID {})\n"
            "✉️ Subject: {}",
            amount, threshold,
            player->GetName(), player->GetGUID().GetCounter(),
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
            receiverName, receiverGuid.GetCounter(), subject);
        sChatTransmitter->QueueNotification("MailFilter", note);
#endif
    }
};

// Filters player chat against the same keyword list as the mail filter. SAY/YELL/
// EMOTE arrive via OnPlayerBeforeSendChatMessage, which can't abort the broadcast,
// so a matched message is blanked. Party and whisper chat arrive via the boolean
// OnPlayerCanUseChat overloads, which abort the broadcast outright when we return false.
class EnhancedSupportChatFilter : public PlayerScript
{
public:
    EnhancedSupportChatFilter() : PlayerScript("EnhancedSupportChatFilter", {
        PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        if (!FilterEnabled() || lang == LANG_ADDON || (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL && type != CHAT_MSG_EMOTE))
            return;

        // This hook runs before the message is broadcast but can't stop it, so
        // clear the text to keep the prohibited content from reaching others.
        if (FilterMessage(player, type, msg))
            msg.clear();
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* /*group*/) override
    {
        if (!FilterEnabled() || lang == LANG_ADDON || (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER))
            return true;

        // Returning false aborts the broadcast, so no need to blank the text.
        return !FilterMessage(player, type, msg);
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (!FilterEnabled() || lang == LANG_ADDON || type != CHAT_MSG_WHISPER)
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
            case CHAT_MSG_WHISPER:      return "whisper";
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
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_LOGOUT
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

        // Restrict to the configured loot sources (0 = watch every source).
        if (_lootFilterSources != 0 && !(src.typeFlag & _lootFilterSources))
            return;

        std::string const location = ResolveLocation(player);
        bool const inGroup = player->GetGroup() != nullptr;

        LOG_INFO("module.enhancedsupport",
            "LootFilter: {} ({}, level {}) looted {}x [{}] ({}, requires level {}, gap {}) from {} {} (entry {}, spawn {}) at {} | {} | Account {} | IP {}",
            player->GetName(), player->GetGUID().GetCounter(), playerLevel,
            count, proto->Name1, proto->ItemId, proto->RequiredLevel, gap,
            src.label, src.name, src.entry, src.spawnId, location,
            inGroup ? "in group" : "not in group",
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress());

#ifdef HAS_CHAT_TRANSMITTER
        std::string sourceLine = Acore::StringFormat("{} | 🗺️ {}", FormatDiscordSource(src), location);

        // No world spawn to go to (container in bags, corpse, player, despawned
        // source); give the GM a ready-to-paste teleport to the looter instead.
        if (!src.spawnId)
            sourceLine += Acore::StringFormat(
                "\n🧭 Looter at: `.go xyz {:.3f} {:.3f} {:.3f} {} {:.3f}`",
                player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                player->GetMapId(), player->GetOrientation());

        std::string header = Acore::StringFormat(
            "👤 **{}** (GUID {}) | Account {} | IP {} | {}",
            player->GetName(), player->GetGUID().GetCounter(),
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
            inGroup ? "👥 in group" : "🧍 solo");

        QueueOrBatchLoot(player->GetGUID(), lootguid, std::move(header), std::move(sourceLine),
            playerLevel, LootBatchItem{ proto->Name1, proto->ItemId, count, proto->RequiredLevel, gap });
#endif
    }

    void OnPlayerLogout(Player* player) override
    {
#ifdef HAS_CHAT_TRANSMITTER
        // Send any pending batch before the looter's data goes stale.
        FlushLooterLoot(player->GetGUID());
#endif
    }

private:
    struct LootSource
    {
        char const* label = "unknown source";
        std::string name;                // creature/object/container name; empty if unresolved
        uint32 entry = 0;                // template entry (item id for a container); 0 if unresolved
        ObjectGuid::LowType spawnId = 0; // DB spawn id; 0 for items and temporary objects
        uint32 typeFlag = 0;             // matching LootSourceFlag; 0 for an unrecognised source
        bool isCreature = false;
        bool isGameObject = false;
        bool isItem = false;             // a container item in the looter's bags
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
            src.typeFlag = LOOT_SOURCE_GAMEOBJECT;
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
            src.typeFlag = LOOT_SOURCE_CREATURE;
            src.label = "creature";
            if (Creature* creature = map->GetCreature(lootguid))
            {
                src.entry = creature->GetEntry();
                src.spawnId = creature->GetSpawnId();
                src.name = creature->GetName();
            }
        }
        else if (lootguid.IsItem())
        {
            src.isItem = true;
            src.typeFlag = LOOT_SOURCE_CONTAINER;
            src.label = "container";
            // A container is an item in the looter's own bags, not a world object.
            if (Item* container = player->GetItemByGuid(lootguid))
            {
                src.entry = container->GetEntry();
                if (ItemTemplate const* containerProto = container->GetTemplate())
                    src.name = containerProto->Name1;
            }
        }
        else if (lootguid.IsCorpse())
        {
            src.typeFlag = LOOT_SOURCE_CORPSE;
            src.label = "corpse";
        }
        else if (lootguid.IsPlayer())
        {
            src.typeFlag = LOOT_SOURCE_PLAYER;
            src.label = "player";
        }

        return src;
    }

    // "Zone - Subarea" for the looter's position, resolved the same way as the
    // .pinfo command: the area's parent zone is the broad zone, the area itself
    // the subarea (omitted when the player is directly in a top-level zone).
    static std::string ResolveLocation(Player* player)
    {
        LocaleConstant const locale = player->GetSession()->GetSessionDbcLocale();

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        if (!area)
            return "unknown";

        std::string zoneName = area->area_name[locale];
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(area->zone))
            return Acore::StringFormat("{} - {}", zone->area_name[locale], zoneName);

        return zoneName;
    }

#ifdef HAS_CHAT_TRANSMITTER
    // Discord "Source" line, linking creatures/objects/containers to their aowow
    // page by template entry. Other sources (corpses, players) carry no link.
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

        if (src.isItem && src.entry)
            return Acore::StringFormat(
                "{} [{} #{}](https://wowgaming.altervista.org/aowow/?item={})",
                src.label, src.name, src.entry, src.entry);

        return std::string(src.label);
    }
#endif
};

// Blocks a sender's group invites once they fire too many in a short window - the
// pattern of throwaway low-level characters mass-inviting players to spam. The
// core's invite handler drops the invite silently when this hook returns false.
class EnhancedSupportInviteFilter : public PlayerScript
{
public:
    EnhancedSupportInviteFilter() : PlayerScript("EnhancedSupportInviteFilter", {
        PLAYERHOOK_CAN_GROUP_INVITE,
        PLAYERHOOK_ON_LOGOUT
    }) { }

    bool OnPlayerCanGroupInvite(Player* player, std::string& membername) override
    {
        if (!_enabled || _inviteFilterAction == MAIL_FILTER_DISABLED || _inviteRateCount == 0 || _inviteRateSeconds == 0)
            return true;

        // Only watch low-level inviters (gold-seller bots are throwaway low-level
        // characters); 0 means watch every level.
        if (_inviteMaxLevel != 0 && player->GetLevel() > _inviteMaxLevel)
            return true;

        // Don't count invites to a guildmate, to someone who has the inviter
        // friended, or to someone on the same IP (same household / multiboxer) -
        // those are legitimate, not spam.
        if (Player* invitee = ObjectAccessor::FindPlayerByName(membername, false))
        {
            uint32 const guildId = player->GetGuildId();
            if (guildId != 0 && invitee->GetGuildId() == guildId)
                return true;

            PlayerSocial* social = invitee->GetSocial();
            if (social && social->HasFriend(player->GetGUID()))
                return true;

            if (invitee->GetSession() && invitee->GetSession()->GetRemoteAddress() == player->GetSession()->GetRemoteAddress())
                return true;
        }

        uint32 const count = RecordInviteAndCount(player->GetGUID());
        if (count <= _inviteRateCount)
            return true;

        LOG_INFO("module.enhancedsupport",
            "InviteFilter: blocked group invite from {} ({}, level {}) to {} - {} invites in {}s (limit {}), action {} | Account {} | IP {}",
            player->GetName(), player->GetGUID().GetCounter(), static_cast<uint32>(player->GetLevel()),
            membername, count, _inviteRateSeconds, _inviteRateCount, static_cast<uint32>(_inviteFilterAction),
            player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress());

#ifdef HAS_CHAT_TRANSMITTER
        {
            std::string note = Acore::StringFormat(
                "🚫 **Invite spam blocked** — {} invites in {}s (limit {}), action `{}`\n"
                "👤 From: **{}** (GUID {}, level {}) | Account {} | IP {}\n"
                "📨 Target: {}",
                count, _inviteRateSeconds, _inviteRateCount, EnhancedSupport::GetInviteFilterActionName(),
                player->GetName(), player->GetGUID().GetCounter(), static_cast<uint32>(player->GetLevel()),
                player->GetSession()->GetAccountId(), player->GetSession()->GetRemoteAddress(),
                membername);
            sChatTransmitter->QueueNotification("ChatFilter", note);
        }
#endif

        ApplyFilterAction(player, _inviteFilterAction, _inviteFilterMessage,
            "EnhancedSupport: party invite spam", INVITE_FILTER_BAN_REASON);

        return false;
    }

    void OnPlayerLogout(Player* player) override
    {
        ClearInviteWindow(player->GetGUID());
    }
};

// Logs (without blocking) auctions of low-quality items put up for an unreasonable
// price - a way to move value between characters dressed up as a normal sale. Flags
// completed sales (where both parties and the paid price are known) and, optionally,
// the listings themselves.
class EnhancedSupportAuctionFilter : public AuctionHouseScript
{
public:
    EnhancedSupportAuctionFilter() : AuctionHouseScript("EnhancedSupportAuctionFilter", {
        AUCTIONHOUSEHOOK_ON_AUCTION_ADD,
        AUCTIONHOUSEHOOK_ON_AUCTION_SUCCESSFUL
    }) { }

    void OnAuctionAdd(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        if (!_enabled || !_auctionFilterOnListing || !AuctionFilterActive() || !entry)
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry->item_template);

        // Asking price is the buyout when set, otherwise the starting bid.
        uint32 const price = entry->buyout ? entry->buyout : entry->startbid;
        if (!IsSuspiciousAuction(proto, price))
            return;

        Report("listing", proto, entry->item_template, entry->itemCount, price, entry->owner, ObjectGuid::Empty);
    }

    void OnAuctionSuccessful(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
    {
        if (!_enabled || !AuctionFilterActive() || !entry)
            return;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry->item_template);

        // On a completed sale (buyout or won bid) entry->bid holds the price paid.
        if (!IsSuspiciousAuction(proto, entry->bid))
            return;

        Report("sale", proto, entry->item_template, entry->itemCount, entry->bid, entry->owner, entry->bidder);
    }

private:
    struct PartyInfo
    {
        std::string name;
        uint32 account = 0;
        std::string ip;  // resolvable only while the character is online
    };

    static PartyInfo ResolveParty(ObjectGuid guid)
    {
        PartyInfo info;
        if (!sCharacterCache->GetCharacterNameByGuid(guid, info.name) || info.name.empty())
            info.name = "Unknown";

        info.account = sCharacterCache->GetCharacterAccountIdByGuid(guid);

        if (Player* player = ObjectAccessor::FindPlayer(guid))
            info.ip = player->GetSession()->GetRemoteAddress();
        else
            info.ip = "offline";

        return info;
    }

    static void Report(char const* event, ItemTemplate const* proto, uint32 itemId, uint32 count,
        uint32 price, ObjectGuid seller, ObjectGuid buyer)
    {
        std::string const itemName = proto ? proto->Name1 : "Unknown";
        uint32 const quality = proto ? proto->Quality : 0;
        uint32 const vendorValue = proto ? proto->SellPrice * count : 0;

        PartyInfo const sellerInfo = ResolveParty(seller);

        std::string buyerLine;  // empty for listings (buyer unknown)
        if (buyer)
        {
            PartyInfo const buyerInfo = ResolveParty(buyer);
            buyerLine = Acore::StringFormat("{} (GUID {}, account {}, IP {})",
                buyerInfo.name, buyer.GetCounter(), buyerInfo.account, buyerInfo.ip);
        }

        LOG_INFO("module.enhancedsupport",
            "AuctionFilter: suspicious {} - {}x [{}] (id {}, {}) for {} (vendor value {}) | Seller: {} ({}, account {}, IP {}) | Buyer: {}",
            event, count, itemName, itemId, QualityName(quality),
            EnhancedSupport::FormatMoney(price), EnhancedSupport::FormatMoney(vendorValue),
            sellerInfo.name, seller.GetCounter(), sellerInfo.account, sellerInfo.ip,
            buyerLine.empty() ? "n/a" : buyerLine);

#ifdef HAS_CHAT_TRANSMITTER
        std::string header = Acore::StringFormat("👤 Seller: **{}** (GUID {}, account {}, IP {})",
            sellerInfo.name, seller.GetCounter(), sellerInfo.account, sellerInfo.ip);

        QueueOrBatchAuction(seller, event, std::move(header),
            AuctionBatchItem{ itemName, itemId, count, quality, price, vendorValue, std::move(buyerLine) });
#endif
    }
};

// Logs (without blocking) when a newly created character's account email matches a
// configured substring pattern - surfaces bot/gold-seller accounts that register with
// recognizable email patterns. Patterns live in the auth DB and are managed like the
// mail keywords. Log-only; relayed to Discord via the ChatFilter alias.
class EnhancedSupportEmailFilter : public PlayerScript
{
public:
    EnhancedSupportEmailFilter() : PlayerScript("EnhancedSupportEmailFilter", {
        PLAYERHOOK_ON_CREATE
    }) { }

    void OnPlayerCreate(Player* player) override
    {
        if (!_enabled || !_emailFilterEnabled || _emailPatterns.empty())
            return;

        uint32 const accountId = player->GetSession()->GetAccountId();

        // Character creation is infrequent, so a single synchronous read here is fine.
        LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_EMAIL_BY_ID);
        stmt->SetData(0, accountId);
        PreparedQueryResult result = LoginDatabase.Query(stmt);
        if (!result)
            return;

        std::string const email = result->Fetch()[0].Get<std::string>();
        if (email.empty())
            return;

        std::string const matched = FindMatchingEmailPattern(email);
        if (matched.empty())
            return;

        LOG_INFO("module.enhancedsupport",
            "EmailFilter: new character {} ({}) on account {} matched email pattern '{}' | email {} | IP {}",
            player->GetName(), player->GetGUID().GetCounter(), accountId, matched, email,
            player->GetSession()->GetRemoteAddress());

#ifdef HAS_CHAT_TRANSMITTER
        std::string note = Acore::StringFormat(
            "📧 **Suspicious account email** — pattern `{}`\n"
            "👤 New character: **{}** (GUID {}) | Account {} | IP {}\n"
            "✉️ Email: {}",
            matched, player->GetName(), player->GetGUID().GetCounter(),
            accountId, player->GetSession()->GetRemoteAddress(), email);
        sChatTransmitter->QueueNotification("ChatFilter", note);
#endif
    }
};

void AddEnhancedSupportScripts()
{
    new EnhancedSupportWorldScript();
    new EnhancedSupportMailFilter();
    new EnhancedSupportChatFilter();
    new EnhancedSupportLootFilter();
    new EnhancedSupportInviteFilter();
    new EnhancedSupportAuctionFilter();
    new EnhancedSupportEmailFilter();
}
