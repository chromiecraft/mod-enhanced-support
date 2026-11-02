-- IPs exempt from mod-enhanced-support's shared-IP alert (known households and
-- the like), managed with .support ipwhitelist. An entry is an IPv4 address, an
-- IPv4 CIDR range no wider than /16 ("1.2.3.0/24"), or an exact IPv6 address.
CREATE TABLE IF NOT EXISTS `enhanced_support_shared_ip_whitelist` (
    `ip` VARCHAR(64) NOT NULL,
    `note` VARCHAR(255) NOT NULL DEFAULT '',
    `added_by` VARCHAR(32) NOT NULL DEFAULT '',
    `added_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`ip`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
