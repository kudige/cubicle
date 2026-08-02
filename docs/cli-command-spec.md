# Cubicle CLI Command Specification

Status: **Draft for phased implementation**
Command: **`cube`**

This document defines the intended user-facing command-line experience for Cubicle. The goal is an intuitive, highly productive workflow for launching, observing, reconnecting to, and controlling persistent managed processes.

The CLI should hide manager/controller implementation details. Users work with:

- a current workspace,
- friendly process names,
- foreground or background launch behaviour,
- process modes (`stream`, `tty`, and later `term`),
- attach/detach operations,
- configurable defaults.

## 1. Core principles

1. All user-facing commands use the single executable name `cube`.
2. The explicit launch operation is `cube run COMMAND...`.
3. Explicit command-line options always override configured defaults.
4. Defaults are policy-driven, not inferred from executable names.
5. The initial built-in launch default is foreground.
6. The initial built-in mode default is TTY when available, otherwise stream during development.
7. Term mode remains explicit; the production built-in mode default is TTY.
8. Closing or detaching a terminal does not stop the managed process.
9. Workspace and process names are the primary user-facing identifiers.
10. Advanced protocol, controller, endpoint, and grant details remain hidden.

## 2. Primary workflow

```console
$ cube workspace Shogun
Workspace Shogun selected

$ cube run make
... make output appears here ...

$ cube run --bg emacs test.c
[emacs-test.c] started in tty mode

$ cube run --bg --term bash
[bash] started in term mode

$ cube run --bg --name CScope bash
[CScope] started in tty mode

$ cube ps
Workspace Shogun

NAME            MODE     STATE      CLIENTS
emacs-test.c    tty      running    0
bash            term     running    0
CScope          tty      running    0

$ cube connect CScope
Connected to [CScope]. Detach with Ctrl-\\ d

$ cube connect --ro bash
Connected read-only to [bash]. Detach with Ctrl-\\ d

$ printf 'make test\n' | cube push shell
Pushed input to [shell]
```

## 3. Launch command shape

```text
cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] COMMAND [ARG...]
```

Examples:

```console
cube run make
cube run --fg pytest
cube run --bg npm run dev
cube run --bg --tty emacs test.c
cube run --bg --term bash
cube run --bg --name backend npm run server
```

### 3.1 Foreground and background

`--fg` launches a managed process and immediately connects the invoking terminal.

For a foreground stream process, Cubicle should:

1. create the managed process,
2. attach stdin/stdout/stderr,
3. wait for completion,
4. return the managed process exit status when possible.

The process remains managed. An unexpected client disconnect must not automatically kill it.

`--bg` launches the process detached and returns after successful startup:

```console
$ cube run --bg make watch
[make-watch] started in tty mode
```

When neither option is supplied, resolution is:

```text
explicit --fg or --bg
        ↓
matching command rule, when Phase 5 exists
        ↓
configured default
        ↓
built-in default: foreground
```

Initially, `cube run COMMAND...` is equivalent to `cube run --fg COMMAND...`.
The top-level shortcut form `cube COMMAND...` is reserved for a later phase.

### 3.2 Process modes

The modes are:

- `--stream`: stdin, stdout, and stderr are independent pipes.
- `--tty`: stdin, stdout, and stderr share one PTY.
- `--term`: stdin and stdout use a PTY while stderr remains separately captured.

Mode selection must **not** depend on executable name or hard-coded command heuristics.

Resolution is:

```text
explicit --stream, --tty, or --term
        ↓
matching command rule, when Phase 5 exists
        ↓
configured default
        ↓
built-in default
```

Built-in default by implementation stage:

```text
term implemented       → term
TTY implemented        → tty
otherwise              → stream
```

Phase 1 explicitly uses `tty` as its normal default, with `stream` as the temporary fallback if TTY support is not ready.

