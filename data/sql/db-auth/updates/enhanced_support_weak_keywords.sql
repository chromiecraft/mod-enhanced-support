-- Weak keywords for mod-enhanced-support's scored spam pass: contextual words
-- that add points next to a real keyword hit but never act alone.
-- Stored lowercased; matched case-insensitively with separators ignored.
CREATE TABLE IF NOT EXISTS `enhanced_support_weak_keywords` (
    `keyword` VARCHAR(255) NOT NULL,
    PRIMARY KEY (`keyword`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
