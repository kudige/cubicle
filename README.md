# Cubicle

Cubicle is a persistent development runtime for long-running local work.
Processes live inside Cubicle workspaces, not inside the terminal that started
them. You can close a terminal, reconnect later, split the same workspace into
panes, inspect logs, push input, and clean up completed work without killing the
active process by accident.

The system is built around:

- **cube** - the CLI for workspaces, process lifecycle, logs, input, access,
  configuration, and direct terminal attachment.
- **desk** - a terminal workspace UI that composes Cubicle processes into
  panes, saved layouts, searchable menus, and configurable tmux-style keys.
- **cubicle-manager** - the per-user control plane. It owns workspace records,
  process records, permissions, events, and controller discovery.
- **cubicle-controller** - the per-process data plane. It owns one managed
  process group, PTYs or pipes, retained logs, snapshots, input, resize, and
  direct attachment sockets.

## Quick Start

Build and test from source:

```console
sudo apt install cmake pkg-config libeconf-dev libssl-dev libvterm-dev
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Start a manager and create a workspace:

```console
cubicle-manager
cube workspace create --dir "$PWD" ProjectA
```

Run a persistent interactive process:

```console
cube run --bg --tty --name shell bash
cube connect shell
```

Detach from `cube connect` with `Ctrl-\ d`. The process keeps running.

Open the full terminal UI:

```console
desk
```

## Installing

Create and install a Debian package:

```console
cmake -S . -B build
cmake --build build --target package
sudo apt install ./build/cubicle_0.1.0_*.deb
```

Create and install a pacman package:

```console
cmake -S . -B build
cmake --build build --target pacman-package
sudo pacman -U ./build/cubicle-0.1.0-1-*.pkg.tar.*
```

Create and install a native macOS package:

```console
brew install cmake pkg-config libeconf openssl libvterm
cmake -S . -B build
cmake --build build --target macos-package
sudo installer -pkg ./build/cubicle-0.1.0-macos-*.pkg -target /
```

The macOS package installs under `/usr/local`, including its package defaults
at `/usr/local/lib/cubicle/config.cfg`. Runtime library dependencies installed
with Homebrew remain external to the package.

The Debian and pacman packages install:

```text
/usr/bin/cube
/usr/bin/desk
/usr/bin/cubicle-manager
/usr/libexec/cubicle/cubicle-controller
/usr/lib/cubicle/config.cfg
```

After installing new binaries, restart the manager and start new cubes for
controller-side changes to take effect. Existing controllers keep running the
old executable image until their managed process exits or is restarted.

## Manager And Runtime Paths

Normal users run a per-user manager by default:

```text
state:   $XDG_STATE_HOME/cubicle, or ~/.local/state/cubicle
runtime: $XDG_RUNTIME_DIR/cubicle, or /run/user/$UID/cubicle
logs:    $XDG_STATE_HOME/cubicle/log
socket:  $XDG_RUNTIME_DIR/cubicle/manager.sock
config:  $XDG_CONFIG_HOME/cubicle/config.cfg, or ~/.config/cubicle/config.cfg
```

On macOS, the default runtime directory is `/tmp/cubicle-$UID` so manager and
controller Unix socket paths stay within the platform's shorter path limit.

Useful commands:

```console
cube config paths
cube config show
cube config effective
cube config validate
```

`cubicle-manager` runs the daemon by default and detaches after startup. Use
`cubicle-manager --foreground` under a supervisor or test harness. The explicit
`cubicle-manager daemon ...` form is also accepted.

`cube` and `desk` auto-start the per-user manager in daemon mode when the
configured Unix manager socket is not accepting connections. Disable this with
`cube.automanager=false` or `desk.automanager=false` when a supervisor should be
the only process allowed to start the manager.

For local-LAN experiments, the manager can listen on unauthenticated TCP only
when explicitly allowed:

```console
cubicle-manager --listen tcp://127.0.0.1:7777 --allow-insecure
cube --manager-socket tcp://127.0.0.1:7777 ps
```

Do not expose insecure TCP to untrusted networks.

## cube Power Mode

`cube` is the fast path for scripting and precise control.

```text
cube workspace [NAME]
cube workspace create [--dir DIRECTORY] NAME
cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIRECTORY] [--restart] COMMAND [ARG...]
cube ps [-a|--all]
cube inspect NAME
cube logs [--follow] [--stdout|--stderr] [--start N] [--end N] NAME
cube connect [--ro] NAME
cube push [--eof] [--output] NAME
cube update NAME|PATTERN [--restart|--no-restart|--name NAME]
cube restart NAME|PATTERN
cube stop NAME
cube kill [--all] [--cleanup] [NAME|PATTERN]
cube save NAME|PATTERN
cube unsave NAME|PATTERN
cube cleanup
cube shutdown [--manager-only]
```

Process patterns use shell-style wildcards in either `NAME` or
`WORKSPACE.NAME` form. Examples: `cube restart "bash-*"`,
`cube restart "*.codex"`, and `cube kill --cleanup "*.*"`. Wildcards are
available for `restart`, `kill`, `save`, `unsave`, and `cube update --restart`
or `--no-restart`; wildcard rename is intentionally rejected.

### Workspaces

Workspaces give process names a scope and store a default directory:

```console
cube workspace create --dir "$PWD" website
cube workspace website
cube workspace list
cube --workspace website ps
cube ps --all
```

Most process commands accept `WORKSPACE.CUBE`, so you can target inactive
workspaces without switching:

```console
cube inspect website.server
cube connect admin.shell
cube logs --stderr build.compile
```

Quote the whole name when the workspace contains spaces:

```console
cube inspect "Project A.server"
```

### Running Processes

Use `--tty` for terminal applications and shells. This is the default mode in
the current package unless changed by config.

```console
cube run --bg --tty --name shell bash
cube run --fg --tty --name editor vim docs/plan.txt
cube run --bg --tty --dir ~/src/site --name codex codex
```

Use `--stream` for commands where separate stdout and stderr matter:

```console
cube run --bg --stream --name build make -j8
cube logs --stdout build
cube logs --stderr build
```

Use `--term` for experimental terminal-style split capture. It keeps terminal
behavior while retaining a separate stderr stream where possible.

```console
cube run --bg --term --name shell-with-stderr bash
```

Use `--restart` for cubes that should be recreated when the manager starts:

```console
cube run --bg --restart --name server npm run dev
cube ps
cube update server --no-restart
cube update server --restart
```

Restart an existing cube from its recorded argv, mode, name, and directory:

```console
cube restart server
```

### Connecting, Logs, And Input

Interactive connect restores the controller's terminal snapshot first, then
streams live output. Resize events are forwarded while attached.

```console
cube connect shell
cube connect --ro server
```

Detach with `Ctrl-\ d`.

Inspect retained byte ranges and command metadata:

```console
cube inspect shell
cube --json inspect shell
```

Read retained logs by stream and byte range:

```console
cube logs shell
cube logs --follow build
cube logs --stdout --start 4096 --end 8192 build
cube logs --stderr build
```

Push stdin without opening an interactive session:

```console
printf 'status\n' | cube push shell
cube push --eof --output worker < input.txt
```

`--eof` is for stream processes. PTY-backed terminal processes cannot close
stdin independently from the terminal master.

### Lifecycle And Cleanup

```console
cube stop server
cube kill stuck
cube kill --cleanup stuck
cube kill --all --cleanup
cube signal worker TERM
cube save important-session
cube unsave important-session
cube cleanup
cube cleanup "old-build-*"
cube shutdown
```

Saved process records are skipped by `cube cleanup` and `cube kill --cleanup`
until `cube unsave` clears the flag.

`cube shutdown` stops managed processes and asks the manager to exit. Use
`cube shutdown --manager-only` to exit the manager without stopping managed
processes.

### JSON And Automation

Many commands support `--json`:

```console
cube --json ps
cube --json inspect server
cube --json workspace list
cube --json cleanup
```

Human-readable output is for terminals. Prefer `--json` in scripts where the
command supports it.

### Access Control

Unix-socket clients from the same UID are treated as manager owners. Workspace
creation bootstraps the creating authenticated key as an owner key. Additional
workspace keys can be granted observer, operator, or owner roles:

```console
cube access list
cube access add ~/.config/cubicle/keys/alice.pub --role observer --label Alice
cube access set-role KEY_ID operator
cube access remove KEY_ID
```

## desk Power Mode

`desk` is the high-density terminal UI for active workspaces. It attaches to
running cubes, renders terminal snapshots, streams output through a local
libvterm-backed model, forwards input to the selected pane, and keeps cube
lifetime independent of the UI.

```console
desk
desk --workspace website
desk --prefix C-Space
desk --no-mouse
```

Default prefix is configurable and currently defaults to `Control-X` in the
packaged config. Examples below use `Prefix`.

### Core Navigation

```text
Prefix-n      Next pane
Prefix-p      Previous pane
Prefix-Left   Select pane to the left
Prefix-Right  Select pane to the right
Prefix-Up     Select pane above
Prefix-Down   Select pane below
Prefix-Space  Toggle full-pane zoom
Prefix-PageUp Enter scroll mode and scroll active pane up one page
Prefix-PageDown Enter scroll mode and scroll active pane down one page
Prefix-Home   Scroll active pane to top
Prefix-End    Return active pane to live output
Prefix-q      Quit desk, leaving cubes running
```

In scroll mode, plain arrows, PageUp/PageDown, Home, and End keep scrolling the
active pane. Press `q` or `s` to leave scroll mode; normal input resumes after
leaving the mode.

Pane direction selection is geometry-aware: it chooses the best pane in the
requested direction based on current desk layout.

### Menus And Cube Picker

```text
Prefix-o      Open workspace/cube/layout menu
Prefix-m      Toggle mouse title selection
```

The menu opens in the middle of the terminal, highlights the last opened
workspace by default, and lets you pick cubes from any workspace or load a
saved layout. Inactive pane
titles can be clicked when mouse title selection is enabled. A non-title mouse
press temporarily releases mouse reporting so the outer terminal can handle
normal text selection.

### Layout Mode

Enter layout mode:

```text
Prefix-s
```

Inside layout mode:

```text
Arrow keys        Resize the active pane side
Shift-arrows      Resize the opposite side
H                 Split horizontally and open the cube picker
V                 Split vertically and open the cube picker
D                 Delete the active pane
t                 Transpose eligible panes on their axis
r                 Reset automatic layout
h                 Toggle horizontal axis zoom
v                 Toggle vertical axis zoom
s, q, Escape      Exit layout mode
```

Resizes are sent to the controller and followed by an authoritative terminal
snapshot reload, so full-screen applications such as shells, editors, `less`,
and Codex see the new PTY size. Desk batches repeated resize-arrow input to
avoid unnecessary snapshot reloads.

For three cubes, the default layout is optimized for work: the left pane is
split top/bottom and the right pane spans the full height.

### Saved Layouts

Layouts are user-global, not workspace-local. They remember pane structure and
the cubes assigned to panes.

```text
Prefix-:      Save current layout under a name
Prefix-;      Open searchable layout picker
```

The layout picker has a search area at the top, supports scrolling for long
lists, and loads the selected layout into the current workspace.

### Bindings Overlay

```text
Prefix-?      Show command names, descriptions, and current bindings
e             Edit the selected binding from the overlay
```

Binding edits apply immediately and are persisted to the per-user config file.

Desk supports tmux-style key names:

```text
Control-X
Ctrl-X
C-X
^X
Control-Space
Prefix-n
C-S-Right
M-PageUp
S-F5
Escape
Enter
Backspace
F1
```

Configure keys in `~/.config/cubicle/config.cfg`:

```ini
[desk]
prefix=Control-X
scrollback_lines=10000

