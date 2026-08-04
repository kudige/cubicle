# Initial architecture

## Design principle

A running process is independent from every terminal, pane, tab, or UI that displays it.

Cubicle is the persistent runtime. Desk will be a local presentation layer that composes one or more attachments into tabs and panes. Multiple Desks may present the same processes differently and concurrently.

## Components

### Global manager

The manager is the control plane. It owns:

- workspace namespaces
- stable process IDs and workspace-local friendly names
- controller discovery and routing
- process metadata and lifecycle policy
- retained terminal process cleanup
- primitive-event aggregation and richer workspace events
- client authorization and attachment token issuance

The manager is not in the terminal byte path. If it exits, controllers and managed processes continue. Existing direct attachments may also continue; clients reconnect to the manager for discovery and management operations.

### Thin per-process controller

Each managed root process has one deliberately small controller. The controller owns:

- the root process and its independent process group or session
- PTY master and/or pipe endpoints
- process input
- output and error persistence
- terminal screen-state snapshots for PTY-backed processes
- direct client attachments
- resize and signal requests
- exit detection and a small durable primitive-event journal

A controller does not understand workspace names, Desk layouts, dependencies, health checks, or agent policy.

For TTY-backed modes, the controller also maintains the canonical virtual
terminal state as PTY bytes are captured. Rendering clients can attach by
requesting the current screen snapshot and then consuming only bytes after the
snapshot offset, instead of replaying all historical terminal output.

If a controller fails, only its managed process tree is affected. If the manager fails, no managed process should be affected.

## Process modes

The mode is selected before launch because descriptor wiring affects child behaviour.

### Stream

```text
stdin  <- pipe
stdout -> pipe
stderr -> pipe
```

Intended launch command:

```console
dev make
```

### TTY

```text
stdin  <-> PTY
stdout <-> PTY
stderr <-> PTY
```

Intended launch command:

```console
tty vim test.c
```

### TTY with captured stderr

```text
stdin  <-> PTY
stdout <-> PTY
stderr -> pipe
```

This permits independent stderr storage, display, filtering, and suppression. Its final user-facing command name remains undecided.

## Identity

Users address a process through either:

- a workspace-local friendly name such as `make-1`
- a stable globally unique process ID

Controller identities and socket locations are internal routing details. The manager maps process IDs to controllers.

## Workspace model

A workspace is a logical namespace, not an operating-system process group. Each managed process normally has its own process group or session so it can be controlled independently.

Workspace-wide stop or deletion is coordinated by the manager, which asks every member controller to terminate its process group.

## Attachments

An attachment is a temporary connection between a client and selected process channels. Initial channel selectors are:

- `in`
- `out`
- `err`
- `all`
- `noerr`

Examples:

```console
attach make-1 out
attach make-1 err
attach vim-test.c-1 all
attach vim-test.c-1 out
```

For a TTY process, `out` is observer mode: the client renders terminal state but does not send keyboard input.

The preferred attachment path is:

```text
client -> manager (resolve and authorize)
client -> controller (direct process I/O)
```

This keeps high-volume terminal traffic out of the manager.

## Events

Controllers emit primitive events with controller-local sequence numbers, including:

- process started or exited
- output available
- stderr available
- client attached or detached
- signal delivered

The manager consumes these events, maintains cursors, and creates ordered workspace events. After manager restart it reconnects to controllers and requests primitive events after the last consumed cursor.

Large output bytes are not embedded in the event journal. Events contain output cursor ranges; clients fetch the corresponding stream separately.

## Initial CLI direction

```console
work "Project A"
work ls
work connect "Project A"
work ps
work events
work events --follow

dev make
tty vim test.c

attach make-1 out
attach vim-test.c-1 all
stop make-1
```

These command names remain provisional while the protocol and runtime are implemented.