If a configured mode is unsupported by the connected manager, the command should fail clearly rather than silently changing semantics, except for explicitly documented development fallback behaviour.

Example:

```text
cube: configured default mode 'term' is not supported by this manager
hint: use --tty or run 'cube defaults set mode tty'
```

### 3.3 Friendly names

`--name NAME` sets the complete user-facing process name:

```console
$ cube run --bg --name CScope bash
[CScope] started in tty mode
```

The display should not rewrite this as `bash:CScope`.

Without `--name`, Cubicle generates a useful name from the command line:

```text
emacs test.c       → emacs-test.c
bash               → bash
make               → make
make               → make-2
npm run dev        → npm-dev
```

Generated names must be unique within the workspace by adding a numeric suffix.

Explicit names should be unique within the workspace. A collision should fail clearly:

```text
cube: process name 'CScope' already exists in workspace 'Shogun'
```

Replacement or reuse semantics can be added later; Phase 1 should not silently replace an existing process.

### 3.4 End of options

The CLI must support `--` so commands beginning with options are unambiguous:

```console
cube run --bg -- command --fg
cube run --stream -- ./script --name internal-option
```

## 4. Workspace commands

Core commands:

```console
cube workspace
cube workspace NAME
cube workspace list
cube workspace create NAME
cube workspace select NAME
cube workspace stop NAME
cube workspace delete NAME
```

### 4.1 Convenience selection

```console
cube workspace Shogun
```

means:

1. select `Shogun` if it exists,
2. otherwise create it and select it, subject to authorization.

The command should print the action:

```text
Workspace Shogun selected
```

or:

```text
Workspace Shogun created and selected
```

### 4.2 Current workspace persistence

The selected workspace must apply predictably to later `cube` invocations. Initial implementation options include:

- a per-shell environment variable through shell integration,
- a current-workspace file scoped to the terminal/session,
- a user-wide selected workspace as an early implementation fallback.

## 4A. Workspace Access Commands

Access commands manage public keys for the selected workspace, or for the
workspace passed through global `--workspace NAME`.

```console
cube access list
cube access add PUBLIC_KEY_OR_FILE [--role observer|operator|owner] [--label LABEL]
cube access set-role KEY_ID observer|operator|owner
cube access remove KEY_ID
```

Roles are CLI conveniences over explicit capability masks:

- `owner`: all workspace, process, event, and key-management capabilities.
- `operator`: can run, observe, attach interactively, signal, stop, and remove
  processes in the workspace, but cannot manage keys.
- `observer`: can list/read workspace, process, and event state and request
  read-only attachments.

When a same-UID local authenticated client creates a workspace, the manager
automatically records that client's public key as the initial `owner` key.

The preferred long-term approach is shell integration so separate shells can select different workspaces independently.

Commands should also support an explicit workspace override:

```console
cube --workspace Shogun ps
cube --workspace Shogun --bg make watch
```

Precedence:

```text
explicit --workspace
        ↓
current shell workspace
        ↓
configured default workspace, if introduced
        ↓
error with guidance
```

## 5. Process inspection

### 5.1 Process list

```console
cube ps
```

Recommended default output:

```text
Workspace Shogun

NAME            MODE     STATE       CLIENTS     AGE       RESULT
emacs-test.c    tty      running     0           12m
bash            term     running     1 ro        8m
CScope          tty      running     1 rw        3m
make            stream   completed   0           15m       exit 0
```

`CLIENTS` scales better than the phrase `not connected`, because several observers may attach simultaneously.

Initial useful filters:

```console
cube ps --running
cube ps --all
cube ps --workspace Shogun
cube ps --json
```

Later:

```console
cube ps --watch
```

The default should show live processes and a bounded number of recently completed processes. `--all` includes retained history.

### 5.2 Inspect one process

```console
cube inspect CScope
```

Example output:

