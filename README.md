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

Enable `EnhancedSupport.MailFilter.SkipSameAccount` to bypass the mail checks for
mail between two characters on the same account (sending to your own alt). It
skips both the keyword filter and the high-value gold logging, since a
self-transfer is never advertising or gold selling.

### Chat keyword filter

Scans player `SAY`, `YELL`, `EMOTE`, `PARTY` and `WHISPER` messages against the
same keyword list as the mail filter (case-insensitive substring match) and
applies the same escalating actions, configured separately via
`EnhancedSupport.ChatFilter.Action`. Blocked messages are logged under
`module.enhancedsupport`.

The `SAY`/`YELL`/`EMOTE` hook the core exposes runs before the message is
broadcast but cannot abort it, so a matched message has its text cleared rather
than being dropped outright. Party and whisper chat use separate hooks that do
abort the broadcast, so a matched message there is dropped entirely. With the
kick/ban actions the sender is removed regardless.

| Action | Behaviour                                               |
| ------ | ------------------------------------------------------- |
| `0`    | Disabled (filter off)                                   |
| `1`    | Block message + notify the sender                       |
| `2`    | Block message + kick the sender                         |
| `3`    | Block message + permanently ban the sender's account    |
| `4`    | Block message + permanently ban the account and its IP  |

### Aggressive low-level pass

Both the mail and chat filters match keywords as contiguous substrings, so
spammers evade them by spacing letters out (`B U Y G O L D . C O M`). Collapsing
whitespace before matching would catch that, but it also fuses perfectly normal
phrases: a group-finding message typed with spaces collapses into a single solid
token that can contain a blocked keyword as a substring, false-positiving
legitimate chat.

The aggressive pass (`EnhancedSupport.AggressiveMaxLevel`) runs the
whitespace-collapsed match for both mail and chat, additionally folding common
look-alike character substitutions back to letters so digit-disguised text reads
as its plain form. It runs only when **both** of these hold:

- the sender is at or below the configured level (gold bots are throwaway
  low-level characters; real raiders advertising `LFG tank for Onyxia` are not), and
- the text also contains a web marker (`.com`, `www`, `http`, `dot com`, ...).

`LFG tank for Onyxia` from a level 70 fails the level gate; `LFG tank for RFC`
from a new player fails the web-marker gate; the spam line passes both. Matches
reuse each filter's own action (`MailFilter.Action` / `ChatFilter.Action`). Set
`AggressiveMaxLevel` to `0` to disable it.

### Cross-message chat window

Both passes above look at a single message, so spammers split an advertisement
across several lines, padding it with unrelated chatter so that no single line is
actionable on its own. No one line carries a complete URL, so neither the strict
nor the aggressive pass fires.

The windowed pass keeps a short, time-bounded history of each sender's recent
chat lines and re-runs the strict and aggressive matches over the joined text,
reassembling the fragments so the match fires.

It is controlled by `EnhancedSupport.ChatFilter.WindowSize` (max lines kept per
sender) and `EnhancedSupport.ChatFilter.WindowSeconds` (how long a line stays in
the window). It only runs when `WindowSize` is at least `2`, `WindowSeconds` is
set, and the sender is at or below `EnhancedSupport.AggressiveMaxLevel`, so it
inherits the same low-level + web-marker safeguards. Because the core hook for
`SAY`/`YELL`/`EMOTE` cannot recall lines already broadcast, only the final
fragment is blanked; the earlier lines have gone out, but the URL never completes
and the full joined text is logged and the configured action applied. The history
is pruned by age and size on each message and cleared on logout.

### Party-invite spam filter

Spammers also create throwaway low-level characters and mass-invite strangers to a
group. A party invite carries no text to match, so this filter acts on the rate
instead: it blocks a sender's invite once more than `RateCount` invites from them
fall within the last `RateSeconds`. The core drops a blocked invite silently, so
the spammer sees no error and the target gets no popup. The same escalating actions
are available through a dedicated `EnhancedSupport.InviteFilter.Action` (separate
from the chat and mail filters).

