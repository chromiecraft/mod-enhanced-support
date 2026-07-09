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
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

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
        ARENA_EVENT_CAST_GO     = 3, // spell actually fired (instants included). Interrupt/dispel response.
        ARENA_EVENT_AURA_APPLY  = 4, // dispellable aura landed on a player; extra = dispel type. Dispel-bot stimulus.
        ARENA_EVENT_POSITION    = 5, // periodic position/orientation sample, for facing analysis
    };

    bool _telemetryEnabled = false;
    bool _telemetryRatedOnly = true;
    uint32 _telemetryPositionSampleMs = 500;
    uint32 _telemetryRetentionDays = 30;

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
        uint32 spellId;
        uint64 targetGuid;
        uint32 extra;
        uint32 latencyMs;
        float posX;
        float posY;
        float orientation;
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
                    "(time_ms, match_id, map_id, arena_type, rated, event_type, actor_guid, actor_team, "
                    "spell_id, target_guid, extra, latency_ms, pos_x, pos_y, orientation) VALUES ";
            else
                sql += ", ";

            sql += Acore::StringFormat("({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
                row.timeMs, matchId, row.mapId, static_cast<uint32>(row.arenaType), row.rated ? 1 : 0,
                static_cast<uint32>(row.eventType), row.actorGuid, static_cast<uint32>(row.actorTeam),
                row.spellId, row.targetGuid, row.extra, row.latencyMs,
                row.posX, row.posY, row.orientation);

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

    // Takes a match's buffer out (if any) and flushes it. Safe to call more than
    // once per match; later calls find nothing.
    void FlushMatch(uint32 matchId)
    {
        std::vector<ArenaEventRow> rows;
        {
            std::lock_guard<std::mutex> guard(_matchesLock);
            auto it = _matchBuffers.find(matchId);
            if (it == _matchBuffers.end())
                return;

            rows = std::move(it->second);
            _matchBuffers.erase(it);
        }

        FlushRows(matchId, rows);
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
        row.spellId = spellId;
        row.targetGuid = target.GetRawValue();
        row.extra = extra;
        row.latencyMs = actor->GetSession()->GetLatency();
        row.posX = actor->GetPositionX();
        row.posY = actor->GetPositionY();
        row.orientation = actor->GetOrientation();

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

// Records dispellable auras landing on arena players - the stimulus a dispel
// bot reacts to. The row is written from the aura target's perspective:
// actor = the player the aura landed on, target_guid = the caster's raw GUID.
class EnhancedSupportArenaAuraTelemetry : public UnitScript
{
public:
    EnhancedSupportArenaAuraTelemetry() : UnitScript("EnhancedSupportArenaAuraTelemetry", true, {
        UNITHOOK_ON_AURA_APPLY
    }) { }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        Player* player = unit->ToPlayer();
        if (!player)
            return;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo || spellInfo->Dispel == DISPEL_NONE)
            return;

        if (Battleground* bg = GetTrackedArena(player))
            BufferEvent(bg, player, ARENA_EVENT_AURA_APPLY, spellInfo->Id,
                aura->GetCasterGUID(), spellInfo->Dispel);
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
// fallback for arenas that never end normally.
class EnhancedSupportArenaTelemetryFlusher : public AllBattlegroundScript
{
public:
    EnhancedSupportArenaTelemetryFlusher() : AllBattlegroundScript("EnhancedSupportArenaTelemetryFlusher", {
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_END,
        ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_DESTROY
    }) { }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeamId*/) override
    {
        if (bg->isArena())
            FlushMatch(bg->GetInstanceID());
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
    new EnhancedSupportArenaPositionSampler();
    new EnhancedSupportArenaTelemetryFlusher();
    new EnhancedSupportArenaTelemetryWorldScript();
}
