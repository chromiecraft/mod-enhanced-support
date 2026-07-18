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
#include "Battleground.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

// Optional integration: relay auto-check alerts to Discord via mod-chat-transmitter.
#if __has_include("mod-chat-transmitter/src/ChatTransmitter.h")
#include "mod-chat-transmitter/src/ChatTransmitter.h"
#define HAS_CHAT_TRANSMITTER 1
#endif

// Arena telemetry: records raw combat events from live arena matches into the
// characters DB (enhanced_support_arena_events) for offline cheat detection -
// input automation (AHK-style interrupt/dispel bots) shows up as statistically
// inhuman reaction-time distributions and frame-perfect facing, neither of
// which can be judged from a single event in real time. Log-only; no gameplay
// is ever altered.
//
// Events are buffered in memory per match and written as one transaction when
// the match ends (or the arena is destroyed / the server shuts down cleanly),
// so a running match costs no DB traffic at all. A crash loses the in-flight
// matches' telemetry, which is acceptable for evidence gathering.

namespace
{
    enum ArenaTelemetryEvent : uint8
    {
        ARENA_EVENT_CAST_START  = 1, // a cast bar appeared; extra = cast time (ms). Interrupt-bot stimulus.
        ARENA_EVENT_CAST_CANCEL = 2, // extra = 1 cancelled by the caster (fake cast), 0 interrupted/failed
        ARENA_EVENT_CAST_GO     = 3, // spell actually fired (instants included). Interrupt/dispel/CC-break response.
        ARENA_EVENT_AURA_APPLY  = 4, // dispellable or controlling aura landed on a player; extra = dispel
                                     // type (0 for non-dispellable CC like stuns). Dispel/CC-break-bot stimulus.
        ARENA_EVENT_POSITION    = 5, // periodic position/orientation sample, for facing analysis
        ARENA_EVENT_AURA_REMOVE = 6, // recorded aura left the player; extra = AuraRemoveMode. DoT-reapply stimulus.
        ARENA_EVENT_CAST_FAILED = 7, // server rejected a cast (SMSG_CAST_FAILED); extra = SpellCastResult
    };

    // Loss-of-control mechanics whose aura applications are recorded as stimuli
    // and matched against removal/immunity responses (trinket, Berserker Rage,
    // ...). Snare/daze are deliberately absent: every frost bolt would log one.
    constexpr uint64 CONTROL_MECHANIC_MASK =
        (1ULL << MECHANIC_CHARM) | (1ULL << MECHANIC_DISORIENTED) | (1ULL << MECHANIC_FEAR) |
        (1ULL << MECHANIC_ROOT) | (1ULL << MECHANIC_SILENCE) | (1ULL << MECHANIC_SLEEP) |
        (1ULL << MECHANIC_STUN) | (1ULL << MECHANIC_FREEZE) | (1ULL << MECHANIC_KNOCKOUT) |
        (1ULL << MECHANIC_POLYMORPH) | (1ULL << MECHANIC_BANISH) | (1ULL << MECHANIC_SHACKLE) |
        (1ULL << MECHANIC_TURN) | (1ULL << MECHANIC_HORROR) | (1ULL << MECHANIC_SAPPED);

    bool _telemetryEnabled = false;
    bool _telemetryRatedOnly = true;
    uint32 _telemetryPositionSampleMs = 500;
    uint32 _telemetryRetentionDays = 30;

    // Auto-check: analyze each recorded match when it ends and report players
    // whose reaction profile crosses the thresholds below. A reaction is "fast"
    // when it is at or below SuspectReactionMs after subtracting the player's
    // latency; a player is flagged when they have at least SuspectMinEvents
    // fast reactions making up at least SuspectPercent of their reactions.
    bool _telemetryAutoCheck = false;
    uint32 _suspectReactionMs = 180;
    uint32 _suspectMinEvents = 4;
    uint32 _suspectPercent = 60;

    // One buffered row of enhanced_support_arena_events (all-numeric).
    struct ArenaEventRow
    {
        uint64 timeMs;
        uint32 mapId;
        uint8 arenaType;
        bool rated;
        uint8 eventType;
        uint32 actorGuid;
        uint8 actorTeam;
        uint32 arenaTeamId;
        uint32 spellId;
        uint64 targetGuid;
        uint32 extra;
        uint32 latencyMs;
        float posX;
        float posY;
        float orientation;
        float tgtX;         // target unit's position/orientation at event time;
        float tgtY;         // all zero when the event has no resolvable unit target
        float tgtO;
    };

    // A match's rows never hit the DB while it runs; they accumulate here and
    // are flushed in one transaction at match end. Keyed by the battleground
    // instance id (== the arena map's instance id, unique among live maps).
    // Arena maps update on parallel worker threads, so access is guarded.
    std::mutex _matchesLock;
    std::unordered_map<uint32 /*matchId*/, std::vector<ArenaEventRow>> _matchBuffers;

    // Position-sample accumulators per arena map instance, same locking story.
    std::mutex _sampleTimersLock;
    std::unordered_map<uint32 /*matchId*/, uint32 /*elapsedMs*/> _sampleTimers;

    // Safety valve: a match buffer that somehow grows past this is flushed
    // mid-match to bound memory. At default sampling a 3v3 produces a few
    // thousand rows, so this is never reached in normal play.
    constexpr size_t MAX_BUFFERED_ROWS = 50000;

    // Rows per INSERT statement when flushing, keeping each statement's text
    // comfortably below MySQL packet limits.
    constexpr size_t FLUSH_CHUNK_ROWS = 250;