```text
Name:        CScope
Workspace:   Shogun
Command:     bash
Mode:        tty
State:       running
Clients:     1 interactive, 1 read-only
Started:     3 minutes ago
Process ID:  0123456789abcdef0123456789abcdef
```

Local PID/PGID may be shown as diagnostics when available but must not be treated as stable remote identities.

## 6. Connect and detach

### 6.1 Interactive connection

```console
cube connect NAME
```

Default semantics:

- request an interactive attachment grant from the manager,
- connect directly or by relay to the process controller,
- forward keyboard input,
- display terminal or stream output,
- forward terminal resize events where applicable.

For TTY and term processes, this renders the virtual terminal.

For stream processes, it connects stdin, stdout, and stderr.

### 6.2 Read-only connection

```console
cube connect --ro NAME
```

Semantics:

- no stdin forwarding,
- observe stdout/stderr for stream and term processes,
- observe rendered terminal output for TTY processes,
- do not claim interactive control.

Future advanced channel options may include:

```console
cube connect --out NAME
cube connect --err NAME
cube connect --noerr NAME
cube connect --in NAME
```

These should not complicate Phase 2.

### 6.3 Non-interactive input push

```console
cube push [--close] NAME
```

`cube push` reads all bytes from its own stdin and writes them to the managed
process stdin without creating an interactive terminal attachment.

Default semantics:

- request an input-capable attachment grant from the manager,
- connect directly or by relay to the process controller,
- copy `cube` stdin to the process stdin or PTY input,
- return after all local stdin has been written,
- do not read or display process output,
- do not stop, detach, or otherwise alter process lifecycle.

By default, `cube push NAME` leaves the managed process stdin open after the
write completes. This allows one-shot input injection into a still-running
process without causing EOF:

```console
printf 'status\n' | cube push shell
```

`--close` writes all input and then closes the managed process stdin:

```console
printf 'quit\n' | cube push --close worker
```

Close semantics:

- for stream processes, close the controller-side stdin pipe after all bytes
  are written;
- for TTY and term processes, send the bytes to the PTY input and then request
  the controller's closest supported input-close/EOF action;
- if the process mode or controller cannot represent input close, fail clearly
  instead of pretending EOF was delivered.

`cube push` is not a replacement for `cube connect`:

- it does not put the local terminal in raw mode,
- it does not forward resize events,
- it does not consume attachment escape sequences,
- it is suitable for scripts, pasted command batches, and feeding data from
  files.

Examples:

```console
cat commands.txt | cube push repl
printf '\003' | cube push terminal
cube push --close build < input.txt
```

### 6.4 Detach sequence

Phase 2 implements exactly one client escape sequence:

```text
Ctrl-\\ d    detach without stopping the process
```

On connection, print:

```text
Connected to [CScope]. Detach with Ctrl-\\ d
```

The escape prefix should eventually be configurable, but Phase 2 may use a fixed prefix.

The detach sequence is consumed by the `cube` client and is not forwarded to the managed process.

A literal escape prefix should eventually be forwardable by pressing the prefix twice, but this can wait until the broader escape-command phases.

## 7. Lifecycle and output commands

Core lifecycle commands:

```console
cube signal NAME SIGNAL
cube stop NAME
cube kill [--all] [--cleanup] [NAME]
cube push [--close] NAME
cube remove NAME
cube cleanup
```

Recommended semantics:

- `signal`: send the named/numbered signal to the managed process group.
- `stop`: request graceful termination, then optionally force after configured grace.
- `kill`: immediate forceful termination.
- `kill --all`: immediate forceful termination for all running processes in
  the selected workspace.
- `kill --cleanup`: after a successful kill, wait briefly for the killed
  process to exit and remove that process record. `defaults.kill_cleanup` may
  make this behavior the default.
- `push`: copy local stdin into process stdin, optionally closing stdin after
  the write with `--close`.
- `remove`: remove retained process state; fail if running unless explicitly forced.
- `cleanup`: remove retained terminal process state in the current workspace; skip live processes.

