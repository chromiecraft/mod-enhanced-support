-- Per-account last world login and IP, kept by mod-enhanced-support as the
-- dormant-login baseline. The account table cannot be used for this: the
-- authserver overwrites account.last_ip/last_login before the worldserver
-- sees the login.
CREATE TABLE IF NOT EXISTS `enhanced_support_account_activity` (
    `account_id` INT UNSIGNED NOT NULL,
    `last_seen` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `last_ip` VARCHAR(45) NOT NULL DEFAULT '',
    PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Seed the baseline from the account table so already-dormant accounts are
-- covered from the first login after this update, not from their second.
INSERT IGNORE INTO `enhanced_support_account_activity` (`account_id`, `last_seen`, `last_ip`)
SELECT `id`, `last_login`, `last_ip` FROM `account` WHERE `last_login` IS NOT NULL;