    uint64 NowEpochMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            GameTime::GetSystemTime().time_since_epoch()).count();
    }

    // The arena the player's actions should be recorded for, or nullptr when out
    // of scope: telemetry/module disabled, not a running arena round (skips the
    // preparation phase), unrated match with RatedOnly set, or a GM/spectator.
    Battleground* GetTrackedArena(Player* player)
    {
        if (!_telemetryEnabled || !EnhancedSupport::IsEnabled())
            return nullptr;

        if (player->IsGameMaster() || player->IsSpectator())
            return nullptr;

        Battleground* bg = player->GetBattleground();
        if (!bg || !bg->isArena() || bg->GetStatus() != STATUS_IN_PROGRESS)
            return nullptr;

        if (_telemetryRatedOnly && !bg->isRated())
            return nullptr;

        return bg;
    }

    // Writes one match's buffered rows as a single transaction of multi-row
    // INSERTs. The commit is queued to the DB worker thread; the caller (a map
    // update or the BG manager) never waits on MySQL.
    void FlushRows(uint32 matchId, std::vector<ArenaEventRow> const& rows)
    {
        if (rows.empty())
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        std::string sql;
        size_t inChunk = 0;
        for (ArenaEventRow const& row : rows)
        {
            if (inChunk == 0)
                sql = "INSERT INTO enhanced_support_arena_events "
                    "(time_ms, match_id, map_id, arena_type, rated, event_type, actor_guid, actor_team, arena_team_id, "
                    "spell_id, target_guid, extra, latency_ms, pos_x, pos_y, orientation, tgt_x, tgt_y, tgt_o) VALUES ";
            else
                sql += ", ";

            sql += Acore::StringFormat("({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
                row.timeMs, matchId, row.mapId, static_cast<uint32>(row.arenaType), row.rated ? 1 : 0,
                static_cast<uint32>(row.eventType), row.actorGuid, static_cast<uint32>(row.actorTeam), row.arenaTeamId,
                row.spellId, row.targetGuid, row.extra, row.latencyMs,
                row.posX, row.posY, row.orientation, row.tgtX, row.tgtY, row.tgtO);

            if (++inChunk == FLUSH_CHUNK_ROWS)
            {
                trans->Append(sql);
                inChunk = 0;
            }
        }

        if (inChunk != 0)
            trans->Append(sql);

        CharacterDatabase.CommitTransaction(trans);

        LOG_DEBUG("module.enhancedsupport", "ArenaTelemetry: flushed {} event(s) for match {}.",
            rows.size(), matchId);
    }

    // Takes a match's buffer out (empty when already taken by an earlier hook).
    std::vector<ArenaEventRow> TakeMatch(uint32 matchId)
    {
        std::lock_guard<std::mutex> guard(_matchesLock);
        auto it = _matchBuffers.find(matchId);
        if (it == _matchBuffers.end())
            return {};

        std::vector<ArenaEventRow> rows = std::move(it->second);
        _matchBuffers.erase(it);
        return rows;
    }

    void FlushMatch(uint32 matchId)
    {
        FlushRows(matchId, TakeMatch(matchId));
    }

    void FlushAllMatches()
    {
        std::unordered_map<uint32, std::vector<ArenaEventRow>> buffers;
        {
            std::lock_guard<std::mutex> guard(_matchesLock);
            buffers.swap(_matchBuffers);
        }

        for (auto const& [matchId, rows] : buffers)
            FlushRows(matchId, rows);
    }

    void BufferEvent(Battleground* bg, Player* actor, uint8 eventType, uint32 spellId,
        ObjectGuid target, uint32 extra)
    {
        ArenaEventRow row;
        row.timeMs = NowEpochMs();
        row.mapId = actor->GetMapId();
        row.arenaType = bg->GetArenaType();
        row.rated = bg->isRated();
        row.eventType = eventType;
        row.actorGuid = actor->GetGUID().GetCounter();
        row.actorTeam = static_cast<uint8>(actor->GetBgTeamId());
        row.arenaTeamId = bg->GetArenaTeamIdForTeam(actor->GetBgTeamId());
        row.spellId = spellId;
        row.targetGuid = target.GetRawValue();
        row.extra = extra;
        row.latencyMs = actor->GetSession()->GetLatency();
        row.posX = actor->GetPositionX();
        row.posY = actor->GetPositionY();
        row.orientation = actor->GetOrientation();

        // The target's placement at the same instant, so facing checks (e.g.
        // behind-arc abilities) don't depend on the nearest position sample.
        row.tgtX = 0.0f;
        row.tgtY = 0.0f;
        row.tgtO = 0.0f;
        if (target)
        {
            if (Unit const* targetUnit = ObjectAccessor::GetUnit(*actor, target))
            {
                row.tgtX = targetUnit->GetPositionX();
                row.tgtY = targetUnit->GetPositionY();
                row.tgtO = targetUnit->GetOrientation();
            }
        }

        uint32 const matchId = bg->GetInstanceID();
        bool overflow = false;
        {
            std::lock_guard<std::mutex> guard(_matchesLock);
            std::vector<ArenaEventRow>& buffer = _matchBuffers[matchId];
            buffer.push_back(row);
            overflow = buffer.size() >= MAX_BUFFERED_ROWS;
        }

        if (overflow)
            FlushMatch(matchId);
    }

    // ---- Match analysis ----
    //
    // Responses are classified by spell effect, so no spell list is maintained:
    // anything with SPELL_EFFECT_INTERRUPT_CAST counts as an interrupt, anything
    // with SPELL_EFFECT_DISPEL as a dispel. Reactions are server-observed and
    // reduced by the actor's latency at the response, approximating the player's
    // own reaction time. The matching is heuristic (nearest preceding stimulus),
    // which is plenty for flagging: verdicts come from the distribution, not
    // from any single event.

    int32 AdjustedReactionMs(uint64 stimulusMs, uint64 responseMs, uint32 latencyMs)
    {
        int64 const raw = static_cast<int64>(responseMs - stimulusMs) - static_cast<int64>(latencyMs);
        return static_cast<int32>(std::max<int64>(raw, 0));
    }

    // Sorts in place; -1 when empty.
    int32 MedianOf(std::vector<int32>& values)
    {
        if (values.empty())
            return -1;

        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    }

    // Mechanics this spell removes or grants immunity to when cast: dispel-by-
    // mechanic effects (PvP trinket) plus the core's precomputed per-effect
    // immunity masks (Berserker Rage, Ice Block, Blessing of Freedom, ...).
    uint64 BreakMechanicMask(SpellInfo const* spellInfo)
    {
        uint64 mask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_DISPEL_MECHANIC && spellInfo->Effects[i].MiscValue >= 0)
                mask |= 1ULL << static_cast<uint32>(spellInfo->Effects[i].MiscValue);

            if (ImmunityInfo const* immunity = spellInfo->GetImmunityInfo(i))
                mask |= immunity->MechanicImmuneMask;
        }

        return mask & CONTROL_MECHANIC_MASK;
    }

    std::vector<EnhancedSupport::ArenaTelemetryPlayerReport> AnalyzeArenaRows(std::vector<ArenaEventRow>& rows)
    {
        struct OpenCast
        {
            uint64 startMs = 0;
            uint32 castTimeMs = 0;  // 0 for channels
            uint64 cancelMs = 0;
            bool cancelBySelf = false;
            bool open = false;
        };

        struct PlayerAgg
        {
            uint8 team = 0;
            std::vector<int32> interruptReactions;
            std::vector<int32> dispelReactions;
            std::vector<int32> ccBreakReactions;
            std::vector<int32> trinketCCReactions;
            std::vector<int32> dotReapplies;
            uint32 casts = 0;
            uint64 lastCastMs = 0;
            std::vector<int32> castGaps;
            uint32 failedCasts = 0;
            uint32 failedNoDispel = 0;
            uint32 failedLos = 0;
            uint32 failedRange = 0;
            uint32 fakeCasts = 0;
            uint32 fakeCastBites = 0;
            uint64 latencySum = 0;
            uint32 latencyCount = 0;
        };

        // Last loss-of-control aura on a player, for matching CC-break responses.
        struct ControlStimulus
        {
            uint64 timeMs = 0;
            uint64 mechanicMask = 0;
        };

        std::sort(rows.begin(), rows.end(),
            [](ArenaEventRow const& a, ArenaEventRow const& b) { return a.timeMs < b.timeMs; });

        std::unordered_map<uint32 /*guidLow*/, OpenCast> casts;
        std::unordered_map<uint32 /*guidLow*/, uint64> lastAuraMs;
        std::unordered_map<uint32 /*guidLow*/, ControlStimulus> lastControl;
        std::unordered_map<uint32 /*guidLow*/, PlayerAgg> players;

        // Open DoT windows: a periodic-damage aura fell off a player (expired or
        // dispelled); the caster's next cast of that spell on the same target is
        // the reapply. Keyed (victim, caster, spell).
        std::map<std::tuple<uint32, uint32, uint32>, uint64> openDots;

        // Last CC-removal/immunity cast per player, for measuring how fast an
        // enemy answers the trinket with fresh CC. Consumed on match.
        std::unordered_map<uint32 /*guidLow*/, uint64> lastBreakMs;

        // Cast-time CC counts from its cast start (a poly pre-cast before the
        // trinket never matches, its start predates the stimulus); instant CC
        // counts from its GO row.
        auto const matchTrinketCC = [&](ArenaEventRow const& row, PlayerAgg& actor, uint32 victimLow)
        {
            auto it = lastBreakMs.find(victimLow);
            if (it == lastBreakMs.end())
                return;

            auto victim = players.find(victimLow);
            if (victim == players.end() || victim->second.team == actor.team)
                return;

            if (row.timeMs >= it->second && row.timeMs - it->second <= 5000)
            {
                actor.trinketCCReactions.push_back(AdjustedReactionMs(it->second, row.timeMs, row.latencyMs));
                lastBreakMs.erase(it);
            }
        };

        for (ArenaEventRow const& row : rows)
        {
            PlayerAgg& actor = players[row.actorGuid];
            actor.team = row.actorTeam;

            switch (row.eventType)
            {
                case ARENA_EVENT_CAST_START:
                {
                    OpenCast& cast = casts[row.actorGuid];
                    cast.startMs = row.timeMs;
                    cast.castTimeMs = row.extra;
                    cast.open = true;

                    ObjectGuid const target(row.targetGuid);
                    if (target.IsPlayer())
                        if (SpellInfo const* startInfo = sSpellMgr->GetSpellInfo(row.spellId))
                            if (startInfo->GetAllEffectsMechanicMask() & CONTROL_MECHANIC_MASK)
                                matchTrinketCC(row, actor, target.GetCounter());
                    break;
                }
                case ARENA_EVENT_CAST_CANCEL:
                {
                    OpenCast& cast = casts[row.actorGuid];
                    if (!cast.open)
                        break;

                    cast.open = false;
                    cast.cancelMs = row.timeMs;
                    cast.cancelBySelf = row.extra == 1;
                    if (cast.cancelBySelf)
                        ++actor.fakeCasts;
                    break;
                }
                case ARENA_EVENT_AURA_APPLY:
                {
                    // Dispellable auras (the capture stored the dispel type in
                    // extra) feed dispel reactions; control auras feed CC breaks.
                    if (row.extra != 0)
                        lastAuraMs[row.actorGuid] = row.timeMs;

                    if (SpellInfo const* auraInfo = sSpellMgr->GetSpellInfo(row.spellId))
                        if (uint64 const ccMask = auraInfo->GetAllEffectsMechanicMask() & CONTROL_MECHANIC_MASK)
                            lastControl[row.actorGuid] = { row.timeMs, ccMask };
                    break;
                }
                case ARENA_EVENT_AURA_REMOVE:
                {
                    // Only expiry and dispel open a reapply window; death or a
                    // manual cancel is not a stimulus the DoT owner reacts to.
                    if (row.extra != AURA_REMOVE_BY_EXPIRE && row.extra != AURA_REMOVE_BY_ENEMY_SPELL)
                        break;

                    SpellInfo const* auraInfo = sSpellMgr->GetSpellInfo(row.spellId);
                    if (!auraInfo || !auraInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE))
                        break;

                    ObjectGuid const caster(row.targetGuid);
                    if (caster.IsPlayer())
                        openDots[{ row.actorGuid, caster.GetCounter(), row.spellId }] = row.timeMs;
                    break;
                }
                case ARENA_EVENT_CAST_FAILED:
                {
                    ++actor.failedCasts;
                    switch (row.extra)
                    {
                        case SPELL_FAILED_NOTHING_TO_DISPEL:
                            ++actor.failedNoDispel;
                            break;
                        case SPELL_FAILED_LINE_OF_SIGHT:
                            ++actor.failedLos;
                            break;
                        case SPELL_FAILED_OUT_OF_RANGE:
                        case SPELL_FAILED_UNIT_NOT_INFRONT:
                            ++actor.failedRange;
                            break;
                        default:
                            break;
                    }
                    break;
                }
                case ARENA_EVENT_CAST_GO:
                {
                    actor.latencySum += row.latencyMs;
                    ++actor.latencyCount;

                    ++actor.casts;
                    // Gaps over 10s are downtime (dead, drinking, pillared), not cadence.
                    if (actor.lastCastMs != 0 && row.timeMs >= actor.lastCastMs && row.timeMs - actor.lastCastMs <= 10000)
                        actor.castGaps.push_back(static_cast<int32>(row.timeMs - actor.lastCastMs));
                    actor.lastCastMs = row.timeMs;

                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(row.spellId);
                    if (!spellInfo)
                        break;

                    // The caster's own cast bar completed. Channels stay open
                    // until their cancel row, since kicks land mid-channel.
                    if (!spellInfo->IsChanneled())
                    {
                        auto own = casts.find(row.actorGuid);
                        if (own != casts.end())
                            own->second.open = false;
                    }

                    // CC-break response: the cast removes or grants immunity to
                    // the mechanic that just landed on the caster (trinket while
                    // stunned, Berserker Rage on fear, ...). Checked before the
                    // target gate since these casts are self-targeted. Consumed
                    // on match, so one CC counts one response.
                    if (uint64 const breakMask = BreakMechanicMask(spellInfo))
                    {
                        lastBreakMs[row.actorGuid] = row.timeMs;

                        auto cc = lastControl.find(row.actorGuid);
                        if (cc != lastControl.end() && (cc->second.mechanicMask & breakMask)
                            && row.timeMs >= cc->second.timeMs && row.timeMs - cc->second.timeMs <= 8000)
                        {
                            actor.ccBreakReactions.push_back(AdjustedReactionMs(cc->second.timeMs, row.timeMs, row.latencyMs));
                            lastControl.erase(cc);
                        }
                    }

                    ObjectGuid const target(row.targetGuid);
                    if (!target.IsPlayer())
                        break;

                    uint32 const targetLow = target.GetCounter();

                    if ((spellInfo->GetAllEffectsMechanicMask() & CONTROL_MECHANIC_MASK) && spellInfo->CalcCastTime() == 0)
                        matchTrinketCC(row, actor, targetLow);

                    if (spellInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE))
                    {
                        auto dot = openDots.find({ targetLow, row.actorGuid, row.spellId });
                        if (dot != openDots.end() && row.timeMs >= dot->second && row.timeMs - dot->second <= 15000)
                        {
                            actor.dotReapplies.push_back(AdjustedReactionMs(dot->second, row.timeMs, row.latencyMs));
                            openDots.erase(dot);
                        }
                    }

                    if (spellInfo->HasEffect(SPELL_EFFECT_INTERRUPT_CAST))
                    {
                        auto it = casts.find(targetLow);
                        if (it != casts.end())
                        {
                            OpenCast const& cast = it->second;
                            uint64 const windowMs = cast.castTimeMs > 0 ? cast.castTimeMs + 500 : 10000;
                            if (cast.open && row.timeMs >= cast.startMs && row.timeMs - cast.startMs <= windowMs)
                                actor.interruptReactions.push_back(AdjustedReactionMs(cast.startMs, row.timeMs, row.latencyMs));
                            // When the kick itself interrupted the cast, the victim's
                            // cancel row lands just before the kick's cast row.
                            else if (!cast.open && !cast.cancelBySelf && row.timeMs >= cast.cancelMs && row.timeMs - cast.cancelMs <= 200)
                                actor.interruptReactions.push_back(AdjustedReactionMs(cast.startMs, row.timeMs, row.latencyMs));
                            // Kick thrown just after the target cancelled their own cast: baited.
                            else if (!cast.open && cast.cancelBySelf && row.timeMs >= cast.cancelMs && row.timeMs - cast.cancelMs <= 1200)
                                ++actor.fakeCastBites;
                        }
                    }

                    if (spellInfo->HasEffect(SPELL_EFFECT_DISPEL))
                    {
                        auto it = lastAuraMs.find(targetLow);
                        if (it != lastAuraMs.end() && row.timeMs >= it->second && row.timeMs - it->second <= 8000)
                            actor.dispelReactions.push_back(AdjustedReactionMs(it->second, row.timeMs, row.latencyMs));
                    }
                    break;
                }
                default:
                    break;
            }
        }

        uint64 const matchDurationMs = rows.empty() ? 0 : rows.back().timeMs - rows.front().timeMs;

        std::vector<EnhancedSupport::ArenaTelemetryPlayerReport> reports;
        reports.reserve(players.size());
        for (auto& [guidLow, agg] : players)
        {
            EnhancedSupport::ArenaTelemetryPlayerReport report;
            report.guidLow = guidLow;
            report.team = agg.team;
            report.interrupts = static_cast<uint32>(agg.interruptReactions.size());
            report.dispels = static_cast<uint32>(agg.dispelReactions.size());
            report.ccBreaks = static_cast<uint32>(agg.ccBreakReactions.size());
            report.trinketCCs = static_cast<uint32>(agg.trinketCCReactions.size());
            report.dotReapplies = static_cast<uint32>(agg.dotReapplies.size());
            report.fakeCasts = agg.fakeCasts;
            report.fakeCastBites = agg.fakeCastBites;
            report.avgLatencyMs = agg.latencyCount != 0 ? static_cast<uint32>(agg.latencySum / agg.latencyCount) : 0;

            report.casts = agg.casts;
            if (matchDurationMs >= 30000)
                report.apm = static_cast<int32>(agg.casts * 60000 / matchDurationMs);
            report.failedCasts = agg.failedCasts;
            report.failedNothingToDispel = agg.failedNoDispel;
            report.failedLos = agg.failedLos;
            report.failedRange = agg.failedRange;

            for (int32 reaction : agg.interruptReactions)
                if (reaction <= static_cast<int32>(_suspectReactionMs))
                    ++report.fastInterrupts;
            for (int32 reaction : agg.dispelReactions)
                if (reaction <= static_cast<int32>(_suspectReactionMs))
                    ++report.fastDispels;
            for (int32 reaction : agg.ccBreakReactions)
                if (reaction <= static_cast<int32>(_suspectReactionMs))
                    ++report.fastCCBreaks;
            for (int32 reaction : agg.trinketCCReactions)
                if (reaction <= static_cast<int32>(_suspectReactionMs))
                    ++report.fastTrinketCCs;
            for (int32 reaction : agg.dotReapplies)
                if (reaction <= static_cast<int32>(_suspectReactionMs))
                    ++report.fastDotReapplies;

            report.medianInterruptMs = MedianOf(agg.interruptReactions);
            report.minInterruptMs = agg.interruptReactions.empty() ? -1 : agg.interruptReactions.front();
            report.medianDispelMs = MedianOf(agg.dispelReactions);
            report.minDispelMs = agg.dispelReactions.empty() ? -1 : agg.dispelReactions.front();
            report.medianCCBreakMs = MedianOf(agg.ccBreakReactions);
            report.minCCBreakMs = agg.ccBreakReactions.empty() ? -1 : agg.ccBreakReactions.front();
            report.medianTrinketCCMs = MedianOf(agg.trinketCCReactions);
            report.minTrinketCCMs = agg.trinketCCReactions.empty() ? -1 : agg.trinketCCReactions.front();
            report.medianDotReapplyMs = MedianOf(agg.dotReapplies);
            report.minDotReapplyMs = agg.dotReapplies.empty() ? -1 : agg.dotReapplies.front();

            report.medianCastGapMs = MedianOf(agg.castGaps);
            if (agg.castGaps.size() >= 4)
                report.castGapIqrMs = agg.castGaps[agg.castGaps.size() * 3 / 4] - agg.castGaps[agg.castGaps.size() / 4];

            // DoT reapplies never feed the flag: expiry is predictable (timer
            // addons), so near-zero reapply times are normal for good players.
            uint32 const total = report.interrupts + report.dispels + report.ccBreaks + report.trinketCCs;
            uint32 const fast = report.fastInterrupts + report.fastDispels + report.fastCCBreaks + report.fastTrinketCCs;
            report.suspicious = fast >= _suspectMinEvents && fast * 100 >= total * _suspectPercent;

            reports.push_back(report);
        }

        return reports;
    }

    std::string ResolvePlayerName(uint32 guidLow)
    {
        std::string name;
        if (!sCharacterCache->GetCharacterNameByGuid(ObjectGuid(HighGuid::Player, guidLow), name) || name.empty())
            name = "Unknown";
        return name;
    }

    // Logs (and relays to Discord, like the chat filter) every flagged player of
    // a just-ended match. Log-only; no punishment is issued.
    void ReportSuspiciousPlayers(Battleground* bg, std::vector<EnhancedSupport::ArenaTelemetryPlayerReport> const& reports)
    {
#ifdef HAS_CHAT_TRANSMITTER
        std::string lines;
#endif
        for (EnhancedSupport::ArenaTelemetryPlayerReport const& report : reports)
        {
            if (!report.suspicious)
                continue;

            std::string const name = ResolvePlayerName(report.guidLow);

            LOG_INFO("module.enhancedsupport",
                "ArenaTelemetry: match {} ({}v{}{}) flagged {} ({}) - interrupts {} (fast {}, min {}ms, median {}ms), "
                "dispels {} (fast {}, min {}ms, median {}ms), CC breaks {} (fast {}, min {}ms, median {}ms), "
                "CC after trinket {} (fast {}, min {}ms, median {}ms), "
                "jukes bitten {}, latency ~{}ms",
                bg->GetInstanceID(), static_cast<uint32>(bg->GetArenaType()), static_cast<uint32>(bg->GetArenaType()), bg->isRated() ? ", rated" : "",
                name, report.guidLow,
                report.interrupts, report.fastInterrupts, report.minInterruptMs, report.medianInterruptMs,
                report.dispels, report.fastDispels, report.minDispelMs, report.medianDispelMs,
                report.ccBreaks, report.fastCCBreaks, report.minCCBreakMs, report.medianCCBreakMs,
                report.trinketCCs, report.fastTrinketCCs, report.minTrinketCCMs, report.medianTrinketCCMs,
                report.fakeCastBites, report.avgLatencyMs);

#ifdef HAS_CHAT_TRANSMITTER
            lines += Acore::StringFormat(
                "\n👤 **{}** (GUID {}) — interrupts {} ({} fast, min {}ms, median {}ms) | "
                "dispels {} ({} fast, min {}ms, median {}ms) | "
                "CC breaks {} ({} fast, min {}ms, median {}ms) | "
                "CC after trinket {} ({} fast, min {}ms, median {}ms) | jukes bitten {} | latency ~{}ms",
                name, report.guidLow,
                report.interrupts, report.fastInterrupts, report.minInterruptMs, report.medianInterruptMs,
                report.dispels, report.fastDispels, report.minDispelMs, report.medianDispelMs,
                report.ccBreaks, report.fastCCBreaks, report.minCCBreakMs, report.medianCCBreakMs,
                report.trinketCCs, report.fastTrinketCCs, report.minTrinketCCMs, report.medianTrinketCCMs,
                report.fakeCastBites, report.avgLatencyMs);
#endif
        }

#ifdef HAS_CHAT_TRANSMITTER
        if (!lines.empty())
        {
            std::string note = Acore::StringFormat(
                "🕵️ **Arena telemetry alert** — match {} ({}v{}{}), reactions ≤ {}ms after latency count as fast\n"
                "Check with `.support arena check {}`{}",
                bg->GetInstanceID(), static_cast<uint32>(bg->GetArenaType()), static_cast<uint32>(bg->GetArenaType()), bg->isRated() ? ", rated" : "",
                _suspectReactionMs, bg->GetInstanceID(), lines);
            sChatTransmitter->QueueNotification("ChatFilter", note);
        }
#endif
    }
}

