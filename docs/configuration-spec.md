# Cubicle Configuration System Specification

Status: Draft

This document defines Cubicle's configuration model, file locations, precedence rules, schema, validation requirements, and command-line management interface.

## 1. Goals

Cubicle configuration must support:

- machine-wide installation and runtime paths,
- manager state and listener settings,
- client manager selection,
- user-facing launch defaults such as foreground/background and stream/TTY/term mode,
- package and development installations,
- layered administrator and user overrides,
- future command-specific launch rules,
- diagnostics that show the effective value and its source,
- safe atomic updates from `cube config` and `cube defaults`.

Configuration parsing and file merging should use `libeconf`. Cubicle remains responsible for schema validation, precedence across non-file sources, security policy, path normalization, and converting values into typed runtime structures.

## 2. Design principles

1. Configuration is layered; later sources override earlier sources.
2. System runtime settings and user workflow preferences are separate concerns.
3. Explicit command-line options always override configured defaults.
4. The manager loads and validates its configuration once at startup.
5. Clients load their own effective configuration independently.
6. User configuration must not silently alter privileged system-manager paths or security settings.
7. Every effective value should be inspectable together with its source.
8. Invalid configuration must produce a clear error and must not be partially applied.
9. Configuration writes must be atomic.
10. Built-in defaults must allow Cubicle to run even when no files exist.

## 3. Configuration library

Cubicle should use `libeconf` for:

- parsing INI-style sectioned `key=value` files,
- merging vendor, system, runtime, and drop-in files,
- typed value retrieval where appropriate,
- reporting file and parse errors.

Cubicle must add a typed validation layer above `libeconf`:

```text
configuration files
        |
        v
libeconf parsing and merging
        |
        v
Cubicle schema and policy validation
        |
        v
path and endpoint normalization
        |
        v
typed cubicle_config_t
```

`libeconf` is an implementation dependency, not part of the public protocol or `libcubicle` API.

## 4. Configuration files and precedence

### 4.1 System manager

Recommended sources, from lowest to highest precedence:

```text
1. compiled-in defaults
2. /usr/lib/cubicle/config.cfg
3. /usr/lib/cubicle/config.cfg.d/*.cfg
4. /etc/cubicle/config.cfg
5. /etc/cubicle/config.cfg.d/*.cfg
6. /run/cubicle/config.cfg
7. /run/cubicle/config.cfg.d/*.cfg
8. manager-specific environment overrides
9. manager command-line options
```

Drop-in files are applied in lexical filename order within each directory.

`/usr/lib/cubicle` contains package/vendor defaults. `/etc/cubicle` contains administrator policy. `/run/cubicle` contains temporary overrides that do not survive reboot.

### 4.2 User client and user manager

Recommended sources, from lowest to highest precedence:

```text
1. compiled-in defaults
2. applicable system client defaults
3. $XDG_CONFIG_HOME/cubicle/config.cfg
4. $XDG_CONFIG_HOME/cubicle/config.cfg.d/*.cfg
5. client environment overrides
6. explicit cube command-line options
```

When `XDG_CONFIG_HOME` is unset, use:

```text
~/.config/cubicle
```

For a per-user manager, default locations should follow XDG:

```text
state:   $XDG_STATE_HOME/cubicle
runtime: $XDG_RUNTIME_DIR/cubicle
config:  $XDG_CONFIG_HOME/cubicle/config.cfg
```

Fallback for an unset `XDG_STATE_HOME`:

```text
~/.local/state/cubicle
```

A per-user manager must require `XDG_RUNTIME_DIR` or an explicitly configured runtime directory.

### 4.3 Launch option precedence

For foreground/background resolution:

```text
explicit --fg or --bg
        |
        v
matching command rule, when implemented
        |
        v
workspace-specific defaults, when implemented
        |
        v
user defaults
        |
        v
system defaults
        |
        v
built-in default: foreground
```

For process mode resolution:

```text
explicit --stream, --tty, or --term
        |
        v
matching command rule, when implemented
        |
        v
workspace-specific defaults, when implemented
        |
        v
user defaults
        |
        v
system defaults
        |
        v
built-in default: tty
```

The `term` mode remains available for explicit use when separately captured
terminal streams are needed, but ordinary interactive sessions should default
to `tty`.

During development, the built-in mode fallback is:

```text
tty if implemented
otherwise stream
```

Cubicle must not infer a mode from the executable name unless a user-defined command rule explicitly requests that behaviour.

## 5. Initial configuration schema

The initial system configuration should support these sections.

### 5.1 `[installation]`

```ini
[installation]
bindir=/usr/bin
libexecdir=/usr/libexec/cubicle
```

Keys:

- `bindir`: directory containing user-facing executables such as `cube`.
- `libexecdir`: directory containing internal package executables.

These values primarily support diagnostics, packaging, and development installations. Cubicle should prefer explicit absolute binary paths for internal process launches.

### 5.2 `[manager]`

```ini
[manager]
state_dir=/var/lib/cubicle
runtime_dir=/run/cubicle
listen=unix:///run/cubicle/manager.sock
controller_binary=/usr/libexec/cubicle/cubicle-controller
log_dir=/var/log/cubicle
socket_mode=0660
socket_group=
```

Required semantic keys:

- `state_dir`: persistent manager and process state.
- `runtime_dir`: ephemeral sockets and runtime files.
- `listen`: manager listener endpoint URI. Initial production deployments should
  use `unix://`; `tcp://host:port` is supported only when the daemon is started
  with an explicit insecure opt-in.
- `controller_binary`: absolute path to the controller executable.

Optional initial key:

- `log_dir`: persistent log directory when file logging is enabled.
- `socket_mode`: octal permissions applied to Unix manager sockets after bind.
  The default is `0660`.
- `socket_group`: optional group owner applied to Unix manager sockets after
  bind. Empty means keep the process default group.

Future versions may allow repeated `listen` entries for local and remote
listeners. Until authenticated remote transport is implemented, TCP listeners
are unauthenticated and must require an explicit `--allow-insecure` daemon
option.

### 5.3 `[controller]`

```ini
[controller]
debug=none
```

Keys:

- `debug`: `none`, `off`, or `false` records normal input events with lengths
  only. `input` adds input source, hex bytes, and escaped text to controller
  input events for terminal-response and keystroke debugging.

### 5.4 `[client]`

```ini
[client]
manager=unix:///run/cubicle/manager.sock
server_identity=
```

Keys:

- `manager`: default manager endpoint URI used by `cube` and `libcubicle` clients.
  `tcp://host:port` endpoints are valid only for explicitly insecure
  deployments until authenticated transport is implemented.
- `server_identity`: expected remote manager identity or certificate/key reference.

For local Unix endpoints, `server_identity` may be empty only when trust is established through an approved local trust policy.

### 5.5 `[defaults]`

```ini
[defaults]
launch=foreground
mode=tty
kill_cleanup=false
```

Keys:

- `launch`: `foreground` or `background`.
- `mode`: `stream`, `tty`, or `term`.
- `kill_cleanup`: `true` or `false`; when true, `cube kill NAME` behaves like
  `cube kill --cleanup NAME` unless overridden by a future explicit no-cleanup
  option.

The packaged default is:

```ini
[defaults]
launch=foreground
mode=tty
kill_cleanup=false
```

If TTY is not available in a development build, use `stream`.

### 5.5 `[retention]`

Deferred but reserved:

```ini
[retention]
completed_process_seconds=86400
event_history_seconds=604800
```

### 5.6 `[limits]`

Deferred but reserved:

```ini
[limits]
max_processes_per_workspace=256
max_attachment_clients_per_process=32
```

## 6. Example system configuration