Output/history commands:

```console
cube logs NAME
cube logs --follow NAME
cube events
cube events --follow
cube events --follow --iterations N
```

`cube logs` reads retained output without creating an interactive attachment. For a TTY process it may show the recorded TTY byte stream initially; richer terminal-history rendering can follow.
`cube events --follow --iterations N` is a bounded polling form for scripts and integration tests; plain `--follow` continues until interrupted.

## 8. Defaults commands

The user can change launch and mode defaults without changing individual commands.

Initial command family:

```console
cube defaults
cube defaults show
cube defaults set launch foreground
cube defaults set launch background
cube defaults set mode stream
cube defaults set mode tty
cube defaults set mode term
cube defaults reset launch
cube defaults reset mode
cube defaults reset
```

Example:

```console
$ cube defaults
Launch: foreground
Mode:   tty

$ cube defaults set launch background
Default launch behaviour set to background

$ cube defaults set mode stream
Default process mode set to stream
```

Then:

```console
$ cube run make watch
[make-watch] started in stream mode
```

An explicit option always wins:

```console
cube run --fg --tty make watch
```

### 8.1 Initial configuration

A simple initial representation:

```toml
[defaults]
launch = "foreground"
mode = "tty"
kill_cleanup = false
```

Suggested user configuration location:

```text
$XDG_CONFIG_HOME/cubicle/config.toml
```

or, when `XDG_CONFIG_HOME` is unset:

```text
~/.config/cubicle/config.toml
```

Writes should be atomic and preserve unrelated future settings.

### 8.2 Future scopes

The design should leave room for:

```text
explicit CLI option
matching command rule
workspace-specific default
user-wide default
built-in default
```

Workspace-specific defaults are useful but not required for the first defaults phase.

## 9. Command-based defaults

A later phase may configure launch policy based on command name or full command-line regular expressions.

Illustrative syntax:

```console
cube defaults rule add --command make --mode stream --launch foreground
cube defaults rule add --match '^npm run (dev|watch)$' --mode term --launch background
cube defaults rule list
cube defaults rule remove 3
```

Example configuration:

```toml
[[rules]]
command = "make"
launch = "foreground"
mode = "stream"

[[rules]]
command_regex = "^npm run (dev|watch)$"
launch = "background"
mode = "term"
```

Rules must never be built into Cubicle as executable heuristics. They are explicit user policy.

Recommended rule behaviour:

1. Rules are ordered.
2. First matching rule wins.
3. Exact-command rules may be evaluated before regex rules, or all rules may retain explicit order; the chosen behaviour must be documented.
4. Explicit CLI options override every rule.
5. Invalid regular expressions are rejected when configured, not during process launch.
6. `cube defaults explain COMMAND...` should eventually show why a mode and launch policy were selected.

Example:

```console
$ cube defaults explain npm run dev
Launch: background
Mode:   term
Source: rule 2 (^npm run (dev|watch)$)
```

## 10. Escape-command roadmap

The attachment client uses an escape prefix, initially `Ctrl-\\`.

### Essential escape commands

After basic detach works, add the most productive commands:

```text
Ctrl-\\ d    detach
Ctrl-\\ ?    show escape help
Ctrl-\\ \\   send a literal Ctrl-\\ to the process
Ctrl-\\ r    request/toggle read-only mode where supported
```

Potentially:

```text
Ctrl-\\ q    request process stop and detach, with confirmation
```

Stopping a process from an escape command is destructive and must not be easy to trigger accidentally.

### Remaining escape commands

Later candidates:

```text
Ctrl-\\ s    show attachment/process status
Ctrl-\\ p    switch/select process or pane in future Desk-like clients
Ctrl-\\ c    request interactive control when observing
Ctrl-\\ i    release interactive control while staying attached
Ctrl-\\ l    redraw/resynchronize terminal
Ctrl-\\ e    toggle stderr display in term mode
Ctrl-\\ o    toggle local connection diagnostics
```