namespace EnhancedSupport
{
    void LoadArenaTelemetryConfig()
    {
        _telemetryEnabled = sConfigMgr->GetOption<bool>("EnhancedSupport.ArenaTelemetry.Enable", false);
        _telemetryRatedOnly = sConfigMgr->GetOption<bool>("EnhancedSupport.ArenaTelemetry.RatedOnly", true);
        _telemetryRetentionDays = sConfigMgr->GetOption<uint32>("EnhancedSupport.ArenaTelemetry.RetentionDays", 30);

        _telemetryPositionSampleMs = sConfigMgr->GetOption<uint32>("EnhancedSupport.ArenaTelemetry.PositionSampleMs", 500);
        if (_telemetryPositionSampleMs != 0 && _telemetryPositionSampleMs < 100)
        {
            LOG_WARN("module.enhancedsupport",
                "ArenaTelemetry: PositionSampleMs = {} is below the 100ms floor; clamped to 100.",
                _telemetryPositionSampleMs);
            _telemetryPositionSampleMs = 100;
        }

        _telemetryAutoCheck = sConfigMgr->GetOption<bool>("EnhancedSupport.ArenaTelemetry.AutoCheck", false);
        _suspectReactionMs = sConfigMgr->GetOption<uint32>("EnhancedSupport.ArenaTelemetry.Suspect.ReactionMs", 180);
        _suspectMinEvents = sConfigMgr->GetOption<uint32>("EnhancedSupport.ArenaTelemetry.Suspect.MinEvents", 4);
        _suspectPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("EnhancedSupport.ArenaTelemetry.Suspect.Percent", 60), 100);
    }

    bool GetArenaTelemetryEnabled()
    {
        return _telemetryEnabled;
    }

    bool GetArenaTelemetryRatedOnly()
    {
        return _telemetryRatedOnly;
    }

    uint32 GetArenaTelemetryPositionSampleMs()
    {
        return _telemetryPositionSampleMs;
    }

    uint32 GetArenaTelemetryRetentionDays()
    {
        return _telemetryRetentionDays;
    }

    bool GetArenaTelemetryAutoCheck()
    {
        return _telemetryAutoCheck;
    }

    uint32 GetArenaTelemetrySuspectReactionMs()
    {
        return _suspectReactionMs;
    }

    uint32 GetArenaTelemetrySuspectMinEvents()
    {
        return _suspectMinEvents;
    }

    uint32 GetArenaTelemetrySuspectPercent()
    {
        return _suspectPercent;
    }

    std::vector<ArenaTelemetryPlayerReport> CheckArenaMatch(uint32 matchId)
    {
        std::vector<ArenaEventRow> rows;

        // Match analysis is a manual GM action on a few thousand rows, so a
        // synchronous read is fine here.
        QueryResult result = CharacterDatabase.Query(
            "SELECT time_ms, event_type, actor_guid, actor_team, spell_id, target_guid, extra, latency_ms "
            "FROM enhanced_support_arena_events WHERE match_id = {} ORDER BY time_ms, id", matchId);
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                ArenaEventRow row{};
                row.timeMs = fields[0].Get<uint64>();
                row.eventType = fields[1].Get<uint8>();
                row.actorGuid = fields[2].Get<uint32>();
                row.actorTeam = fields[3].Get<uint8>();
                row.spellId = fields[4].Get<uint32>();
                row.targetGuid = fields[5].Get<uint64>();
                row.extra = fields[6].Get<uint32>();
                row.latencyMs = fields[7].Get<uint32>();
                rows.push_back(row);
            } while (result->NextRow());
        }

        // Not flushed yet: the match is still running (or just ending); analyze
        // a copy of its live buffer instead.
        if (rows.empty())
        {
            std::lock_guard<std::mutex> guard(_matchesLock);
            auto it = _matchBuffers.find(matchId);
            if (it != _matchBuffers.end())
                rows = it->second;
        }

        return AnalyzeArenaRows(rows);
    }
}

