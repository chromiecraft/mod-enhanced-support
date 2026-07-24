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
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectGuid.h"
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
            { "bans",          HandleListBansCommand,          SEC_GAMEMASTER, Console::Yes },
            { "keywords",      HandleListKeywordsCommand,      SEC_GAMEMASTER, Console::Yes },
            { "emailpatterns", HandleListEmailPatternsCommand, SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable keywordTable =
        {
            { "add",    HandleKeywordAddCommand,    SEC_ADMINISTRATOR, Console::Yes },
            { "remove", HandleKeywordRemoveCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable emailPatternTable =
        {
            { "add",    HandleEmailPatternAddCommand,    SEC_ADMINISTRATOR, Console::Yes },
            { "remove", HandleEmailPatternRemoveCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable arenaTable =
        {
            { "matches", HandleArenaMatchesCommand, SEC_GAMEMASTER, Console::Yes },
            { "team",    HandleArenaTeamCommand,    SEC_GAMEMASTER, Console::Yes },
            { "check",   HandleArenaCheckCommand,   SEC_GAMEMASTER, Console::Yes },
            { "replay",  HandleArenaReplayCommand,  SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable supportTable =
        {
            { "info",         HandleInfoCommand,   SEC_GAMEMASTER,    Console::Yes },
            { "action",       HandleActionCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "reload",       HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "list",         listTable },
            { "keyword",      keywordTable },
            { "emailpattern", emailPatternTable },
            { "arena",        arenaTable },
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
        handler->PSendSysMessage("  Mail skip same-account: {}",
            EnhancedSupport::GetMailSkipSameAccount() ? "yes" : "no");
        handler->PSendSysMessage("  Chat filter action: {} ({})",
            static_cast<uint32>(EnhancedSupport::GetChatFilterAction()), EnhancedSupport::GetChatFilterActionName());

        uint8 const aggressiveMaxLevel = EnhancedSupport::GetAggressiveMaxLevel();
        if (aggressiveMaxLevel == 0)
            handler->PSendSysMessage("  Aggressive pass: disabled");
        else
            handler->PSendSysMessage("  Aggressive pass: enabled for level <= {}",
                static_cast<uint32>(aggressiveMaxLevel));

        uint32 const windowSize = EnhancedSupport::GetChatWindowSize();
        uint32 const windowSeconds = EnhancedSupport::GetChatWindowSeconds();
        if (windowSize < 2 || windowSeconds == 0 || aggressiveMaxLevel == 0)
            handler->PSendSysMessage("  Chat window pass: disabled");
        else
            handler->PSendSysMessage("  Chat window pass: last {} line(s) within {}s (level <= {})",
                windowSize, windowSeconds, static_cast<uint32>(aggressiveMaxLevel));

        handler->PSendSysMessage("  Keywords loaded: {}", EnhancedSupport::GetKeywords().size());
        handler->PSendSysMessage("  Email filter: {} ({} pattern(s) loaded)",
            EnhancedSupport::GetEmailFilterEnabled() ? "enabled" : "disabled",
            EnhancedSupport::GetEmailPatterns().size());

        uint8 const inviteAction = EnhancedSupport::GetInviteFilterAction();
        uint32 const inviteCount = EnhancedSupport::GetInviteRateCount();
        uint32 const inviteSeconds = EnhancedSupport::GetInviteRateSeconds();
        if (inviteAction == 0 || inviteCount == 0 || inviteSeconds == 0)
            handler->SendSysMessage("  Invite filter: disabled");
        else
        {
            uint8 const inviteMaxLevel = EnhancedSupport::GetInviteMaxLevel();
            handler->PSendSysMessage("  Invite filter: > {} invites in {}s, action {} ({}), level {}",
                inviteCount, inviteSeconds, static_cast<uint32>(inviteAction),
                EnhancedSupport::GetInviteFilterActionName(),
                inviteMaxLevel == 0 ? "any" : Acore::StringFormat("<= {}", static_cast<uint32>(inviteMaxLevel)));
        }

        uint32 const goldThreshold = EnhancedSupport::GetGoldFilterThreshold();
        handler->PSendSysMessage("  Gold filter threshold: {}",
            goldThreshold == 0 ? "disabled" : EnhancedSupport::FormatMoney(goldThreshold));

        uint32 const lootLevelGap = EnhancedSupport::GetLootFilterLevelGap();
        if (lootLevelGap == 0)
            handler->SendSysMessage("  Loot filter level gap: disabled");
        else
        {
            handler->PSendSysMessage("  Loot filter level gap: {}", lootLevelGap);

            uint32 const lootSources = EnhancedSupport::GetLootFilterSources();
            if (lootSources == 0)
                handler->SendSysMessage("  Loot filter sources: all");
            else
            {
                std::string names;
                auto const append = [&names](uint32 mask, uint32 bit, char const* name)
                {
                    if (mask & bit)
                    {
                        if (!names.empty())
                            names += ", ";
                        names += name;
                    }
                };
                append(lootSources, 1, "creature");
                append(lootSources, 2, "gameobject");
                append(lootSources, 4, "container");
                append(lootSources, 8, "corpse");
                append(lootSources, 16, "player");
                handler->PSendSysMessage("  Loot filter sources: {}", names.empty() ? "none" : names);
            }

            uint8 const lootMaxLevel = EnhancedSupport::GetLootFilterMaxLevel();
            if (lootMaxLevel == 0)
                handler->SendSysMessage("  Loot filter max level: no cap (all levels)");
            else
                handler->PSendSysMessage("  Loot filter max level: {}", static_cast<uint32>(lootMaxLevel));

            uint32 const lootBatch = EnhancedSupport::GetLootBatchSeconds();
            if (lootBatch == 0)
                handler->SendSysMessage("  Loot notification batching: off (one per item)");
            else
                handler->PSendSysMessage("  Loot notification batching: {}s window", lootBatch);
        }

        int32 const auctionMaxQuality = EnhancedSupport::GetAuctionFilterMaxQuality();
        bool const auctionAlwaysGrey = EnhancedSupport::GetAuctionFilterAlwaysLogGrey();
        uint32 const auctionGreyMinPrice = EnhancedSupport::GetAuctionFilterGreyMinPrice();
        if (auctionMaxQuality < 0 && !auctionAlwaysGrey && auctionGreyMinPrice == 0)
            handler->SendSysMessage("  Auction filter: disabled");
        else
        {
            std::string const quality = auctionMaxQuality < 0
                ? "off"
                : Acore::StringFormat("<= {} (price >= {})", auctionMaxQuality,
                    EnhancedSupport::FormatMoney(EnhancedSupport::GetAuctionFilterMinPrice()));
            std::string const grey = auctionAlwaysGrey
                ? "always"
                : (auctionGreyMinPrice == 0
                    ? "follows quality rule"
                    : Acore::StringFormat("price >= {}", EnhancedSupport::FormatMoney(auctionGreyMinPrice)));
            handler->PSendSysMessage("  Auction filter: quality {}, grey: {}, scope: {}",
                quality, grey,
                EnhancedSupport::GetAuctionFilterOnListing() ? "sales + listings" : "sales only");

            uint32 const auctionBatch = EnhancedSupport::GetAuctionBatchSeconds();
            if (auctionBatch == 0)
                handler->SendSysMessage("  Auction notification batching: off (one per auction)");
            else
                handler->PSendSysMessage("  Auction notification batching: {}s window", auctionBatch);
        }

        if (!EnhancedSupport::GetArenaTelemetryEnabled())
            handler->SendSysMessage("  Arena telemetry: disabled");
        else
        {
            uint32 const sampleMs = EnhancedSupport::GetArenaTelemetryPositionSampleMs();
            uint32 const retentionDays = EnhancedSupport::GetArenaTelemetryRetentionDays();
            handler->PSendSysMessage("  Arena telemetry: {}, position samples {}, retention {}",
                EnhancedSupport::GetArenaTelemetryRatedOnly() ? "rated matches only" : "all arena matches",
                sampleMs == 0 ? "off" : Acore::StringFormat("every {}ms", sampleMs),
                retentionDays == 0 ? "unlimited" : Acore::StringFormat("{} day(s)", retentionDays));

            if (!EnhancedSupport::GetArenaTelemetryAutoCheck())
                handler->SendSysMessage("  Arena auto-check: off");
            else
                handler->PSendSysMessage("  Arena auto-check: on (fast <= {}ms, flag at >= {} fast reactions and >= {}%)",
                    EnhancedSupport::GetArenaTelemetrySuspectReactionMs(),
                    EnhancedSupport::GetArenaTelemetrySuspectMinEvents(),
                    EnhancedSupport::GetArenaTelemetrySuspectPercent());
        }

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
        EnhancedSupport::LoadEmailPatterns();
        handler->PSendSysMessage("mod-enhanced-support: configuration, keywords and email patterns reloaded.");
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

    static bool HandleListEmailPatternsCommand(ChatHandler* handler)
    {
        std::vector<std::string> const& patterns = EnhancedSupport::GetEmailPatterns();
        if (patterns.empty())
        {
            handler->SendSysMessage("mod-enhanced-support: no email patterns configured.");
            return true;
        }

        handler->PSendSysMessage("mod-enhanced-support: {} email pattern(s):", patterns.size());
        for (std::string const& pattern : patterns)
            handler->PSendSysMessage(" - {}", pattern);

        return true;
    }

    static bool HandleEmailPatternAddCommand(ChatHandler* handler, Tail pattern)
    {
        std::string normalized = EnhancedSupport::NormalizeKeyword(pattern);
        if (normalized.empty())
        {
            handler->SendSysMessage("Usage: .support emailpattern add <pattern>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (EnhancedSupport::HasEmailPattern(normalized))
        {
            handler->PSendSysMessage("Email pattern already present: {}", normalized);
            return true;
        }

        EnhancedSupport::AddEmailPattern(normalized);
        handler->PSendSysMessage("Added email pattern: {}", normalized);
        return true;
    }

    static bool HandleEmailPatternRemoveCommand(ChatHandler* handler, Tail pattern)
    {
        std::string normalized = EnhancedSupport::NormalizeKeyword(pattern);
        if (normalized.empty())
        {
            handler->SendSysMessage("Usage: .support emailpattern remove <pattern>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!EnhancedSupport::HasEmailPattern(normalized))
        {
            handler->PSendSysMessage("Email pattern not found: {}", normalized);
            handler->SetSentErrorMessage(true);
            return false;
        }

        EnhancedSupport::RemoveEmailPattern(normalized);
        handler->PSendSysMessage("Removed email pattern: {}", normalized);
        return true;
    }

    // Lists the most recently recorded arena matches, newest first, so a GM can
    // find the match id to feed into ".support arena check".
    static bool HandleArenaMatchesCommand(ChatHandler* handler, Optional<uint32> count)
    {
        uint32 const limit = std::clamp<uint32>(count.value_or(10), 1, 50);

        QueryResult result = CharacterDatabase.Query(
            "SELECT match_id, MIN(time_ms), MAX(time_ms), COUNT(*), MAX(arena_type), MAX(rated) "
            "FROM enhanced_support_arena_events GROUP BY match_id ORDER BY MAX(time_ms) DESC LIMIT {}", limit);

        if (!result)
        {
            handler->SendSysMessage("No recorded arena matches found.");
            return true;
        }

        handler->SendSysMessage("Recorded arena matches (newest first):");
        do
        {
            Field* fields = result->Fetch();
            uint32 const matchId = fields[0].Get<uint32>();
            uint64 const firstMs = fields[1].Get<uint64>();
            uint64 const lastMs = fields[2].Get<uint64>();
            uint64 const events = fields[3].Get<uint64>();
            uint32 const arenaType = fields[4].Get<uint8>();
            bool const rated = fields[5].Get<uint8>() != 0;

            uint64 const durationSec = (lastMs - firstMs) / 1000;
            handler->PSendSysMessage("  match {} | {}v{}{} | {} | {}m {}s | {} event(s)",
                matchId, arenaType, arenaType, rated ? " rated" : "",
                Acore::Time::TimeToTimestampStr(Seconds(firstMs / 1000)),
                durationSec / 60, durationSec % 60, events);
        } while (result->NextRow());

        return true;
    }

    // Lists the recorded matches a given rated arena team took part in, newest
    // first. Only rated matches carry a team id; unrated arenas record 0.
    static bool HandleArenaTeamCommand(ChatHandler* handler, uint32 arenaTeamId, Optional<uint32> count)
    {
        if (arenaTeamId == 0)
        {
            handler->SendSysMessage("Usage: .support arena team <arenaTeamId> [count]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const limit = std::clamp<uint32>(count.value_or(10), 1, 50);

        QueryResult result = CharacterDatabase.Query(
            "SELECT match_id, MIN(time_ms), MAX(time_ms), COUNT(*), MAX(arena_type), MAX(rated) "
            "FROM enhanced_support_arena_events WHERE arena_team_id = {} "
            "GROUP BY match_id ORDER BY MAX(time_ms) DESC LIMIT {}", arenaTeamId, limit);

        if (!result)
        {
            handler->PSendSysMessage("No recorded arena matches found for team {}.", arenaTeamId);
            return true;
        }

        handler->PSendSysMessage("Recorded matches for arena team {} (newest first):", arenaTeamId);
        do
        {
            Field* fields = result->Fetch();
            uint32 const matchId = fields[0].Get<uint32>();
            uint64 const firstMs = fields[1].Get<uint64>();
            uint64 const lastMs = fields[2].Get<uint64>();
            uint64 const events = fields[3].Get<uint64>();
            uint32 const arenaType = fields[4].Get<uint8>();
            bool const rated = fields[5].Get<uint8>() != 0;

            uint64 const durationSec = (lastMs - firstMs) / 1000;
            handler->PSendSysMessage("  match {} | {}v{}{} | {} | {}m {}s | {} event(s)",
                matchId, arenaType, arenaType, rated ? " rated" : "",
                Acore::Time::TimeToTimestampStr(Seconds(firstMs / 1000)),
                durationSec / 60, durationSec % 60, events);
        } while (result->NextRow());

        return true;
    }

    // Shared printer for live-telemetry and replay-decoded reports; fromReplay
    // drops the fields replay packet data cannot provide (failed casts, jukes,
    // latency).
    static void PrintArenaReports(ChatHandler* handler,
        std::vector<EnhancedSupport::ArenaTelemetryPlayerReport>& reports, bool fromReplay)
    {
        std::sort(reports.begin(), reports.end(),
            [](EnhancedSupport::ArenaTelemetryPlayerReport const& a, EnhancedSupport::ArenaTelemetryPlayerReport const& b)
            {
                return a.team != b.team ? a.team < b.team : a.guidLow < b.guidLow;
            });

        auto const reactionBlock = [](uint32 total, uint32 fast, int32 minMs, int32 medianMs) -> std::string
        {
            if (total == 0)
                return "none";
            return Acore::StringFormat("{} (fast {}, min {}ms, median {}ms)", total, fast, minMs, medianMs);
        };

        for (EnhancedSupport::ArenaTelemetryPlayerReport const& report : reports)
        {
            std::string name;
            if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid(HighGuid::Player, report.guidLow), name) || name.empty())
                name = "Unknown";

            handler->PSendSysMessage("=== [team {}] {} (GUID {}){} ===",
                report.team, name, report.guidLow,
                report.suspicious ? " << SUSPICIOUS" : "");
            handler->PSendSysMessage("> Interrupts: {}",
                reactionBlock(report.interrupts, report.fastInterrupts, report.minInterruptMs, report.medianInterruptMs));
            handler->PSendSysMessage("> Dispels: {}",
                reactionBlock(report.dispels, report.fastDispels, report.minDispelMs, report.medianDispelMs));
            handler->PSendSysMessage("> CC breaks: {}",
                reactionBlock(report.ccBreaks, report.fastCCBreaks, report.minCCBreakMs, report.medianCCBreakMs));
            handler->PSendSysMessage("> CC after enemy trinket: {}",
                reactionBlock(report.trinketCCs, report.fastTrinketCCs, report.minTrinketCCMs, report.medianTrinketCCMs));
            handler->PSendSysMessage("> DoT reapplies: {}",
                reactionBlock(report.dotReapplies, report.fastDotReapplies, report.minDotReapplyMs, report.medianDotReapplyMs));

            std::string cadence;
            if (report.casts == 0)
                cadence = "none";
            else
            {
                cadence = Acore::StringFormat("{}", report.casts);
                if (report.apm >= 0)
                    cadence += Acore::StringFormat(", {} per minute", report.apm);
                if (report.medianCastGapMs >= 0)
                    cadence += Acore::StringFormat(", median gap {}ms", report.medianCastGapMs);
                if (report.castGapIqrMs >= 0)
                    cadence += Acore::StringFormat(" (IQR {}ms)", report.castGapIqrMs);
            }
            handler->PSendSysMessage("> Casts: {}", cadence);

            if (fromReplay)
                continue;

            if (report.failedCasts == 0)
                handler->SendSysMessage("> Failed casts: none");
            else
                handler->PSendSysMessage("> Failed casts: {} (nothing to dispel {}, line of sight {}, range/facing {})",
                    report.failedCasts, report.failedNothingToDispel, report.failedLos, report.failedRange);

            handler->PSendSysMessage("> Jukes: thrown {}, bitten {}", report.fakeCasts, report.fakeCastBites);
            handler->PSendSysMessage("> Latency: ~{}ms", report.avgLatencyMs);
        }
    }

    // Analyzes one recorded match (running matches are analyzed from the live
    // buffer) and prints per-player reaction statistics.
    static bool HandleArenaCheckCommand(ChatHandler* handler, uint32 matchId)
    {
        std::vector<EnhancedSupport::ArenaTelemetryPlayerReport> reports = EnhancedSupport::CheckArenaMatch(matchId);
        if (reports.empty())
        {
            handler->PSendSysMessage("No telemetry recorded for match {}.", matchId);
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage(
            "Match {} telemetry ({} player(s)). Fast = reaction <= {}ms after subtracting latency; "
            "flagged at >= {} fast reactions making up >= {}%:",
            matchId, reports.size(), EnhancedSupport::GetArenaTelemetrySuspectReactionMs(),
            EnhancedSupport::GetArenaTelemetrySuspectMinEvents(), EnhancedSupport::GetArenaTelemetrySuspectPercent());

        PrintArenaReports(handler, reports, false);

        handler->SendSysMessage("DoT reapplies, cast cadence and failed casts are informational and never flag: "
            "DoT expiry is predictable with timer addons, and cast volume varies by spec. "
            "A scripted player shows a near-zero cast gap IQR and many 'nothing to dispel' failures.");
        handler->SendSysMessage("Verdicts need several matches, not one: compare with this player's other matches before acting.");
        return true;
    }

    // Analyzes a match recorded by mod-arena-replay: its stored packet stream is
    // decoded into telemetry events and run through the same analysis, covering
    // matches this module never recorded live.
    static bool HandleArenaReplayCommand(ChatHandler* handler, uint32 replayId)
    {
        std::string error;
        std::vector<EnhancedSupport::ArenaTelemetryPlayerReport> reports = EnhancedSupport::CheckArenaReplay(replayId, error);
        if (reports.empty())
        {
            handler->PSendSysMessage("Cannot check replay {}: {}.", replayId, error);
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage(
            "Replay {} analysis ({} player(s)), decoded from mod-arena-replay packet data. "
            "Fast = reaction <= {}ms; flagged at >= {} fast reactions making up >= {}%:",
            replayId, reports.size(), EnhancedSupport::GetArenaTelemetrySuspectReactionMs(),
            EnhancedSupport::GetArenaTelemetrySuspectMinEvents(), EnhancedSupport::GetArenaTelemetrySuspectPercent());

        PrintArenaReports(handler, reports, true);

        handler->SendSysMessage("Replay timestamps advance once per world tick and carry no latency data, "
            "so single reactions are coarser than live telemetry; judge the distributions, not one value. "
            "Failed casts and jukes are not recoverable from replay data.");
        handler->SendSysMessage("Verdicts need several matches, not one: compare with this player's other matches before acting.");
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