```ini
[installation]
bindir=/usr/bin
libexecdir=/usr/libexec/cubicle

[manager]
state_dir=/var/lib/cubicle
runtime_dir=/run/cubicle
listen=unix:///run/cubicle/manager.sock
controller_binary=/usr/libexec/cubicle/cubicle-controller
log_dir=/var/log/cubicle

[client]
manager=unix:///run/cubicle/manager.sock

[defaults]
launch=foreground
mode=tty
```

## 7. Example user configuration

```ini
[client]
manager=unix:///run/user/1000/cubicle/manager.sock

[defaults]
launch=background
mode=tty
```

A user config may override client behaviour and launch defaults. It must not override system-manager state paths, controller binary paths, privileged listeners, or manager security policy when the user is merely connecting to a system manager.

## 8. Typed configuration model

Cubicle should expose one internal typed structure rather than passing strings throughout the program.

Illustrative shape:

```c
typedef enum cubicle_launch_default {
    CUBICLE_LAUNCH_FOREGROUND = 1,
    CUBICLE_LAUNCH_BACKGROUND = 2
} cubicle_launch_default_t;

typedef struct cubicle_config {
    char bindir[PATH_MAX];
    char libexecdir[PATH_MAX];

    char manager_state_dir[PATH_MAX];
    char manager_runtime_dir[PATH_MAX];
    char manager_listen_uri[CUBICLE_ENDPOINT_URI_MAX];
    char controller_binary[PATH_MAX];
    char manager_log_dir[PATH_MAX];

    char client_manager_uri[CUBICLE_ENDPOINT_URI_MAX];
    char client_server_identity[CUBICLE_SERVER_ID_MAX];

    cubicle_launch_default_t default_launch;
    cubicle_process_mode_t default_mode;
} cubicle_config_t;
```

The exact internal organization may use nested structures, but consumers should receive validated typed values.

## 9. Validation requirements

### 9.1 General

The loader must reject:

- unknown values for enums,
- malformed endpoint URIs,
- relative paths where absolute paths are required,
- empty required values,
- paths exceeding implementation limits,
- conflicting settings,
- invalid integers or integer overflow,
- unsupported configuration keys when strict mode is enabled.

Unknown keys should normally produce a warning during the v0 development cycle. Security-sensitive sections may reject unknown keys immediately.

### 9.2 Paths

The following must be absolute paths for a system manager:

- `manager.state_dir`
- `manager.runtime_dir`
- `manager.controller_binary`
- `manager.log_dir`, when set
- installation directories

Paths should be normalized lexically. Symlink resolution should not be required during parsing because some paths may not yet exist, but runtime ownership and permission checks must occur before use.

The manager must not follow unsafe writable-directory configurations without warning or rejection.

### 9.3 Runtime and state separation

`state_dir` and `runtime_dir` must not be treated interchangeably.

Persistent state includes:

- manager database,
- workspace metadata,
- process records,
- event history,
- retained output,
- controller recovery metadata.

Runtime state includes:

- Unix sockets,
- lock files,
- temporary attachment endpoints,
- optional PID files.

The listener socket should normally reside under `runtime_dir`, not `state_dir`.

### 9.4 Binary paths

`controller_binary` should resolve in this order:

```text
1. explicit absolute configured path
2. compiled-in libexec path
3. PATH lookup only in explicitly enabled development mode
```

A packaged system manager should not depend on an untrusted user `PATH`.

### 9.5 Endpoint validation

Supported initial schemes:

- `unix://`
- `tcp://` for development/testing only
- future `cubicle+tls://`

Production remote endpoints must eventually require authenticated encryption. Plain TCP must not be enabled as a production default.

## 10. Security and override policy

Configuration keys belong to policy classes.

### 10.1 User-overridable

Examples:

- `client.manager`
- `client.server_identity`
- `defaults.launch`
- `defaults.mode`

### 10.2 System-manager-only

Examples:

- `manager.state_dir`
- `manager.runtime_dir`
- `manager.listen`
- `manager.controller_binary`
- manager authentication policy
- manager grant-signing keys
- privileged resource limits