// Records the cast lifecycle of arena players: cast bars appearing (the
// stimulus an interrupt bot reacts to), cancels (fake casts - a reflex bot
// "kicks" cancelled casts at a fixed delay, humans get juked), and casts
// firing (the responses: interrupts and dispels are instant casts, so their
// CAST_GO timestamp is the reaction time).
class EnhancedSupportArenaSpellTelemetry : public AllSpellScript
{
public:
    EnhancedSupportArenaSpellTelemetry() : AllSpellScript("EnhancedSupportArenaSpellTelemetry", {
        ALLSPELLHOOK_ON_PREPARE,
        ALLSPELLHOOK_ON_CAST_CANCEL,
        ALLSPELLHOOK_ON_CAST
    }) { }

    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* spellInfo) override
    {
        // Only casts with a cast bar (cast time or channel) are interrupt bait;
        // instants are recorded by their CAST_GO event instead.
        if (spell->IsTriggered() || (spell->GetCastTime() <= 0 && !spellInfo->IsChanneled()))
            return;

        Player* player = caster->ToPlayer();
        if (!player)
            return;

        if (Battleground* bg = GetTrackedArena(player))
            BufferEvent(bg, player, ARENA_EVENT_CAST_START, spellInfo->Id,
                spell->m_targets.GetUnitTargetGUID(),
                static_cast<uint32>(std::max<int32>(spell->GetCastTime(), 0)));
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool bySelf) override
    {
        if (spell->IsTriggered())
            return;

        Player* player = caster->ToPlayer();
        if (!player)
            return;

        if (Battleground* bg = GetTrackedArena(player))
            BufferEvent(bg, player, ARENA_EVENT_CAST_CANCEL, spellInfo->Id,
                spell->m_targets.GetUnitTargetGUID(), bySelf ? 1 : 0);
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        if (spell->IsTriggered())
            return;

        Player* player = caster->ToPlayer();
        if (!player)
            return;

        if (Battleground* bg = GetTrackedArena(player))
            BufferEvent(bg, player, ARENA_EVENT_CAST_GO, spellInfo->Id,
                spell->m_targets.GetUnitTargetGUID(), 0);
    }
};

