# mod-enhanced-support

An AzerothCore module scaffold. Drop your support-related behaviour into
`src/` and wire it through `Addmod_enhanced_supportScripts()`.

## Installation

1. Place this folder under `modules/` in your AzerothCore source tree.
2. Re-run CMake and rebuild. The module is auto-discovered (no manual CMake
   edits required for a static build).
3. Copy `conf/mod-enhanced-support.conf.dist` to your config directory as
   `mod-enhanced-support.conf` and adjust as needed. If the file is absent the
   built-in defaults are used.

## Features

### Mail keyword filter

Blocks player-to-player mail whose subject or body contains any blocked
keyword (case-insensitive substring match). Intended to stop gold-seller and
scam advertising. Blocked mail (including its subject and body) is logged under
`module.enhancedsupport`.

Keywords live in the auth DB table `enhanced_support_mail_keywords` and are
managed with the `.support keyword` commands below; no rebuild or config edit is
needed to change them. Because the table is in the auth DB, the keyword list is
shared across all realms.

The `Action` option controls how matches are handled:

| Action | Behaviour                                            |
| ------ | --------------------------------------------------- |
| `0`    | Disabled (filter off)                               |
| `1`    | Block mail + notify the sender                      |
| `2`    | Block mail + kick the sender                        |
| `3`    | Block mail + permanently ban the sender's account   |
| `4`    | Block mail + permanently ban the account and its IP |

### Startup Discord notice

When enabled, posts a decorated Discord message once the world server has
started, showing the git revision (hash, branch and build date) the server is
running on. This uses `mod-chat-transmitter` as the transport, so that module
must be installed and enabled; without it the notice
is skipped and a warning is logged. The notice is scheduled (via
`TaskScheduler`) a few seconds after startup so the relay's WebSocket has come
up first; the delay is configurable.

## Commands

All commands live under `.support` and work in-game and from the server console.

| Command                                  | Security      | Description                                                                                  |
| ---------------------------------------- | ------------- | -------------------------------------------------------------------------------------------- |
| `.support info`                          | Game Master   | Shows active settings: enabled state, mail filter action, keyword count, ban author, message. |
| `.support action <0-4>`                  | Administrator | Overrides the mail filter action at runtime (not saved; reverts on `.support reload` or restart). |
| `.support reload`                        | Administrator | Reloads this module's config and keywords, independent of the global `.reload config`.       |
| `.support keyword add <keyword>`         | Administrator | Adds a blocked keyword (stored lowercased).                                                   |
| `.support keyword remove <keyword>`      | Administrator | Removes a blocked keyword.                                                                    |
| `.support list keywords`                 | Game Master   | Lists the blocked keywords.                                                                   |
| `.support list bans [count] [author]`    | Game Master   | Lists the most recent account bans (newest first). `count` defaults to 10, max 50; `author` filters by the ban author substring and defaults to `EnhancedSupport.MailFilter.BanAuthor`, so a bare call shows the module's own bans. |

Examples: `.support keyword add wowgold`, `.support list keywords`,
`.support list bans` (module bans), `.support list bans 50 GM_Name`.

## Configuration

| Option                               | Default   | Description                                                              |
| ------------------------------------ | --------- | ------------------------------------------------------------------------ |
| `EnhancedSupport.Enable`             | `1`       | Master switch for the module's behaviour                                 |
| `EnhancedSupport.MailFilter.Action`  | `0`       | Match handling (0-4, see table above); also gated by `EnhancedSupport.Enable` |
| `EnhancedSupport.MailFilter.Message` | (see conf)| System message shown to the sender when `Action = 1`; empty to send none  |
| `EnhancedSupport.MailFilter.BanAuthor`| `SupportModule` | Author recorded on Action 3/4 bans; also the default `.support list bans` search term |
| `EnhancedSupport.StartupNotice.Enable` | `0`     | Post a Discord notice with the git revision on server start (needs mod-chat-transmitter) |
| `EnhancedSupport.StartupNotice.Message`| `Server restarted!` | Headline for the startup notice; the full version line is shown below it in a code block |
| `EnhancedSupport.StartupNotice.DelaySeconds`| `5`  | Seconds to wait after startup before sending, so the relay's WebSocket is up |

## Layout

- `src/mod_enhanced_support_loader.cpp` — entry point invoked by the core; calls
  the module's `Add*Scripts` registrars.
- `src/mod_enhanced_support.cpp` — world/player scripts (config cache, mail filter).
- `src/cs_enhanced_support.cpp` — the `.support` chat commands.
- `src/EnhancedSupport.h` — shared config/keyword API used by both `.cpp` files.
- `conf/mod-enhanced-support.conf.dist` — distributed config template.
- `data/sql/db-auth`, `data/sql/db-world` — `base`/`updates` SQL applied
  automatically by the module DB updater. The mail keyword table lives in
  `db-auth/updates`; the `.support` command help rows in `db-world/updates`.

## License

Released under the GNU AGPL v3, consistent with AzerothCore.
