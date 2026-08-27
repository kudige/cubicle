# cube(1)

## Name

`cube` - run, reconnect to, inspect, and control Cubicle-managed processes.

## Synopsis

```text
cube [--config PATH] [--manager-socket ENDPOINT] [--workspace NAME] [--json] COMMAND [ARG...]
cube help
cube workspace [NAME]
cube workspace list
cube workspace create [--dir DIRECTORY] NAME
cube workspace select NAME
cube workspace stop NAME
cube workspace delete NAME
cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIRECTORY] [--restart] COMMAND [ARG...]
cube ps [-a|--all]
cube inspect NAME
cube logs [--follow] [--stdout|--stderr] [--start N] [--end N] NAME
cube events [--follow [--iterations N]]
cube connect [--ro] NAME
cube push [--eof] [--output] NAME
cube update NAME|PATTERN [--restart|--no-restart|--name NAME]
cube restart NAME|PATTERN
cube stop NAME
cube kill [--all] [--cleanup] [NAME|PATTERN]
cube signal NAME SIGNAL
cube save NAME|PATTERN
cube unsave NAME|PATTERN
cube remove NAME
cube cleanup [NAME|PATTERN]
cube shutdown [--manager-only]
cube access list
cube access add PUBLIC_KEY_OR_FILE [--role observer|operator|owner] [--label LABEL]
cube access set-role KEY_ID observer|operator|owner
cube access remove KEY_ID
cube access revoke KEY_ID
cube remote list
cube remote add NAME tls://host:port [--yes]
cube remote inspect NAME
cube remote remove NAME
cube config show
cube config effective
cube config paths
cube config validate
```

## Description

`cube` is the current user-facing Cubicle CLI. It talks to a running
`cubicle-manager`, starts commands through per-process controllers, and lets a
user reconnect to managed processes without tying process lifetime to the
terminal that launched them.

This page describes the CLI as implemented in the current code. The broader
future command design is tracked separately in `docs/cli-command-spec.md`.

## Global Options

`--manager-socket ENDPOINT`
: Manager endpoint to use for this invocation. It may be a Unix socket path,
  a `unix:///absolute/path.sock` URI, or a `tcp://host:port` URI when the
  manager is listening on TCP.

`--workspace NAME`
: Use `NAME` for commands that operate on a workspace. This overrides the
  locally selected workspace for the command.

`--json`
: Print JSON results for commands that support it. Most command JSON output is
  the raw manager API result. Some successful lifecycle commands print `{}`.

`--config PATH`
: Load `PATH` as a full development/test configuration override. This is
  equivalent to setting `CUBICLE_CONFIG=PATH` for the command.

`help`, `--help`, `-h`
: Print top-level help.

## Manager Selection

`cube` resolves its manager endpoint in this order:

1. `--manager-socket ENDPOINT`
2. `CUBICLE_MANAGER_SOCKET`
3. `[client] manager=...` from the loaded Cubicle configuration
4. built-in/configured defaults

Installed packages provide `/usr/lib/cubicle/config.cfg`. Administrators can
override through system configuration handled by `libeconf`, such as
`/etc/cubicle/config.cfg`. User configuration is read from
`$XDG_CONFIG_HOME/cubicle/config.cfg`, or `~/.config/cubicle/config.cfg` when
`XDG_CONFIG_HOME` is unset. Passing `--config /path/to/config.cfg` or setting
`CUBICLE_CONFIG=/path/to/config.cfg` makes `cube` load that exact file. Each
loaded `config.cfg` also applies lexical
`config.cfg.d/*.cfg` drop-ins when that directory exists.

Examples:

```sh
cube --manager-socket /run/user/$UID/cubicle/manager.sock ps
cube --manager-socket unix:///tmp/cubicle/manager.sock workspace Dev
cube --manager-socket tcp://127.0.0.1:7777 ps
CUBICLE_CONFIG=/tmp/cubicle.cfg cube config paths
cube --config /tmp/cubicle.cfg config paths
```

## Workspaces

Most process commands require a selected workspace or an explicit
`--workspace NAME`.

`cube workspace NAME`
: Select an existing workspace named `NAME`, or create it if it does not exist.

`cube workspace`
: Print the locally selected workspace.

`cube workspace list`
: List workspaces.

`cube workspace create [--dir DIRECTORY] NAME`
: Create or select `NAME`. In the current implementation this has the same
  create-if-missing behavior as `cube workspace NAME`. A newly created
  workspace stores `DIRECTORY` as its default directory; without `--dir`, the
  current directory is stored.