// Records dispellable and loss-of-control auras landing on and leaving arena
// players - the stimuli dispel, CC-break and DoT-reapply bots react to
// (roots/fears for dispels; stuns and fears for trinket / Berserker Rage style
// breaks; a DoT expiring or being dispelled for reapply timing). Rows are
// written from the aura target's perspective: actor = the player the aura is
// on, target_guid = the caster's raw GUID.
class EnhancedSupportArenaAuraTelemetry : public UnitScript
{
public:
    EnhancedSupportArenaAuraTelemetry() : UnitScript("EnhancedSupportArenaAuraTelemetry", true, {
        UNITHOOK_ON_AURA_APPLY,
        UNITHOOK_ON_AURA_REMOVE
    }) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit->ToPlayer();
        if (!player)
            return;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo)
            return;

        // Non-dispellable CC (stuns, ...) matters for CC-break reactions, so it
        // is recorded too; extra keeps carrying the dispel type (0 for those).
        if (spellInfo->Dispel == DISPEL_NONE && !(spellInfo->GetAllEffectsMechanicMask() & CONTROL_MECHANIC_MASK))
            return;

        if (Battleground* bg = GetTrackedArena(player))
            BufferEvent(bg, player, ARENA_EVENT_AURA_APPLY, spellInfo->Id,
                aura->GetCasterGUID(), spellInfo->Dispel);
    }

    // Same aura kinds as OnAuraApply, so every recorded application has its
    // removal; extra carries the AuraRemoveMode (expire, dispel, death, ...).
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        Player* player = unit->ToPlayer();
        if (!player)
            return;

        Aura const* aura = aurApp->GetBase();
        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo)
            return;

        if (spellInfo->Dispel == DISPEL_NONE && !(spellInfo->GetAllEffectsMechanicMask() & CONTROL_MECHANIC_MASK))
            return;

        if (Battleground* bg = GetTrackedArena(player))
            BufferEvent(bg, player, ARENA_EVENT_AURA_REMOVE, spellInfo->Id,
                aura->GetCasterGUID(), mode);
    }
};

