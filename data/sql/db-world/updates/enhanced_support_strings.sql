-- Player-facing texts of the dormant-login lock, localized per client language.
-- 90101: red warning shown to the locked player at login and on every blocked
--        mail, trade or auction house attempt.
-- 90102: shown to the other player when they try to trade with a locked account.
DELETE FROM `acore_string` WHERE `entry` IN (90101, 90102);
INSERT INTO `acore_string` (`entry`, `content_default`, `locale_koKR`, `locale_frFR`, `locale_deDE`, `locale_zhCN`, `locale_zhTW`, `locale_esES`, `locale_esMX`, `locale_ruRU`) VALUES
(90101,
 'Suspicious activity has been detected on your account. The account will be locked - please open a Discord ticket to verify your identity and restore access.',
 '계정에서 의심스러운 활동이 감지되었습니다. 계정이 잠기게 됩니다. 신원 확인과 접속 복구를 위해 Discord 티켓을 열어 주십시오.',
 'Une activité suspecte a été détectée sur votre compte. Le compte va être verrouillé - veuillez ouvrir un ticket Discord pour vérifier votre identité et rétablir l''accès.',
 'Auf Eurem Account wurde verdächtige Aktivität festgestellt. Der Account wird gesperrt - bitte öffnet ein Discord-Ticket, um Eure Identität zu bestätigen und den Zugriff wiederherzustellen.',
 '检测到您的账号存在可疑活动。该账号即将被锁定 - 请在 Discord 开启工单以验证您的身份并恢复访问。',
 '偵測到您的帳號存在可疑活動。該帳號即將被鎖定 - 請在 Discord 開立工單以驗證您的身分並恢復存取。',
 'Se ha detectado actividad sospechosa en tu cuenta. La cuenta será bloqueada - por favor, abre un ticket de Discord para verificar tu identidad y restaurar el acceso.',
 'Se detectó actividad sospechosa en tu cuenta. La cuenta será bloqueada - por favor, abre un ticket de Discord para verificar tu identidad y restaurar el acceso.',
 'На вашей учётной записи обнаружена подозрительная активность. Учётная запись будет заблокирована - пожалуйста, откройте тикет в Discord, чтобы подтвердить свою личность и восстановить доступ.'),
(90102,
 'That player cannot trade right now.',
 '해당 플레이어는 지금 거래할 수 없습니다.',
 'Ce joueur ne peut pas commercer pour le moment.',
 'Dieser Spieler kann derzeit nicht handeln.',
 '该玩家目前无法交易。',
 '該玩家目前無法交易。',
 'Ese jugador no puede comerciar ahora mismo.',
 'Ese jugador no puede comerciar en este momento.',
 'Этот игрок сейчас не может торговать.');
