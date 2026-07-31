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
```

This initial repository contains buildable manager and controller placeholders plus the first architecture notes.
