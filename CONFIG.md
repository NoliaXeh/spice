# Spice Configuration

## Format: TOML

Spice's configuration is declarative data in TOML. It is not a scripting language.

That is a deliberate architectural choice, and it follows from the rest of the design rather than
from taste. Spice's whole premise is that *logic lives in plugins, in any language, out of process*.
Embedding a scripting runtime in the core to configure it would contradict that, and would drag a VM
into a C++ core that deliberately does not have one. If you want behaviour, write a plugin.

TOML over the alternatives:

|      |                                                                                                                                                                                   |
| ---- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| TOML | Comments, typed, small unambiguous spec. `[[plugin]]` arrays-of-tables map exactly onto a plugin list. Header-only C++ library (`toml++`). Familiar from Helix, Alacritty, Cargo. |
| YAML | Large spec, indentation-sensitive, real footguns (`no` → `false`; version numbers → floats). Buys nothing here.                                                                   |
| JSON | No comments, no trailing commas. Fine as a wire format, hostile to hand-edit.                                                                                                     |
| KDL  | Genuinely a nice fit, but thin C++ library support - we would end up maintaining a parser instead of Spice.                                                                       |
| INI  | No nesting, no arrays. Too weak for plugin definitions.                                                                                                                           |

## Two files, two owners

This split matters, and it exists for a specific reason.

The palette can bind a key to a command with `SHIFT-RETURN` - which means **Spice writes config back
to disk**. No TOML serialiser round-trips a hand-written file faithfully: the first time a user
presses `SHIFT-RETURN`, their comments, ordering, and formatting would be destroyed. That is a
reliable way to lose someone's trust in an editor.

So:

| file            | owner    | Spice writes it?                                                                          |
| --------------- | -------- | ----------------------------------------------------------------------------------------- |
| `config.toml`   | the user | Never. Not once.                                                                          |
| `keybinds.toml` | Spice    | Yes - regenerated freely. Comments are not preserved, because there are none to preserve. |

Users who prefer to hand-write keybinds still can, in `config.toml`. The ones Spice writes for them
land in `keybinds.toml`.

### Precedence

`config.toml` wins. Explicit user intent beats generated state.

Anytime a conflict appears, the conflicting setting in `keybinds.toml` is removed.

If a user tries to bind a key from the palette that is already bound in `config.toml`, the bind is
refused, with a message saying where the existing binding lives.

### Locations

```
$XDG_CONFIG_HOME/spice/config.toml     # user-owned
$XDG_STATE_HOME/spice/keybinds.toml    # Spice-owned
```

Falling back to `~/.config/spice/` and `~/.local/state/spice/`. Machine-written state lives under
*state*, not *config*, because that is what it is.

## `config.toml`

```toml
# ---------------------------------------------------------------
# Keys
# ---------------------------------------------------------------

[keys]
# The Master key is a PREFIX, not a mode: Master-then-key reaches Spice,
# and every other key passes straight through to the pane - including to a
# child program in a PTYPane. That is how a PTYPane can run vim without
# Spice swallowing its keys.
#
# It is deliberately not ESCAPE: terminal escape sequences (arrows, function
# keys, alt-combos) are themselves ESC-prefixed, so a bare ESC can only be
# told apart from the start of a sequence by a timeout - which is either
# laggy or occasionally wrong.
master = "ctrl-space"

palette_run  = "return"        # run the selected command
palette_bind = "shift-return"  # bind a key to the selected command

# ---------------------------------------------------------------
# Plugins
# ---------------------------------------------------------------

[[plugin]]
name    = "files"
command = ["spice-files"]
mode    = "pane"               # "pane" | "global"
restart = "on-crash"

[[plugin]]
name    = "lsp"
command = ["/usr/local/bin/spice-lsp", "--config", "~/.config/spice/lsp.toml"]
mode    = "global"
restart = "on-crash"
max_restarts   = 3             # within restart_window; then give up and tell the user
restart_window = "60s"

[[plugin]]
name    = "scratch-experiment"
command = ["python3", "~/dev/spice-scratch/main.py"]
mode    = "global"
restart = "never"              # crashing every 200ms while you debug it is not helpful

# ---------------------------------------------------------------
# Hand-written keybinds (optional - these take precedence over
# anything bound from the palette)
# ---------------------------------------------------------------

[[keybind]]
key     = "ctrl-p"
plugin  = "files"
command = "open"

# ---------------------------------------------------------------

[lifecycle]
shutdown_grace = "2s"          # shutdown event → grace → SIGTERM → SIGKILL
sigterm_grace  = "1s"

[log]
level = "info"                 # trace | debug | info | warn | error
file  = "$XDG_STATE_HOME/spice/spice.log"
```

### Plugin fields

| field            | required | meaning                                                                                                                                                   |
| ---------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `name`           | yes      | Identifier. Owns the matching **topic prefix** for broadcast (`lsp.*`) and the matching **error-code prefix** (`lsp.server_unavailable`). Must be unique. |
| `command`        | yes      | Path + argv. Run as an ordinary subprocess.                                                                                                               |
| `mode`           | yes      | `pane` - one instance per pane. `global` - one instance watching everything.                                                                              |
| `restart`        |          | `never` · `on-crash` (default) · `always`                                                                                                                 |
| `max_restarts`   |          | Give up after this many restarts inside `restart_window`, and tell the user. Stops a crash-loop from pinning a CPU.                                       |
| `restart_window` |          | Default `60s`.                                                                                                                                            |

Note that `name` is doing real work: it is the plugin's namespace in *two* places in the protocol
(broadcast topics, error codes), which is why it must be unique and why it is not merely a label.

### Keybinds

Binds target **`(plugin, command)` by name** - never a command id. Plugins register commands
dynamically, so an id-based bind would be orphaned by every plugin restart.

A bind whose plugin is not running, or whose command is not currently registered, is a **no-op that
tells the user why** (*"plugin `lsp` is not running"*). It is not an error, and it is not silently
dropped.

## `keybinds.toml`

Machine-owned. Written by Spice when you bind a key from the palette. Safe to delete; safe to
ignore. Do not put comments in it - they will not survive.

```toml
# Generated by Spice. Do not edit; edit config.toml instead.

[[keybind]]
key     = "ctrl-g"
plugin  = "git"
command = "blame"
```

## Reloading

Config is read at startup. There is no live reload in v1.

`Restart plugin` re-reads that plugin's entry, which covers the common case of iterating on a plugin
you are writing. Changing the Master key or the plugin list requires a restart of Spice.