// Records server-rejected casts by sniffing outbound SMSG_CAST_FAILED. Only
// verdicts the client cannot predict reach the server (nothing to dispel, line
// of sight, immunity, facing) - a dispel bot firing blind shows up here.
// Attempts the client rejects locally (range on a normal button press,
// cooldown, resource) never produce a packet and stay invisible.
class EnhancedSupportArenaCastFailTelemetry : public ServerScript
{
public:
    EnhancedSupportArenaCastFailTelemetry() : ServerScript("EnhancedSupportArenaCastFailTelemetry", {
        SERVERHOOK_CAN_PACKET_SEND
    }) { }

    bool CanPacketSend(WorldSession* session, WorldPacket const& packet) override
    {
        // Layout (Spell::WriteCastResultInfo): uint8 castCount, uint32 spellId,
        // uint8 result, then optional per-result data.
        if (packet.GetOpcode() != SMSG_CAST_FAILED || packet.size() < 6)
            return true;

        Player* player = session->GetPlayer();
        if (!player)
            return true;

        Battleground* bg = GetTrackedArena(player);
        if (!bg)
            return true;

        uint8 const* data = packet.contents();
        uint32 const spellId = uint32(data[1]) | (uint32(data[2]) << 8) | (uint32(data[3]) << 16) | (uint32(data[4]) << 24);
        uint8 const result = data[5];

        // Suppressed errors come from triggered/internal casts, not from a
        // player-initiated attempt.
        if (result == SPELL_FAILED_DONT_REPORT)
            return true;

        BufferEvent(bg, player, ARENA_EVENT_CAST_FAILED, spellId, ObjectGuid::Empty, result);
        return true;
    }
};