To keep legitimate grouping clear, invites to a guildmate, to a player who has the
inviter on their friends list, or to a player on the same IP (same household or
multiboxer) are never counted. `InviteFilter.MaxLevel`
restricts the filter to low-level inviters (`0` watches every level). The filter
runs only when `InviteFilter.Action`, `RateCount` and `RateSeconds` are all set.
Per-sender invite history is pruned on each invite and cleared on logout.

### Underlevel loot logger

Logs when a character loots an item whose required level exceeds their own by at
least `EnhancedSupport.LootFilter.LevelGap` levels, for example a level-5
character pulling high-level gear from a world chest in Burning Steppes. Intended
to surface boosting or loot exploits. This is log-only: the loot is never
blocked. Items with no level requirement are ignored, as is anything pulled from
a fishing bobber or fishing hole (fish are caught above your level by design).
Entries are written under `module.enhancedsupport` (and relayed to Discord when
mod-chat-transmitter is available), and identify the looter, the item, the level
gap, the loot source (its type, template entry and DB spawn id), the looter's
zone/subarea and whether they were in a group at the time. In the Discord notice
the item and the source creature/object are linked to their aowow database pages.

`EnhancedSupport.LootFilter.MaxLevel` optionally caps the check to looters at or
below a given level (e.g. `10` to watch only characters up to level 10); `0`
applies it to every level.

When a looter pulls several flagged items from one container or creature, each is
its own Discord notification by default. Set `EnhancedSupport.LootFilter.BatchSeconds`
to coalesce them into a single message, sent once no further item has arrived from
that source for that many seconds (or when the looter switches source or logs out).
This affects the Discord notification only; the server log keeps one line per item.

### Account email pattern logger

Logs when a newly created character belongs to an account whose email contains
any configured substring pattern (case-insensitive). Intended to surface
bot/gold-seller accounts that register with recognizable email patterns. This is
log-only: character creation is never blocked. Matches are written under
`module.enhancedsupport` and, when mod-chat-transmitter is available, relayed to
Discord (sharing the chat filter's channel), identifying the new character, its
account and the matched pattern.

Patterns live in the auth DB table `enhanced_support_email_patterns`, managed with
the `.support emailpattern` commands below and shared across all realms, like the
mail keywords. `EnhancedSupport.EmailFilter.Enable` is a master switch for the check
(default on); when enabled it runs only once at least one pattern is configured.

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
| `.support info`                          | Game Master   | Shows active settings: enabled state, mail and chat filter actions, keyword and email-pattern counts, ban author, message. |
| `.support action <0-4>`                  | Administrator | Overrides the mail filter action at runtime (not saved; reverts on `.support reload` or restart). |
| `.support reload`                        | Administrator | Reloads this module's config and keywords, independent of the global `.reload config`.       |
| `.support keyword add <keyword>`         | Administrator | Adds a blocked keyword (stored lowercased).                                                   |
| `.support keyword remove <keyword>`      | Administrator | Removes a blocked keyword.                                                                    |
| `.support list keywords`                 | Game Master   | Lists the blocked keywords.                                                                   |
| `.support emailpattern add <pattern>`    | Administrator | Adds an email substring pattern (stored lowercased).                                          |
| `.support emailpattern remove <pattern>` | Administrator | Removes an email pattern.                                                                     |
| `.support list emailpatterns`            | Game Master   | Lists the configured email patterns.                                                          |
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
| `EnhancedSupport.LootFilter.LevelGap` | `0`      | Log loot whose required level exceeds the looter's level by at least this gap; `0` disables |
| `EnhancedSupport.LootFilter.MaxLevel` | `0`      | Cap the loot check to looters at or below this level; `0` applies to all levels |
| `EnhancedSupport.EmailFilter.Enable` | `1`       | Master switch for the account email-pattern check at character creation; runs once at least one pattern is configured |
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
  automatically by the module DB updater. The mail keyword and email pattern
  tables live in `db-auth/updates`; the `.support` command help rows in
  `db-world/updates`.

## License

Released under the GNU AGPL v3, consistent with AzerothCore.