`cube workspace select NAME`
: Select or create `NAME`.

`cube workspace stop NAME`
: Stop the workspace through the manager.

`cube workspace delete NAME`
: Delete the workspace through the manager. The current CLI asks the manager to
  remove retained processes and not stop running processes.

Examples:

```sh
cube workspace ProjectA
cube workspace create --dir "$PWD" ProjectA
cube workspace list
cube --json workspace list
cube workspace stop OldProject
cube workspace delete OldProject
```

The selected workspace is stored client-side under the user's Cubicle state
area. If no workspace is selected, workspace-scoped commands use the first
workspace returned by the manager.

`desk` keeps its own explicit default workspace and layout. With no explicit
desk default, it follows the selected `cube` workspace; once a named desk layout
is saved or loaded, no-argument `desk` starts from that layout until another
workspace is opened in desk.

## Running Processes

```text
cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIRECTORY] [--restart] COMMAND [ARG...]
```

`cube run` starts a managed process in the current workspace.

`--fg`
: Start in the foreground. This is the built-in launch default unless changed
  by configuration. For `stream` mode, `cube` waits for completion and exits
  with the managed process exit status when possible. For `tty` and `term`
  modes, `cube` attaches the terminal.

`--bg`
: Start in the background and return after successful startup.

`--stream`
: Use separate stdin, stdout, and stderr pipes.

`--tty`
: Use one PTY for terminal-style stdin/stdout/stderr.

`--term`
: Use terminal-style split capture. In the current implementation,
  stdin/stderr share the controlling PTY and stdout is captured through a
  second PTY.

`--name NAME`
: Set the process friendly name. Without `--name`, `cube` derives a name from
  the executable basename. If a generated name already exists, `cube` tries
  suffixes such as `sleep-1`, `sleep-2`, up to its internal limit.

`--dir DIRECTORY`
: Set the managed process working directory. Without `--dir`, the process uses
  the selected workspace's default directory.

`--restart`
: Mark the process for manager-start autostart. When `cubicle-manager` starts,
  it relaunches restart-enabled cubes whose controllers are not already live.
  The manager also recreates the recorded workspace if that workspace record is
  missing.

Use `--` before a command whose first argument might otherwise be parsed as a
`cube run` option.

Examples:

```sh
cube run --stream make test
cube run --dir /tmp --stream pwd
cube run --fg --term vim docs/plan.txt
cube run --bg --term --name shell bash
cube run --bg --restart --name worker ./worker
cube run --bg --stream ls -lR /
cube run --bg --name server -- npm run dev
cube --json run --bg --stream /bin/sleep 30
```

## Process Listing And Inspection

`cube ps`
: List process names, modes, and states in the selected workspace. A trailing
  `*` marks a process configured for manager-start restart.

`cube ps -a`
: List every workspace, with the cubes in each workspace.

`cube inspect NAME`
: Show the current manager record for one process, including the stored command
  line when restart metadata is available, and the currently readable stdout
  and stderr byte ranges.

Examples:

```sh
cube ps
cube ps -a
cube --workspace ProjectA ps
cube --json ps
cube inspect server
cube --json inspect server
```

Typical process states include `running`, `completed`, `failed`, `lost`, and
`removed`.

For commands that take a process `NAME`, use `WORKSPACE.NAME` to target a cube
outside the currently selected workspace. If the workspace name contains spaces,
quote the whole reference, for example `cube inspect "Project A.server"`.

For multi-action commands, `PATTERN` may use shell-style wildcards in either
`NAME` or `WORKSPACE.NAME` form. Examples: `cube restart "bash-*"`,
`cube restart "*.codex"`, and `cube kill --cleanup "*.*"`. Wildcards are
supported by `restart`, `kill`, `save`, `unsave`, and restart-policy-only
`update`; `cube update PATTERN --name NAME` is rejected.

## Logs

```text
cube logs [--follow] [--stdout|--stderr] [--start N] [--end N] NAME
```

Print retained output for a process.

For `stream` processes, stdout is written to `cube` stdout and stderr is
written to `cube` stderr. For `tty` processes, the PTY stream is written to
stdout. For `term` processes, the PTY stream is written to stdout and the
captured stderr pipe is written to stderr.

`--stdout` prints only the stdout stream. For `tty` and `term` processes this
selects the retained PTY stream. `--stderr` prints only the retained stderr
stream; it is valid for `stream` and `term` processes.