[desk.keys]
bind.1=Prefix-n pane.next
bind.2=Prefix-p pane.previous
bind.3=Prefix-Space layout.zoom
bind.4=Prefix-? bindings.show
bind.5=Prefix-PageUp scroll.page_up
bind.6=Prefix-PageDown scroll.page_down
```

Supported command names include:

```text
pane.next
pane.previous
pane.left
pane.right
pane.above
pane.below
layout.zoom
layout.resize.toggle
layout.movepane
layout.transpose
layout.split.horizontal
layout.split.vertical
layout.delete
layout.reset
layout.zoom.horizontal
layout.zoom.vertical
layout.save
layout.load
bindings.show
scroll.page_up
scroll.page_down
scroll.line_up
scroll.line_down
scroll.top
scroll.bottom
menu.open
mouse.toggle
quit
```

Use `none` to unbind a default shortcut.

### Debugging desk

Enable terminal/UI diagnostics:

```ini
[desk]
debug=terminal
```

or include library tracing:

```ini
[desk]
debug=library,terminal
```

Logs are written under the configured manager log directory, normally:

```text
~/.local/state/cubicle/log/desk-terminal.log
~/.local/state/cubicle/log/client-library.log
```

Resize timing logs use:

```text
event=layout_resize_flush elapsed_ms=...
```

## Configuration Defaults

Show and edit launch defaults:

```console
cube defaults
cube defaults show
cube defaults set launch foreground
cube defaults set launch background
cube defaults set mode tty
cube defaults set mode stream
cube defaults set kill-cleanup true
cube defaults reset mode
```

Common config shape:

```ini
[manager]
state_dir=/home/me/.local/state/cubicle
runtime_dir=/run/user/1000/cubicle
log_dir=/home/me/.local/state/cubicle/log
listen=unix:///run/user/1000/cubicle/manager.sock
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

