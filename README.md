# Cubicle

Cubicle is a persistent development runtime that separates running processes from the terminals and user interfaces used to view and control them.

The runtime is built around two components:

- **Manager** — the control plane. It owns workspace namespaces, process identities, discovery, policy, and aggregated events.
- **Controller** — a deliberately thin per-process data plane. It owns one process group's PTY or pipes, persists output, and accepts direct attachment/control connections.

A future UI named **Desk** will compose independent local tabs and panes over Cubicle processes. Closing a Desk must never stop work running inside a Cubicle workspace.

## Early command model

```console
$ work "Project A"
$ dev make
Started make-1 in stream mode

$ tty vim test.c
Started vim-test.c-1 in tty mode

$ work ps
NAME          MODE      STATE
make-1        stream    running
vim-test.c-1  tty       running

$ attach make-1 out
$ attach vim-test.c-1 all
$ attach vim-test.c-1 out
```

The CLI names are exploratory. The architecture and protocol are the first implementation focus.

## Build

```console
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The manager currently provides a persistent registry CLI for workspaces and
process/controller records:

```console
./build/cubicle-manager --state-dir /tmp/cubicle-manager workspace create "Project A"
./build/cubicle-manager --state-dir /tmp/cubicle-manager workspace list
./build/cubicle-manager --state-dir /tmp/cubicle-manager process register \
  --workspace "Project A" \
  --friendly-name make-1 \
  --mode stream \
  --controller-id controller-1 \
  --control-socket /tmp/cubicle-run.sock
./build/cubicle-manager --state-dir /tmp/cubicle-manager process list --workspace "Project A"
```

The controller can launch stream-mode processes, mirror stdout/stderr, persist
channel logs and primitive events, serve a local control socket, and return the
child exit status:

```console
./build/cubicle-controller \
  --state-dir /tmp/cubicle-run \
  --control-socket /tmp/cubicle-run.sock \
  --mode stream -- make
```

If `--state-dir` is omitted, the controller creates
`.cubicle/controllers/<controller_id>` using a generated controller ID.

Stream stdin is kept open by default so clients can use `attach stdin`. Pass
`--stdin-policy eof` for noninteractive commands that should see immediate EOF
on stdin:

```console
./build/cubicle-controller \
  --stdin-policy eof \
  --state-dir /tmp/cubicle-run \
  --control-socket /tmp/cubicle-run.sock \
  --mode stream -- make
```

Pass `--daemon` to detach the controller before it launches the managed
process. In daemon mode, controller stdin/stdout/stderr are redirected to
`/dev/null`; use the control socket and persisted logs to interact with it:

```console
./build/cubicle-controller \
  --daemon \
  --state-dir /tmp/cubicle-run \
  --control-socket /tmp/cubicle-run.sock \
  --mode stream -- make
```

The initial control socket protocol accepts one command per connection:

```text
status
metadata
events after 0 100
read stdout 0 4096
read stderr 0 4096
attach stdout 0
attach stderr 0
attach stdin
terminate
signal 15
```

Output `attach` sends a header with the persisted catch-up length, then streams
raw future bytes until the process exits or the client disconnects. Stdin
`attach` sends a header, then forwards bytes from the socket to the managed
process stdin until the client disconnects.

Attached clients are fail-closed on backpressure: if a nonblocking attachment
cannot accept or forward bytes, the controller detaches that client instead of
silently dropping data.

See `docs/protocol.txt` for the current command, response, stream framing, and
event formats.

TTY modes and manager/controller routing are not implemented yet.