`--start N` starts reading at zero-based byte offset `N`. `--end N` stops before
zero-based byte offset `N`. `--end` is exclusive and must be greater than or
equal to `--start`.

`--follow` keeps polling until the process reaches a terminal state and all
selected streams have reached end-of-stream. `--follow` may be used with
`--start`, but not with `--end`.

Examples:

```sh
cube logs build
cube logs --follow build
cube logs --stdout --start 4096 --end 8192 build
cube logs --stderr build
cube logs term-run >stdout.txt 2>stderr.txt
```

## Events

```text
cube events [--follow [--iterations N]]
```

List workspace events from the manager. Non-JSON output starts with:

```text
Workspace NAME

SEQ    PROCESS    TYPE    PAYLOAD
```

`--follow`
: Continue polling for new events.

`--iterations N`
: With `--follow`, stop after `N` polling iterations. `N` must be nonnegative.

In JSON mode, `cube events` prints a JSON response only when the manager returns
at least one event for that poll.

Examples:

```sh
cube events
cube events --follow
cube events --follow --iterations 200
cube --json events
```

## Connecting To Processes

```text
cube connect [--ro] NAME
```

Attach the current terminal to a managed process. `cube` requests an attachment
grant from the manager, then connects directly to the process controller.

`--ro`
: Read-only attach. `cube` reads output but does not request stdin.

Detach from an interactive connection with:

```text
Ctrl-\ d
```

That is Control-backslash followed by `d`. Detaching does not stop the managed
process. In terminal modes, `cube` sends terminal resize updates to the
controller when attaching and while active.

Examples:

```sh
cube connect shell
cube connect --ro build
cube --workspace Project@lab connect shell
cube connect Project@lab.shell
```

Remote-qualified workspace names use remotes configured by `cube remote add`.
For remote attachments, `cube` relays controller I/O through the authenticated
TLS manager connection.

## Pushing Input

```text
cube push [--eof] [--output] NAME
```

Read bytes from local stdin and write them to the managed process stdin without
starting an interactive attachment.

`--eof`
: After all local stdin is written, close the managed process stdin. This is
  supported for stream processes. Terminal processes fail clearly because PTYs
  cannot close stdin without closing the terminal master.

`--output`
: Print new output generated after the push starts. Stream stdout is printed to
  stdout and stream stderr to stderr; TTY output is printed to stdout. If the
  process stays running, `cube push --output` returns after a short idle period.

Examples:

```sh
printf 'status\n' | cube push shell
cube push --eof --output worker < input.txt
```

## Process Control

`cube stop NAME`
: Request graceful termination through the manager.

`cube kill NAME|PATTERN`
: Request forceful termination through the manager.

`cube kill --cleanup NAME|PATTERN`
: Request forceful termination, wait briefly for the process to exit, then
  remove that process record unless it is saved.

`cube kill --all`
: Request forceful termination for all running processes in the selected
  workspace.

`cube kill --all --cleanup`
: Kill all running processes in the selected workspace, then run workspace
  cleanup. This removes all unsaved non-live process records, including records
  that were already completed or lost before the kill command ran.

`cube restart NAME|PATTERN`
: Kill the process if it is running, remove the old unsaved process record, and
  start a new background process with the same name, mode, directory, and stored
  command argv.

`cube update NAME|PATTERN [--restart|--no-restart|--name NAME]`
: Change retained process attributes. `--restart` enables manager-start
  restart, `--no-restart` disables it, and `--name` renames the process within
  its workspace. Wildcards may only be used with `--restart` or `--no-restart`.

`cube signal NAME SIGNAL`
: Send a signal. `SIGNAL` may be a number or one of `HUP`, `INT`, `QUIT`,
  `TERM`, `KILL`, `USR1`, or `USR2`; names may also include the `SIG` prefix,
  for example `SIGTERM`.

`cube save NAME|PATTERN`
: Mark a process record as saved. Saved records are skipped by `cube cleanup`
  and by `cube kill --cleanup`.

`cube unsave NAME|PATTERN`
: Clear a process record's saved flag so cleanup commands may remove it.

`cube remove NAME`
: Remove a retained process record. This is for processes that have already
  completed, failed, or otherwise reached a removable state.

`cube cleanup [NAME|PATTERN]`
: Without an argument, remove completed/removable process records in the
  selected workspace and report how many live and saved processes were skipped.
  With `NAME` or `PATTERN`, only matching process records are considered.