[defaults]
launch=foreground
mode=tty
kill_cleanup=false
```

Controller debug categories include `input`, `library`, and `terminal`:

```ini
[controller]
debug=input,terminal
```

## Development Notes

Run targeted or full tests:

```console
ctest --test-dir build -R '^desk-tty-attach-safety-test$' --output-on-failure
ctest --test-dir build -R 'cubicle-terminal-resize-e2e-test|cubicle-controller-snapshot-test' --output-on-failure
ctest --test-dir build --output-on-failure
```

Coverage is opt-in:

```console
cmake -S . -B build-coverage -DCUBICLE_ENABLE_COVERAGE=ON
cmake --build build-coverage --target coverage-summary
```

The `coverage-summary` target builds instrumented binaries, runs CTest, filters
out test sources, and prints production-source coverage from
`build-coverage/coverage.filtered.info`.

Useful lower-level tools remain available for protocol and controller work:

```console
python3 tests/api_client.py unix:///run/user/$UID/cubicle/manager.sock ping
python3 tests/control_socket_client.py /path/to/control.sock status
python3 tests/controller_resize_probe.py /path/to/control.sock --rows 40 --cols 120
```

## Current Limitations

- The shortcut form `cube COMMAND...` is intentionally deferred; use
  `cube run COMMAND...`.
- Existing controllers must be restarted to pick up controller-side fixes after
  installing new binaries.
- `term` mode is experimental. `tty` and `stream` are the primary modes.
- TCP manager access exists, but secure remote deployment should use the
  authenticated socket path and a trusted transport configuration.

See also:

- `docs/cube.1.md`
- `docs/cli-command-spec.md`
- `docs/configuration-spec.md`
- `docs/api-spec-v0.md`
- `docs/protocol.txt`
