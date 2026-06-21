-- Keywords scanned in player-to-player mail by mod-enhanced-support.
-- Stored lowercased; matching is case-insensitive substring against subject + body.
CREATE TABLE IF NOT EXISTS `enhanced_support_mail_keywords` (
    `keyword` VARCHAR(255) NOT NULL,
    PRIMARY KEY (`keyword`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