User config files must not override system-manager-only values used by a privileged manager process.

### 10.3 Environment variables

Environment variables are useful for development and one-off client selection, but should be limited and explicitly named, for example:

```text
CUBICLE_CONFIG
CUBICLE_MANAGER
CUBICLE_DEFAULT_LAUNCH
CUBICLE_DEFAULT_MODE
CUBICLE_STATE_DIR
CUBICLE_RUNTIME_DIR
CUBICLE_CONTROLLER_BINARY
```

Manager-only environment overrides should be ignored in privileged/setuid contexts and should be constrained by service configuration.

## 11. CLI configuration commands

### 11.1 Read-only diagnostics

```console
cube config show
cube config effective
cube config paths
cube config validate
cube config get defaults.mode
cube config get manager.state_dir
```

`cube config effective` should show each value and its source:

```text
Configuration sources:
  /usr/lib/cubicle/config.cfg
  /etc/cubicle/config.cfg
  /home/user/.config/cubicle/config.cfg

Effective values:
  manager.state_dir       /var/lib/cubicle
      source: /etc/cubicle/config.cfg
  manager.listen          unix:///run/cubicle/manager.sock
      source: /etc/cubicle/config.cfg
  defaults.launch         background
      source: /home/user/.config/cubicle/config.cfg
  defaults.mode           tty
      source: /home/user/.config/cubicle/config.cfg
```

### 11.2 Friendly defaults interface

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

These commands edit user configuration by default.

### 11.3 General mutation interface

Future administrative commands:

```console
cube config set client.manager unix:///run/user/1000/cubicle/manager.sock
cube config unset client.server_identity
sudo cube config --system set manager.state_dir /srv/cubicle
```

System writes must require an explicit `--system` scope and appropriate privilege.

## 12. Atomic updates

Configuration mutation commands must:

1. read the current target file,
2. parse and validate it,
3. apply the requested change,
4. write a temporary file in the same directory,
5. flush file contents,
6. set expected ownership and permissions,
7. atomically rename the temporary file over the target,
8. optionally fsync the containing directory,
9. re-read and validate the resulting file.

A failed update must leave the previous file intact.

The implementation must preserve unrelated sections and keys. Comment preservation is desirable but not required for the first implementation if clearly documented.

## 13. Source tracking

The effective configuration layer should retain metadata for every value:

```c
typedef struct cubicle_config_origin {
    cubicle_config_source_kind_t kind;
    char source_path[PATH_MAX];
    unsigned int line_number;
} cubicle_config_origin_t;
```

This supports:

- `cube config effective`,
- manager startup diagnostics,
- actionable validation errors,
- support and troubleshooting.

When line numbers are unavailable from the parsing library, at minimum retain the source file and precedence layer.

## 14. Manager startup behaviour

At startup, the manager must:

1. load all applicable system configuration layers,
2. apply service environment and explicit command-line overrides,
3. validate the complete effective configuration,
4. verify required directories and ownership,
5. create the runtime directory securely if permitted,
6. verify the controller binary,
7. bind configured listener endpoints,
8. log the effective configuration with sensitive values redacted,
9. expose non-sensitive resolved configuration through manager status or diagnostics.

Configuration changes do not need live reload in the first implementation. A manager restart is acceptable.

A future reload operation must be transactional and must distinguish reloadable from restart-required keys.

## 15. Client startup behaviour

The `cube` client must:

1. load applicable system client defaults,
2. merge user configuration,
3. apply environment overrides,
4. apply explicit CLI arguments,
5. validate the selected endpoint and launch defaults,
6. connect to the resolved manager.

Explicit CLI options always win:

```console
cube --fg --stream make
```

must ignore configured `defaults.launch` and `defaults.mode` for that invocation.

## 16. Future command-specific defaults

Reserved future syntax:

