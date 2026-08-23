# macros(1)

## Name

`macros` - workspace-specific Desk shortcuts that send saved commands to panes.

## Description

Cubicle macros are saved per workspace. A macro has a numeric position, a name,
command text, an optional custom key binding, and the pane that was active when
the macro was created.

Running a macro sends its saved text followed by Enter to the target pane. If
the saved target pane is no longer available or is detached, Desk sends the
macro to the current active pane instead.

Macros are managed from `desk`. CLI management is not implemented yet.

## Opening The Macro Menu

Start Desk:

```sh
desk
```

Open the Desk menu with the configured menu binding. By default:

```text
Prefix-o
```

From the open menu, press:

```text
m
```

This opens the macro manager for the current workspace.

## Creating A Macro

In the macro manager:

```text
Enter
```

on `New macro`, then enter:

1. the macro name, such as `build`
2. the macro text, such as `make test`

Desk saves the macro in the current workspace. The active pane at creation time
is stored as the preferred target pane.

Example macro:

```text
name: build
text: make test
```

When run, this sends:

```text
make test<Enter>
```

## Running Macros

Macros 1 through 10 always have default prefixed bindings:

```text
Prefix-1   Run macro 1
Prefix-2   Run macro 2
Prefix-3   Run macro 3
Prefix-4   Run macro 4
Prefix-5   Run macro 5
Prefix-6   Run macro 6
Prefix-7   Run macro 7
Prefix-8   Run macro 8
Prefix-9   Run macro 9
Prefix-0   Run macro 10
```

Macros after 10 can still be run with custom key bindings.

## Editing Macros

Open the macro manager, select a macro, and press:

```text
Enter
```

Desk prompts for the name and text again. Existing custom key bindings and the
saved target pane are preserved.

## Deleting Macros

Open the macro manager, select a macro, and press:

```text
d
```

The macro is removed from the current workspace.

## Reordering Macros

Macro order controls the default numeric bindings. In the macro manager:

```text
u   Move selected macro up
n   Move selected macro down
```

For example, moving `build` to position 1 makes it run from `Prefix-1`.

## Custom Key Bindings

Open the key bindings overlay:

```text
Prefix-?
```

Macros appear at the bottom in numeric order, for example:

```text
[macro.1]  1. build
[macro.2]  2. run gdb
[macro.3]  3. kill test
```

Select a macro row and press:

```text
e
```

Enter a key name such as:

```text
C-B
Prefix-b
C-S-Right
```

To remove only the custom key binding, enter:

```text
none
```

Default numeric bindings such as `Prefix-1` remain available.

## Manager Compatibility

Macros require a manager that implements the `workspace.macro.*` API. If Desk is
newer than the running manager, Desk still opens, but macro management is
unavailable until the manager is restarted with a build that includes macro
support.

After upgrading, restart the manager before using macros.
