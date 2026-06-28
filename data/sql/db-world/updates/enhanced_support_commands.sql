DELETE FROM `command` WHERE `name` IN (
    'support',
    'support info',
    'support action',
    'support reload',
    'support list',
    'support list bans',
    'support list keywords',
    'support list emailpatterns',
    'support keyword',
    'support keyword add',
    'support keyword remove',
    'support emailpattern',
    'support emailpattern add',
    'support emailpattern remove'
);

INSERT INTO `command` (`name`, `security`, `help`) VALUES
('support',                2, 'Syntax: .support info|list|keyword|emailpattern|reload\nmod-enhanced-support administration commands.'),
('support info',           2, 'Syntax: .support info\nShows the module''s active settings: enabled state, mail filter action, keyword and email-pattern counts, ban author and notify message.'),
('support action',         3, 'Syntax: .support action $action\nOverrides the mail filter action at runtime (not saved; reverts on reload).\n$action: 0=off, 1=notify, 2=kick, 3=ban account, 4=ban account+IP.'),
('support reload',         3, 'Syntax: .support reload\nReloads mod-enhanced-support config, mail keywords and email patterns from disk/DB, independent of .reload config.'),
('support list',           2, 'Syntax: .support list bans|keywords|emailpatterns\nListing commands for mod-enhanced-support.'),
('support list bans',      2, 'Syntax: .support list bans [$count] [$author]\nLists the most recent account bans, newest first.\n$count defaults to 10 (max 50). $author filters by ban author substring and defaults to the module ban author, so a bare call shows the bans this module issued.'),
('support list keywords',  2, 'Syntax: .support list keywords\nLists the mail keywords currently blocked by the mail filter.'),
('support list emailpatterns', 2, 'Syntax: .support list emailpatterns\nLists the email substring patterns matched against an account''s email at character creation.'),
('support keyword',        3, 'Syntax: .support keyword add|remove $keyword\nManages the mail filter keyword list.'),
('support keyword add',    3, 'Syntax: .support keyword add $keyword\nAdds a keyword to the mail filter. Stored lowercased; matching is case-insensitive against the mail subject and body.'),
('support keyword remove', 3, 'Syntax: .support keyword remove $keyword\nRemoves a keyword from the mail filter.'),
('support emailpattern',        3, 'Syntax: .support emailpattern add|remove $pattern\nManages the email pattern list matched at character creation.'),
('support emailpattern add',    3, 'Syntax: .support emailpattern add $pattern\nAdds an email substring pattern. Stored lowercased; matching is case-insensitive against the account email.'),
('support emailpattern remove', 3, 'Syntax: .support emailpattern remove $pattern\nRemoves an email pattern.');