```ini
[defaults]
launch=foreground
mode=tty

[rule "make"]
command_regex=^make($| )
launch=foreground
mode=stream

[rule "interactive-editors"]
command_regex=^(vim|nvim|emacs)( |$)
launch=background
mode=tty
```

Command rules are not part of the initial configuration implementation.

When implemented, rules must be ordered and deterministic. Recommended precedence:

```text
explicit CLI option
first matching command rule
workspace default
user default
system default
built-in default
```

Rule regular expressions must be bounded in size and evaluated by a regex implementation resistant to pathological resource use.

## 17. Phased implementation

### Phase 1: Core read-only configuration

- Add `libeconf` dependency.
- Implement compiled defaults.
- Load `/usr/lib/cubicle/config.cfg` and `/etc/cubicle/config.cfg`.
- Parse `[manager]`, `[client]`, and `[defaults]`.
- Produce typed `cubicle_config_t`.
- Add schema validation and clear errors.
- Use configured manager socket, state directory, runtime directory, and controller binary.
- Add `cube config show`, `paths`, and `validate`.

### Phase 2: Drop-ins and source tracking

- Load `.d` directories in lexical order.
- Add `/run` temporary overrides.
- Track the origin of each effective value.
- Add `cube config effective`.
- Add manager startup logging of resolved non-sensitive settings.

### Phase 3: User configuration and defaults

- Load main XDG user configuration. `DONE`
- Load XDG user configuration drop-ins.
- Apply user launch defaults.
- Implement `cube defaults show/set/reset`.
- Implement atomic user-config writes.
- Enforce system-only versus user-overridable key policy.

### Phase 4: Environment and CLI overrides

- Add documented environment variables.
- Add explicit `--config` support for testing/development.
- Ensure CLI launch flags override configured defaults.
- Add precedence tests across all layers.

### Phase 5: Administrative mutation commands

- Implement `cube config get/set/unset`.
- Add `--system` and user scopes.
- Preserve unrelated configuration content.
- Enforce permissions and atomic replacement.

### Phase 6: Extended manager policy

- Add retention, limits, logging, and authentication settings.
- Add multiple listener support.
- Add secure redaction and policy validation.

### Phase 7: Workspace and command-specific defaults

- Add workspace-local defaults.
- Add ordered command and command-line regex rules.
- Add rule list/add/remove/test commands.
- Add diagnostics showing which rule matched.

### Phase 8: Reload and portability

- Classify reloadable versus restart-required settings.
- Add transactional manager reload.
- Add non-Linux fallback strategy if required.
- Add migration/versioning support for configuration schema changes.

## 18. Testing requirements

The configuration implementation must include tests for:

- missing files and built-in fallback,
- vendor/system/user precedence,
- lexical drop-in ordering,
- command-line override precedence,
- valid and invalid enum values,
- malformed endpoint URIs,
- absolute path enforcement,
- path length limits,
- permission failures,
- system-only key override attempts,
- atomic write interruption,
- preservation of unrelated keys,
- source reporting,
- XDG path resolution,
- development-mode binary fallback,
- manager and client use of the same validated semantics.

## 19. Initial acceptance criteria

The first usable configuration milestone is complete when:

1. The manager reads `/etc/cubicle/config.cfg` through `libeconf`.
2. `state_dir`, `runtime_dir`, `listen`, and `controller_binary` affect runtime behaviour.
3. The client reads `client.manager` and launch defaults.
4. Invalid configuration prevents startup with an actionable error.
5. `cube config validate` validates the same schema used by the manager and client.
6. `cube config effective` shows merged values and their sources.
7. Explicit `--fg`, `--bg`, `--stream`, `--tty`, and `--term` options override configuration.
8. User configuration cannot alter privileged system-manager paths.
9. Configuration writes are atomic.

## 20. Central rule

The configuration system should follow this principle:

> `libeconf` parses and merges configuration files; Cubicle owns the schema, security policy, typed values, precedence across non-file sources, and runtime meaning of every setting.
