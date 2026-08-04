# cube(1)

## Name

`cube` - run, reconnect to, inspect, and control Cubicle-managed processes.

## Synopsis

```text
cube [--manager-socket ENDPOINT] [--workspace NAME] [--json] COMMAND [ARG...]
cube help
cube workspace [NAME]
cube workspace list
cube workspace create [--dir DIRECTORY] NAME
cube workspace select NAME
cube workspace stop NAME
cube workspace delete NAME
cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIRECTORY] COMMAND [ARG...]
cube ps
cube inspect NAME
cube logs [--follow] [--stdout|--stderr] [--start N] [--end N] NAME
cube events [--follow [--iterations N]]
cube connect [--ro] NAME
cube stop NAME
cube kill [--all] [--cleanup] [NAME]
cube signal NAME SIGNAL
cube save NAME
cube unsave NAME
cube remove NAME
cube cleanup
cube access list
cube access add PUBLIC_KEY_OR_FILE [--role observer|operator|owner] [--label LABEL]
cube access set-role KEY_ID observer|operator|owner
cube access remove KEY_ID
cube access revoke KEY_ID
cube config show
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
`XDG_CONFIG_HOME` is unset. Setting `CUBICLE_CONFIG=/path/to/config.cfg` makes
`cube` load that exact file.

Examples:

```sh
cube --manager-socket /run/user/$UID/cubicle/manager.sock ps
cube --manager-socket unix:///tmp/cubicle/manager.sock workspace Dev
cube --manager-socket tcp://127.0.0.1:7777 ps
CUBICLE_CONFIG=/tmp/cubicle.cfg cube config paths
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
area. If no workspace is selected, commands such as `cube ps` exit with
`cube: no workspace selected`.

## Running Processes

```text
cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] [--dir DIRECTORY] COMMAND [ARG...]
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

Use `--` before a command whose first argument might otherwise be parsed as a
`cube run` option.

Examples:

```sh
cube run --stream make test
cube run --dir /tmp --stream pwd
cube run --fg --term vim docs/plan.txt
cube run --bg --term --name shell bash
cube run --bg --stream ls -lR /
cube run --bg --name server -- npm run dev
cube --json run --bg --stream /bin/sleep 30
```

## Process Listing And Inspection

`cube ps`
: List process names, modes, and states in the selected workspace.

`cube inspect NAME`
: Show the current manager record for one process.

Examples:

```sh
cube ps
cube --workspace ProjectA ps
cube --json ps
cube inspect server
cube --json inspect server
```

Typical process states include `running`, `completed`, `failed`, `lost`, and
`removed`.

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
```

## Process Control

`cube stop NAME`
: Request graceful termination through the manager.

`cube kill NAME`
: Request forceful termination through the manager.

`cube kill --cleanup NAME`
: Request forceful termination, wait briefly for the process to exit, then
  remove that process record unless it is saved.

`cube kill --all`
: Request forceful termination for all running processes in the selected
  workspace.

`cube kill --all --cleanup`
: Kill all running processes in the selected workspace, then remove the killed
  process records after they exit unless they are saved.

`cube signal NAME SIGNAL`
: Send a signal. `SIGNAL` may be a number or one of `HUP`, `INT`, `QUIT`,
  `TERM`, `KILL`, `USR1`, or `USR2`; names may also include the `SIG` prefix,
  for example `SIGTERM`.

`cube save NAME`
: Mark a process record as saved. Saved records are skipped by `cube cleanup`
  and by `cube kill --cleanup`.

`cube unsave NAME`
: Clear a process record's saved flag so cleanup commands may remove it.

`cube remove NAME`
: Remove a retained process record. This is for processes that have already
  completed, failed, or otherwise reached a removable state.

`cube cleanup`
: Remove completed/removable process records in the selected workspace and
  report how many live and saved processes were skipped.

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

[client]
manager=unix:///run/cubicle/manager.sock
server_identity=

[defaults]
launch=foreground
mode=tty
kill_cleanup=false
```

Examples:

```sh
cube config validate
cube config paths
cube config show
CUBICLE_CONFIG=/tmp/cubicle.cfg cube config validate
```

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
- `cube connect` supports controller attachment through the manager-issued
  Unix controller endpoint. For terminal processes it restores the current
  controller snapshot first and falls back to bounded replay only when snapshot
  data is unavailable. TCP manager connections are supported only when the
  manager is explicitly configured/listening for TCP.
- JSON output is broad but not uniform across every command; lifecycle success
  commands often print `{}`.
- User configuration drop-ins and config-editing commands described in
  `docs/configuration-spec.md` are not fully implemented yet.

## See Also

`cubicle-manager(1)`, `cubicle-controller(1)`, `docs/cli-command-spec.md`,
`docs/configuration-spec.md`, `docs/api-spec-v0.md`
