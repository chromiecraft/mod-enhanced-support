-- Email substrings matched against an account's email when a character is created by mod-enhanced-support.
-- Stored lowercased; matching is case-insensitive substring against the account email.
CREATE TABLE IF NOT EXISTS `enhanced_support_email_patterns` (
    `pattern` VARCHAR(255) NOT NULL,
    PRIMARY KEY (`pattern`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
