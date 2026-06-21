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

Blocks player-to-player mail whose subject or body contains any configured
keyword (case-insensitive substring match). Intended to stop gold-seller and
scam advertising. Every block is logged under `module.enhancedsupport`.

The `Action` option controls how matches are handled:

| Action | Behaviour                                            |
| ------ | --------------------------------------------------- |
| `0`    | Disabled (filter off)                               |
| `1`    | Block mail + notify the sender                      |
| `2`    | Block mail + kick the sender                        |
| `3`    | Block mail + permanently ban the sender's account   |
| `4`    | Block mail + permanently ban the account and its IP |

## Configuration

| Option                               | Default   | Description                                                              |
| ------------------------------------ | --------- | ------------------------------------------------------------------------ |
| `EnhancedSupport.Enable`             | `1`       | Master switch for the module's behaviour                                 |
| `EnhancedSupport.MailFilter.Action`  | `0`       | Match handling (0-4, see table above); also gated by `EnhancedSupport.Enable` |
| `EnhancedSupport.MailFilter.Keywords`| (empty)   | Comma-separated keywords matched in the subject/body; commas not allowed  |
| `EnhancedSupport.MailFilter.Message` | (see conf)| System message shown to the sender when `Action = 1`; empty to send none  |

## Layout

- `src/mod_enhanced_support_loader.cpp` — entry point invoked by the core; calls
  the module's `Add*Scripts` registrar.
- `src/mod_enhanced_support.cpp` — register your `ScriptObject` subclasses here.
- `conf/mod-enhanced-support.conf.dist` — distributed config template.
- `data/sql/db-world/base` and `data/sql/db-world/updates` — SQL applied
  automatically by the module DB updater.

## License

Released under the GNU AGPL v3, consistent with AzerothCore.