`cube shutdown`
: Stop all managed processes and then ask the manager daemon to exit. Use
  `--manager-only` to exit the manager without stopping managed processes.

Examples:

```sh
cube stop server
cube kill stuck-build
cube kill --cleanup stuck-build
cube kill --all --cleanup
cube signal worker TERM
cube signal worker 15
cube save important-build
cube unsave important-build
cube remove old-build
cube cleanup
cube shutdown
cube --json cleanup
```

## Workspace Access

Access commands operate on the selected workspace unless `--workspace NAME` is
provided.

Roles:

`observer`
: Read and observe workspace/process/event state.

`operator`
: Observer permissions plus process start, input, signal, and remove, and
  workspace stop.

`owner`
: Operator permissions plus workspace rename/delete and workspace key
  management.

Commands:

`cube access list`
: List authorized workspace keys.

`cube access add PUBLIC_KEY_OR_FILE [--role ROLE] [--label LABEL]`
: Add a 64-character hex public key, or read one from a file. The default role
  is `operator`; the default label is empty.

`cube access set-role KEY_ID ROLE`
: Replace a key's capabilities with the selected role.

`cube access remove KEY_ID`
: Revoke a key.

`cube access revoke KEY_ID`
: Alias for `remove`.

Examples:

```sh
cube access list
cube --json access list
cube access add ~/.config/cubicle/keys/alice.pub --role observer --label Alice
cube access add 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f --role operator
cube access set-role 9bb5cf3b18d4e6c407a07e68f8c8a7dd owner
cube access remove 9bb5cf3b18d4e6c407a07e68f8c8a7dd
```

Same-UID Unix-socket clients are treated as manager owners by the manager.
Workspace creation bootstraps the creating authenticated key as an owner key.

## Configuration

`cube config validate`
: Validate the effective configuration. Prints `configuration valid` on
  success.

`cube config paths`
: Print the source and path-oriented effective values, including manager state,
  runtime, log directory, and socket mode.

`cube config show`
: Print the effective configuration values currently exposed by the CLI.

`cube config effective`
: Print each effective configuration value together with the source that
  supplied it.

`cube` and `desk` auto-start `cubicle-manager` in daemon mode when their
configured Unix manager socket is not accepting connections. Explicit
`--manager-socket` and `CUBICLE_MANAGER_SOCKET` selections do not auto-start a
manager. Set `cube.automanager=false` or `desk.automanager=false` to disable the
behavior.

Configuration keys currently read include:

```ini
[installation]
bindir=/usr/bin
libexecdir=/usr/libexec/cubicle

[manager]
state_dir=/var/lib/cubicle
runtime_dir=/run/cubicle
log_dir=/var/log/cubicle
listen=unix:///run/cubicle/manager.sock
socket_mode=0660
socket_group=
controller_binary=/usr/libexec/cubicle/cubicle-controller

[controller]
debug=none

[cube]
debug=none
automanager=true

[desk]
debug=none
automanager=true
scrollback_lines=10000
prefix=Control-X

[desk.keys]
bind.1=Prefix-n pane.next
bind.2=Prefix-p pane.previous
bind.3=Prefix-Space layout.zoom
bind.4=Prefix-PageUp scroll.page_up
bind.5=Prefix-PageDown scroll.page_down

[client]
manager=unix:///run/cubicle/manager.sock
server_identity=

[defaults]
launch=foreground
mode=tty
kill_cleanup=false
```

Set `controller.debug` to a comma-separated list of `input`, `library`, and
`terminal` to write controller debug events to each process `events.log`.
Set `cube.debug=library` or include `library` in `desk.debug` to write
libcubicle call traces to `manager.log_dir/client-library.log`; payload bytes
are not logged. Include `terminal` in `desk.debug`, for example
`desk.debug=library,terminal`, to write desk attach, snapshot, read, input, and
exit diagnostics to `manager.log_dir/desk-terminal.log`.