// Samples position and orientation of every live arena player on a fixed
// interval, with their current selection in target_guid. Facing scripts snap
// orientation onto their attacker within one movement packet; over a match the
// samples expose a victim whose frontal arc never lapses while being circled.
class EnhancedSupportArenaPositionSampler : public AllMapScript
{
public:
    EnhancedSupportArenaPositionSampler() : AllMapScript("EnhancedSupportArenaPositionSampler", {
        ALLMAPHOOK_ON_MAP_UPDATE,
        ALLMAPHOOK_ON_DESTROY_MAP
    }) { }

    void OnMapUpdate(Map* map, uint32 diff) override
    {
        if (!_telemetryEnabled || _telemetryPositionSampleMs == 0 || !EnhancedSupport::IsEnabled())
            return;

        if (!map->IsBattleArena())
            return;

        {
            std::lock_guard<std::mutex> guard(_sampleTimersLock);
            uint32& timer = _sampleTimers[map->GetInstanceId()];
            timer += diff;
            if (timer < _telemetryPositionSampleMs)
                return;

            timer = 0;
        }

        Map::PlayerList const& players = map->GetPlayers();
        for (auto itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* player = itr->GetSource();
            if (!player || !player->IsAlive())
                continue;

            if (Battleground* bg = GetTrackedArena(player))
                BufferEvent(bg, player, ARENA_EVENT_POSITION, 0, player->GetTarget(), 0);
        }
    }

