-- Arena telemetry events recorded by mod-enhanced-support for offline cheat
-- detection (reaction-time and facing analysis). One row per event; the
-- module README documents the event types and per-type column semantics.
-- Dropped first so re-running this file picks up schema changes; recorded
-- telemetry is evidence data bounded by the retention window, not state to keep.
DROP TABLE IF EXISTS `enhanced_support_arena_events`;
CREATE TABLE `enhanced_support_arena_events` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `time_ms` BIGINT UNSIGNED NOT NULL COMMENT 'event time, unix epoch milliseconds',
    `match_id` INT UNSIGNED NOT NULL COMMENT 'battleground instance id',
    `map_id` SMALLINT UNSIGNED NOT NULL,
    `arena_type` TINYINT UNSIGNED NOT NULL COMMENT '2 / 3 / 5',
    `rated` TINYINT UNSIGNED NOT NULL,
    `event_type` TINYINT UNSIGNED NOT NULL COMMENT '1 cast start, 2 cast cancel, 3 cast, 4 aura apply, 5 position sample, 6 aura remove, 7 cast failed',
    `actor_guid` INT UNSIGNED NOT NULL COMMENT 'player low GUID',
    `actor_team` TINYINT UNSIGNED NOT NULL COMMENT 'BgTeamId: which side of this match (0/1), not persistent',
    `arena_team_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'persistent rated arena team id; 0 for unrated',
    `spell_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `target_guid` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'raw ObjectGuid: spell target / aura caster / current selection',
    `extra` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'cast time ms / cancelled-by-self flag / dispel type / aura remove mode / cast fail reason',
    `latency_ms` INT UNSIGNED NOT NULL DEFAULT 0,
    `pos_x` FLOAT NOT NULL DEFAULT 0,
    `pos_y` FLOAT NOT NULL DEFAULT 0,
    `orientation` FLOAT NOT NULL DEFAULT 0,
    `tgt_x` FLOAT NOT NULL DEFAULT 0 COMMENT 'target unit position/orientation at event time; 0 when no unit target',
    `tgt_y` FLOAT NOT NULL DEFAULT 0,
    `tgt_o` FLOAT NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_actor_time` (`actor_guid`, `time_ms`),
    KEY `idx_match` (`match_id`, `time_ms`),
    KEY `idx_arena_team` (`arena_team_id`, `time_ms`),
    KEY `idx_time` (`time_ms`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