Desk enables mouse selection of inactive pane titles by default. On this
experimental branch, a non-title mouse press temporarily releases mouse
reporting so the outer terminal may handle a text-selection drag. Use
`desk --no-mouse` to start with title mouse selection disabled. Press
`Prefix-m` inside Desk to toggle mouse title selection. Inactive pane titles are
shown as `[name]` while mouse title selection is enabled and as `name` while it
is disabled. Press `Prefix-:` to save the current pane layout and cube
assignments under a user-global name. Press `Prefix-;` to open a centered
searchable layout picker for all saved layouts owned by the user; type to
filter, use the arrow keys to move through long lists, and press
Enter to load the selected layout into the current workspace. Press `Prefix-s`
to enter layout mode. In layout mode, arrows resize the active pane side,
Shift-arrows resize the opposite side, `H`/`V` split the active pane and open
the cube picker for the new pane, `D` deletes the active pane, `t` transposes
eligible panes on their axis, `r` resets the automatic layout, `h`/`v` apply
horizontal or vertical zoom, and `s`, `q`, or Escape exits layout mode. Press
`Prefix-?` to show a centered overlay with command names, descriptions, and
configured key bindings. From that overlay, press `e` to edit the selected
command's binding. Desk applies the edit immediately and persists it in a
managed block in the per-user config file.

Desk key bindings can be configured with `[desk] prefix=KEY` and
`[desk.keys] bind.N=KEY COMMAND`. Key names follow tmux-style forms such as
`Control-X`, `Ctrl-X`, `C-X`, `^X`, `Control-Space`, `Space`, `Enter`,
`Escape`, `Backspace`, `Tab`, arrows, `Home`, `End`, `PageUp`, `PageDown`,
`Insert`, `Delete`, `F1` through `F12`, modified keys such as `C-S-Right`,
`M-PageUp`, `S-F5`, or a single printable character. Prefix shortcuts
use `Prefix-KEY`; direct shortcuts omit `Prefix-`. Long aliases such as
`Control-Shift-Right` are accepted and displayed as canonical tmux-style names.
`desk.scrollback_lines` controls how many session-local scrolled-off lines each
pane keeps while the current `desk` process is running.
Supported command names are
`pane.next`, `pane.previous`, `pane.left`, `pane.right`, `pane.above`,
`pane.below`, `layout.zoom`, `layout.resize.toggle`,
`layout.transpose`, `layout.split.horizontal`, `layout.split.vertical`,
`layout.delete`, `layout.reset`, `layout.zoom.horizontal`,
`layout.zoom.vertical`, `layout.save`, `layout.load`, `bindings.show`,
`scroll.page_up`, `scroll.page_down`, `scroll.line_up`, `scroll.line_down`,
`scroll.top`, `scroll.bottom`, `menu.open`, `mouse.toggle`, and `quit`. Use
`none` to unbind a default shortcut.

Examples:

```sh
cube config validate
cube config paths
cube config show
cube config effective
cube defaults
cube defaults show
cube defaults set launch foreground|background
cube defaults set mode stream|tty|term
cube defaults set kill-cleanup true|false
cube defaults reset [launch|mode|kill-cleanup]
CUBICLE_CONFIG=/tmp/cubicle.cfg cube config validate
```

`cube defaults` and `cube defaults show` print the effective launch defaults:

```sh
cube defaults
launch=foreground
mode=tty
kill_cleanup=false
```

`cube defaults set` and `cube defaults reset` edit the per-user
`config.cfg` atomically while preserving unrelated settings.

## Output And Exit Status

Successful foreground `stream` runs return the managed process exit code when
the manager reports one. Most successful CLI commands exit `0`. Usage errors
exit `2`. Missing selected workspace and manager-reported not-found errors
commonly exit `1`.

Human-readable output is intended for terminals. Use `--json` for scripts where
the command supports it.

## Current Limitations

- The top-level shortcut form `cube COMMAND...` is not implemented; use
  `cube run COMMAND...`.
- `cube config` is read-only today; it validates and displays effective
  configuration but does not edit files.
- `cube connect` supports local controller attachment through manager-issued
  Unix controller endpoints and remote attachment relay through TLS manager
  connections. For terminal processes it restores the current controller
  snapshot first and falls back to bounded replay only when snapshot data is
  unavailable. Plain TCP manager connections remain development/test-only.
- `desk --workspace WORKSPACE@REMOTE` can open a remote workspace through the
  configured remote. A single Desk session still targets one manager, so mixed
  local and remote panes in the same layout are not implemented yet.
- JSON output is broad but not uniform across every command; lifecycle success
  commands often print `{}`.
- User configuration drop-ins and config-editing commands described in
  `docs/configuration-spec.md` are not fully implemented yet.

## See Also

`cubicle-manager(1)`, `cubicle-controller(1)`, `docs/cli-command-spec.md`,
`docs/configuration-spec.md`, `docs/api-spec-v0.md`