    void OnDestroyMap(Map* map) override
    {
        if (!map->IsBattleArena())
            return;

        // Normally the BG-end hook has already flushed; this catches arenas torn
        // down without a normal end.
        FlushMatch(map->GetInstanceId());

        std::lock_guard<std::mutex> guard(_sampleTimersLock);
        _sampleTimers.erase(map->GetInstanceId());
    }
};

// Flushes a match's buffered events once it ends (the round is over, no further
// events are recorded past STATUS_IN_PROGRESS), with the destroy hook as a
// fallback for arenas that never end normally. When AutoCheck is on, the ended
// match is analyzed on its still-in-memory rows first - no DB read - and any
// flagged player is logged and relayed to Discord.
class EnhancedSupportArenaTelemetryFlusher : public AllBattlegroundScript
{
public:
    EnhancedSupportArenaTelemetryFlusher() : AllBattlegroundScript("EnhancedSupportArenaTelemetryFlusher", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY
    }) { }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeamId*/) override
    {
        if (!bg->isArena())
            return;

        uint32 const matchId = bg->GetInstanceID();
        std::vector<ArenaEventRow> rows = TakeMatch(matchId);
        if (rows.empty())
            return;

        if (_telemetryAutoCheck)
            ReportSuspiciousPlayers(bg, AnalyzeArenaRows(rows));

        FlushRows(matchId, rows);
    }

    void OnBattlegroundDestroy(Battleground* bg) override
    {
        if (bg->isArena())
            FlushMatch(bg->GetInstanceID());
    }
};

// Applies the retention window on startup (events older than RetentionDays are
// dropped, keeping the table bounded without a recurring job) and flushes any
// still-buffered matches on clean shutdown.
class EnhancedSupportArenaTelemetryWorldScript : public WorldScript
{
public:
    EnhancedSupportArenaTelemetryWorldScript() : WorldScript("EnhancedSupportArenaTelemetryWorldScript", {
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_SHUTDOWN
    }) { }

    void OnStartup() override
    {
        if (_telemetryRetentionDays == 0)
            return;

        uint64 const cutoff = NowEpochMs() - static_cast<uint64>(_telemetryRetentionDays) * 24 * 60 * 60 * 1000;
        CharacterDatabase.Execute("DELETE FROM enhanced_support_arena_events WHERE time_ms < {}", cutoff);
        LOG_INFO("module.enhancedsupport",
            "ArenaTelemetry: purging arena events older than {} day(s).", _telemetryRetentionDays);
    }

    void OnShutdown() override
    {
        FlushAllMatches();
    }
};

void AddEnhancedSupportArenaTelemetryScripts()
{
    new EnhancedSupportArenaSpellTelemetry();
    new EnhancedSupportArenaAuraTelemetry();
    new EnhancedSupportArenaCastFailTelemetry();
    new EnhancedSupportArenaPositionSampler();
    new EnhancedSupportArenaTelemetryFlusher();
    new EnhancedSupportArenaTelemetryWorldScript();
}
