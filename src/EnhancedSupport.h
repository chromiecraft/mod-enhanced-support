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

#ifndef MOD_ENHANCED_SUPPORT_H
#define MOD_ENHANCED_SUPPORT_H

#include "Define.h"

#include <string>
#include <string_view>
#include <vector>

// Shared module API. State lives in mod_enhanced_support.cpp; the command
// script (cs_enhanced_support.cpp) reaches it through these functions.
namespace EnhancedSupport
{
    void LoadConfig();
    void LoadKeywords();

    // Canonical (trimmed + lowercased) form used for storage and matching.
    std::string NormalizeKeyword(std::string_view input);

    std::vector<std::string> const& GetKeywords();
    bool HasKeyword(std::string const& normalized);
    void AddKeyword(std::string const& normalized);
    void RemoveKeyword(std::string const& normalized);

    // Email substrings matched against an account's email at character creation.
    // Stored and matched like keywords (trimmed, lowercased, case-insensitive substring).
    void LoadEmailPatterns();
    std::vector<std::string> const& GetEmailPatterns();
    bool GetEmailFilterEnabled();
    bool HasEmailPattern(std::string const& normalized);
    void AddEmailPattern(std::string const& normalized);
    void RemoveEmailPattern(std::string const& normalized);

    // Current active settings (for the .support info command).
    bool IsEnabled();
    uint8 GetMailFilterAction();
    std::string_view GetMailFilterActionName();
    std::string const& GetMailFilterMessage();
    bool GetMailSkipSameAccount();
    uint8 GetChatFilterAction();
    std::string_view GetChatFilterActionName();
    uint8 GetAggressiveMaxLevel();

    // Cross-message chat window: max lines kept per sender, and how long (seconds)
    // a line stays in the window. Both 0 when the windowed pass is disabled.
    uint32 GetChatWindowSize();
    uint32 GetChatWindowSeconds();

    // Party-invite spam filter: action scale, the level cap it watches (0 = all
    // levels), and the rate threshold (more than Count invites within Seconds).
    uint8 GetInviteFilterAction();
    std::string_view GetInviteFilterActionName();
    uint8 GetInviteMaxLevel();
    uint32 GetInviteRateCount();
    uint32 GetInviteRateSeconds();

    std::string const& GetBanAuthor();

    // Money threshold (in copper) at/above which mail is logged; 0 disables the check.
    uint32 GetGoldFilterThreshold();

    // Level gap between a looted item's required level and the looter's level at/above
    // which the loot is logged; 0 disables the check.
    uint32 GetLootFilterLevelGap();

    // Bitmask of loot sources the check watches (1 creature, 2 gameobject, 4 container,
    // 8 corpse, 16 player); 0 watches every source.
    uint32 GetLootFilterSources();

    // Highest looter level the loot check applies to; 0 means no cap (all levels).
    uint8 GetLootFilterMaxLevel();

    // Seconds of inactivity before a looter's batched Discord loot notification is
    // sent; 0 sends one notification per item (no batching).
    uint32 GetLootBatchSeconds();

    // Auction filter: highest item quality flagged (0 grey, 1 white, ...; < 0 disables
    // the check), the price (copper) at/above which a match is flagged, whether every
    // grey item is flagged regardless of price, the grey-specific price (copper) at/above
    // which grey items are flagged (0 = off), and whether listings are flagged too or
    // only completed sales.
    int32 GetAuctionFilterMaxQuality();
    uint32 GetAuctionFilterMinPrice();
    bool GetAuctionFilterAlwaysLogGrey();
    uint32 GetAuctionFilterGreyMinPrice();
    bool GetAuctionFilterOnListing();

    // Seconds of inactivity before a seller's batched Discord auction notification is
    // sent; 0 sends one notification per auction (no batching).
    uint32 GetAuctionBatchSeconds();

    // Formats a copper amount as a "Xg Ys Zc" string.
    std::string FormatMoney(uint32 copper);

    // Arena telemetry (cheat detection): raw cast/aura/position events from live
    // arena matches, written to the characters DB table enhanced_support_arena_events
    // for offline reaction-time and facing analysis. State lives in
    // arena_telemetry.cpp; LoadConfig() calls LoadArenaTelemetryConfig().
    void LoadArenaTelemetryConfig();
    bool GetArenaTelemetryEnabled();
    bool GetArenaTelemetryRatedOnly();
    uint32 GetArenaTelemetryPositionSampleMs();
    uint32 GetArenaTelemetryRetentionDays();
    bool GetArenaTelemetryAutoCheck();
    uint32 GetArenaTelemetrySuspectReactionMs();
    uint32 GetArenaTelemetrySuspectMinEvents();
    uint32 GetArenaTelemetrySuspectPercent();

    // Per-player result of analyzing one match's telemetry. Reaction figures are
    // milliseconds, reduced by the player's latency at the response; -1 means no
    // data. "Fast" counts reactions at or below the suspect threshold. A player
    // is suspicious when their fast reactions reach the configured minimum count
    // and share (see the ArenaTelemetry.Suspect.* options).
    struct ArenaTelemetryPlayerReport
    {
        uint32 guidLow = 0;
        uint8 team = 0;
        uint32 interrupts = 0;      // interrupt casts matched to an enemy cast bar
        uint32 fastInterrupts = 0;
        int32 minInterruptMs = -1;
        int32 medianInterruptMs = -1;
        uint32 dispels = 0;         // dispel casts matched to a dispellable aura on their target
        uint32 fastDispels = 0;
        int32 minDispelMs = -1;
        int32 medianDispelMs = -1;
        uint32 fakeCasts = 0;       // own casts cancelled by the player (jukes thrown)
        uint32 fakeCastBites = 0;   // interrupts thrown right after an enemy fake cast (jukes bitten)
        uint32 avgLatencyMs = 0;
        bool suspicious = false;
    };

    // Analyzes a finished match from the DB (falls back to the in-memory buffer
    // of a still-running match). Synchronous; intended for the .support command.
    std::vector<ArenaTelemetryPlayerReport> CheckArenaMatch(uint32 matchId);

    // Highest valid mail filter action value.
    uint8 GetMaxMailFilterAction();

    // Runtime-only override of the mail filter action; reverts to config on reload.
    void SetMailFilterAction(uint8 action);
}

#endif // MOD_ENHANCED_SUPPORT_H
