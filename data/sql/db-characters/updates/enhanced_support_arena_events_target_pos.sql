-- Position/orientation of the event's target unit at event time, so facing
-- checks (behind-arc abilities, facing bots) don't depend on joining the
-- nearest position sample. All zero when the event has no resolvable unit
-- target.
ALTER TABLE `enhanced_support_arena_events`
    ADD COLUMN `tgt_x` FLOAT NOT NULL DEFAULT 0 AFTER `orientation`,
    ADD COLUMN `tgt_y` FLOAT NOT NULL DEFAULT 0 AFTER `tgt_x`,
    ADD COLUMN `tgt_o` FLOAT NOT NULL DEFAULT 0 AFTER `tgt_y`;