Only commands with clear workflow value should be implemented. The escape prefix must not become a miniature, difficult-to-remember terminal multiplexer language.

## 11. Exit status and scripting

Commands should have predictable exit codes:

- `0`: operation succeeded.
- foreground process: return the process exit code when representable.
- nonzero Cubicle-specific failures: use a documented small range or conventional failure code.
- signal termination: follow shell-compatible conventions where practical.

Machine-readable output:

```console
cube ps --json
cube inspect --json NAME
cube workspace list --json
cube defaults show --json
```

Human output goes to stdout. Diagnostics and errors go to stderr.

No colour should be emitted when stdout is not a terminal unless explicitly requested.

## 12. Help and discoverability

```console
cube --help
cube help workspace
cube help connect
cube help defaults
cube COMMAND --help
```

Top-level help should emphasize the workflow rather than expose internal architecture:

```text
Usage:
  cube workspace NAME
  cube run [OPTIONS] COMMAND [ARG...]
  cube ps
  cube connect [--ro] NAME
  cube push [--close] NAME
  cube stop NAME

Run and reconnect to persistent processes inside Cubicle workspaces.
```

Unknown-command errors should provide relevant suggestions.

## 13. Phased implementation

### Phase 1 — Basic workspace and managed-process CLI

Goal: make `cube` useful for normal process launch and inspection without attachments.

Implement:

- `cube workspace`
- `cube workspace NAME`
- `cube workspace list`
- `cube run [--fg|--bg] [--stream|--tty|--term] [--name NAME] COMMAND...`
- `cube ps`
- `cube inspect NAME`
- `cube signal NAME SIGNAL`
- `cube stop NAME`
- `cube kill [--all] [--cleanup] [NAME]`
- `cube remove NAME`
- `cube cleanup`
- explicit `--workspace NAME`
- generated friendly names and collision handling
- standard built-in defaults:
  - launch: foreground
  - mode: TTY
  - temporary stream fallback if TTY is not yet available
- stable human-readable output
- `--json` for at least `ps` and `inspect`
- shell-compatible foreground exit status

Not included:

- connect/attach
- configurable defaults
- term mode
- command-based rules

Acceptance workflow:

```console
cube workspace Shogun
cube run make
cube run --bg --name editor emacs test.c
cube ps
cube inspect editor
cube stop editor
```

### Phase 2 — Attach and detach

Goal: reconnect safely to running managed processes.

Implement:

- `cube connect NAME`
- `cube connect --ro NAME`
- `cube push NAME`
- `cube push --close NAME`
- manager attachment-grant request
- direct/relay controller connection according to grant
- interactive input forwarding
- non-interactive stdin push without output attachment
- optional stdin close after push
- output forwarding
- TTY resize propagation
- exactly one escape command:
  - `Ctrl-\\ d` detach
- clear connection banner
- connection cleanup without process termination
- tests for disconnect, reconnect, read-only access, and controller loss

Also add:

- `cube logs NAME`
- `cube logs --follow NAME` if retained-output APIs are ready

### Phase 3 — User defaults

Goal: make the common launch behaviour configurable without command heuristics.

Implement:

- `cube defaults show`
- `cube defaults set launch foreground|background`
- `cube defaults set mode stream|tty|term`
- `cube defaults reset launch|mode`
- config file loading and atomic writes
- precedence:
  - explicit option
  - configured user default
  - built-in default
- validation and actionable diagnostics
- `cube defaults show --json`

Do not include command-specific matching yet.

### Phase 4 — Term mode

Goal: provide an explicit mode combining terminal interactivity with separately
captured streams. The default mode remains `tty`.

Implement:

- controller support for PTY stdin/stdout plus stderr pipe
- `--term`
- term-mode attachment framing and channel handling
- retained stderr reads
- read-only combined display
- terminal resize behaviour
- tests for `isatty(stdout)`, non-TTY stderr, ordering expectations, backpressure, and EOF
- explicit term-mode selection without changing the TTY production default

Configured users who explicitly selected TTY or stream remain unchanged.

### Phase 5 — Command-based defaults

Goal: allow users to define their own executable/command-line launch policy.

Implement:

- exact command-name rules
- command-line regex rules
- ordered rule storage
- rule add/list/remove/update commands
- deterministic precedence
- regex validation at configuration time
- `cube defaults explain COMMAND...`
- tests for quoting, argument boundaries, overlapping rules, and explicit overrides

Recommended precedence:

```text
explicit CLI option
matching command rule
configured user default
built-in default
```

Workspace-specific defaults may be added in this phase or a separate phase if required.

### Phase 6 — Essential escape commands

Goal: make attachments comfortable for daily use without excessive complexity.

Add:

- `Ctrl-\\ ?` help
- `Ctrl-\\ \\` send literal prefix
- `Ctrl-\\ r` switch/request read-only behaviour
- configurable escape prefix
- robust escape parsing across partial reads and pasted input

Consider, but only add with confirmation safeguards:

- stop-and-detach command

### Phase 7 — Remaining high-value escape commands

Goal: add only proven workflow improvements.

Candidates:

- status display
- request/release interactive control
- terminal redraw/resynchronization
- stderr visibility toggle in term mode
- local connection diagnostics

Do not add commands merely to imitate tmux or screen. Each command needs an explicit use case and tests.

### Phase 8 — Workflow polish and remote usability

This additional phase captures supporting work that does not fit cleanly into the preceding feature phases.

Implement as needed:

- shell completion for Bash, Zsh, and Fish
- per-shell workspace selection integration
- remote manager selection and clear manager identity display
- connection timeout/retry UX
- `cube events` and `cube events --follow`
- `cube ps --watch`
- stable scripting/JSON output contracts
- man pages and examples
- command aliases only where they improve discoverability without ambiguity
- telemetry-free local diagnostics command such as `cube doctor`
- compatibility handling when client and manager expose different capabilities

## 14. Testing strategy

Each phase should include:

- command parser unit tests,
- golden tests for human-readable output,
- JSON-output contract tests,
- Unix-socket integration tests,
- manager error propagation tests,
- ambiguous-name and collision tests,
- authorization-denied tests,
- client/manager capability mismatch tests.

Attach phases additionally require pseudo-terminal integration tests that exercise:

- resize,
- detach without termination,
- reconnect,
- read-only enforcement,
- multiple observers,
- connection loss,
- control ownership rules.

Defaults phases require isolated temporary configuration directories and atomic-write failure tests.

## 15. Open design questions

The following can be resolved during implementation without changing the overall command shape:

1. Exact shell-integration mechanism for per-shell workspace selection.
2. Whether `cube workspace NAME` should always create when absent or require an interactive/configurable policy.
3. Exact generated-name normalization rules.
4. Whether foreground Ctrl-C is forwarded only, interpreted locally, or both according to mode.
5. Interactive-control ownership when two writable clients request the same TTY.
6. Whether `cube logs` renders a TTY history or initially emits recorded PTY bytes.
7. Whether workspace-specific defaults land in Phase 5 or a later dedicated phase.
8. Exact configurable escape-prefix syntax.

## 16. Stable decisions

The following are intentional and should not be changed casually:

- the executable is `cube`,
- `cube COMMAND...` is the common launch form,
- default launch policy is configurable and initially foreground,
- default mode is configurable and never inferred from executable names,
- TTY is the production built-in default,
- term is an explicit mode for split terminal stream capture,
- explicit CLI options always override defaults and rules,
- attachment is separate from process lifetime,
- Phase 2 starts with only one detach escape sequence,
- command-based defaults are user-configured policy, not built-in heuristics.
