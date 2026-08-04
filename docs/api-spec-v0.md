# Cubicle API Specification v0

Status: **Draft for parallel implementation**  
Date: 2026-07-31

This document defines the transport-neutral semantic contract shared by:

- the Cubicle manager daemon,
- per-process Cubicle controllers,
- `libcubicle`,
- command-line clients,
- the future Desk UI,
- local and remote agents.

The exact wire encoding is deliberately not fixed here. JSON over a framed stream may be used first, while the semantic API remains stable.

## 1. Architectural boundary

Cubicle has a control plane and a data plane.

```text
CLI / Desk / agent
        |
        v
    libcubicle
        |
        +---- manager API ----> global manager daemon
        |                         workspace namespace
        |                         identity and authorization
        |                         process discovery and lifecycle
        |                         event aggregation
        |                         attachment grants
        |
        +---- controller API --> one thin process controller
                                  stdin/stdout/stderr or PTY
                                  durable output
                                  live attachments
                                  process-group control
```

Rules:

1. Namespace, identity, policy, lifecycle, and discovery operations go through the manager.
2. Live process I/O goes directly between the client and controller after manager authorization.
3. The public API never exposes controller IDs as user-visible identities.
4. A process ID is stable for the life and retained history of a managed process.
5. Manager and controller endpoints are transport-neutral and may be local or remote.
6. Every manager connection is authenticated, including local connections.
7. A public key authenticates an identity; workspace ACLs authorize that identity.

## 2. Versioning

Every connection negotiates:

```text
protocol_major
protocol_minor
capabilities[]
```

Compatibility rules:

- Different major versions are incompatible.
- A server may accept an older minor version.
- Unknown optional fields must be ignored.
- Unknown required capabilities must cause negotiation failure.
- Method semantics defined in this document must not change incompatibly within major version 0.

Initial version:

```text
protocol_major = 0
protocol_minor = 1
```

## 3. Stable identities

Identifiers are opaque lowercase hexadecimal strings representing 128-bit values.

```text
manager_id
workspace_id
process_id
client_key_id
session_id
request_id
attachment_grant_id
```

Clients must not infer meaning from identifier contents.

A globally unambiguous process reference is:

```text
manager_id + process_id
```

Friendly names are workspace-local conveniences and are not permanent identities.

## 4. Endpoint model

The API uses endpoint URIs rather than Unix socket paths.

Examples:

```text
unix:///run/user/1000/cubicle/manager.sock
cubicle+tls://build.example.com:7443
cubicle+quic://build.example.com:7444
```

Public endpoint structure:

```c
typedef struct cubicle_endpoint {
    char uri[CUBICLE_ENDPOINT_URI_MAX];
    char server_identity[CUBICLE_SERVER_ID_MAX];
} cubicle_endpoint_t;
```

`server_identity` identifies the manager or controller key/certificate expected by the client. It may be omitted only when trust is established by an explicitly configured trust store.

## 5. Authentication and authorization

### 5.1 Identity

The default client identity is an Ed25519 public key.

The private key remains on the client and may be accessed through:

- a private-key file,
- `ssh-agent`,
- an OS key store,
- a hardware-backed signing provider.

The library must support an abstract signer and must not require direct private-key bytes.

```c
typedef struct cubicle_signer cubicle_signer_t;

typedef int (*cubicle_sign_fn)(
    void *context,
    const unsigned char *message,
    size_t message_length,
    unsigned char *signature,
    size_t *signature_length);
```

### 5.2 Manager authentication handshake

All manager transports perform a logical challenge-response handshake:

1. Client opens a transport connection.
2. Client and manager exchange protocol versions, nonces, and supported capabilities.
3. Manager proves its configured server identity through the secure transport or signed transcript.
4. Client sends its public key and signs the complete handshake transcript.
5. Manager verifies the signature.
6. Manager creates an authenticated session associated with the key identity.

The signed transcript must bind at least:

```text
protocol version
manager identity
client nonce
manager nonce
connection/session identifier
negotiated capabilities
```

No manager request may execute before authentication succeeds.

### 5.3 Workspace ACLs

Authentication does not imply workspace access.

Each workspace has ACL entries:

```text
workspace_id
client_key_id
capability set
created_at
revoked_at
label
```

Initial capabilities:

```text
workspace.read
workspace.rename
workspace.stop
workspace.delete
workspace.manage_keys

process.start
process.read
process.observe
process.input
process.signal
process.remove

events.read
```

Convenience roles may map to capability sets:

```text
observer
operator
administrator
owner
```

Roles are not authoritative protocol primitives; explicit capabilities are.

### 5.4 Bootstrap

A new manager installation has an implementation-defined local administrative bootstrap path.

A newly created workspace must acquire an initial owner key atomically with creation, unless the authenticated caller already has manager-level administrative authority.

### 5.5 Attachment grants

Controllers do not maintain workspace ACLs.

The manager issues a short-lived signed attachment grant containing:

```text
grant_id
manager_id
workspace_id
process_id
client_key_id
permitted channels
attachment mode
issued_at
expires_at
connection limit
nonce
```

The controller verifies the grant using the manager's grant-signing public key.

A grant authorizes connection establishment. Once connected, the controller may keep the attachment alive after grant expiry. Active revocation is optional in v0.

## 6. Common error model

All RPC failures return a structured error.

```c
typedef enum cubicle_error_code {
    CUBICLE_OK = 0,
    CUBICLE_ERR_INVALID_ARGUMENT,
    CUBICLE_ERR_NOT_FOUND,
    CUBICLE_ERR_ALREADY_EXISTS,
    CUBICLE_ERR_AMBIGUOUS_NAME,
    CUBICLE_ERR_PERMISSION_DENIED,
    CUBICLE_ERR_AUTHENTICATION_FAILED,
    CUBICLE_ERR_SESSION_EXPIRED,
    CUBICLE_ERR_UNSUPPORTED,
    CUBICLE_ERR_INVALID_STATE,
    CUBICLE_ERR_CONFLICT,
    CUBICLE_ERR_TIMEOUT,
    CUBICLE_ERR_MANAGER_UNAVAILABLE,
    CUBICLE_ERR_CONTROLLER_UNAVAILABLE,
    CUBICLE_ERR_PROTOCOL,
    CUBICLE_ERR_IO,
    CUBICLE_ERR_RESOURCE_LIMIT,
    CUBICLE_ERR_INTERNAL
} cubicle_error_code_t;

typedef struct cubicle_error {
    cubicle_error_code_t code;
    int system_errno;
    bool retryable;
    char message[256];
} cubicle_error_t;
```

A server error response must include:

```text
request_id
error code
human-readable message
retryable flag
optional structured details
```

## 7. Common request semantics

Every manager request contains:

```text
request_id
method
parameters
optional workspace_id
optional idempotency_key
optional deadline
```

Every response contains:

```text
request_id
success result OR structured error
```

### 7.1 Idempotency

Mutating operations such as `workspace.create` and `process.start` accept an idempotency key.

If a client retries the same authenticated operation with the same idempotency key, the manager must return the original result rather than repeat the operation.

Idempotency keys are scoped to authenticated client identity and method.

### 7.2 Deadlines

A client may provide a deadline or timeout. Expiration means the client is no longer waiting; it does not necessarily cancel an operation already committed by the manager.

Operations requiring cancellation must expose an explicit cancellation or lifecycle method.

## 8. Public object models

### 8.1 Workspace

```c
typedef struct cubicle_workspace_info {
    char manager_id[CUBICLE_ID_MAX];
    char id[CUBICLE_ID_MAX];
    char name[CUBICLE_NAME_MAX];
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    uint64_t process_count;
    uint64_t running_process_count;
} cubicle_workspace_info_t;
```

### 8.2 Process mode

```c
typedef enum cubicle_process_mode {
    CUBICLE_PROCESS_STREAM = 1,
    CUBICLE_PROCESS_TTY = 2,
    CUBICLE_PROCESS_TTY_CAPTURED_STDERR = 3
} cubicle_process_mode_t;
```

Semantics:

- `STREAM`: stdin, stdout, and stderr are independent streams.
- `TTY`: stdin, stdout, and stderr are attached to one PTY.
- `TTY_CAPTURED_STDERR`: stdin and stdout use a PTY; stderr is independently captured.

### 8.3 Process state

```c
typedef enum cubicle_process_state {
    CUBICLE_PROCESS_ALLOCATED,
    CUBICLE_PROCESS_STARTING,
    CUBICLE_PROCESS_RUNNING,
    CUBICLE_PROCESS_STOPPING,
    CUBICLE_PROCESS_DRAINING,
    CUBICLE_PROCESS_COMPLETED,
    CUBICLE_PROCESS_FAILED,
    CUBICLE_PROCESS_LOST,
    CUBICLE_PROCESS_REMOVED
} cubicle_process_state_t;
```

`COMPLETED` means the managed process exited and retained output/status remain available.

### 8.4 Process information

```c
typedef struct cubicle_process_info {
    char manager_id[CUBICLE_ID_MAX];
    char workspace_id[CUBICLE_ID_MAX];
    char id[CUBICLE_ID_MAX];
    char friendly_name[CUBICLE_NAME_MAX];

    cubicle_process_mode_t mode;
    cubicle_process_state_t state;
    bool saved;

    int exit_code;
    int termination_signal;
    bool has_exit_status;

    uint64_t stdout_offset;
    uint64_t stderr_offset;
    uint64_t tty_offset;

    uint64_t created_at_ms;
    uint64_t started_at_ms;
    uint64_t exited_at_ms;

    /* Optional local diagnostics. Never required for remote operation. */
    int64_t local_pid;
    int64_t local_pgid;
} cubicle_process_info_t;
```

Controller endpoint and controller ID are intentionally absent.

## 9. Manager API

### 9.1 Session methods

#### `manager.ping`

Returns manager identity, protocol version, server time, and uptime.

#### `manager.status`

Returns:

```text
manager_id
started_at
workspace_count
process_count
controller_count
active_client_sessions
capabilities[]
```

#### `manager.reconcile`

Requests reconciliation of persistent manager records with live and retained controllers.

Requires manager administration authority.

#### `manager.cleanup`

Removes retained terminal process records and controller state. Live and saved
processes are never stopped or removed.

Request:

```text
workspace_id            # optional; workspace name or id
```

Response:

```text
removed_count
skipped_live_count
skipped_saved_count
failed_count
```

#### `manager.shutdown`

Stops the manager daemon without stopping process controllers or managed processes unless an explicit separate option is supplied.

### 9.2 Workspace methods

#### `workspace.create`

Request:

```text
name
directory?                # defaults to the manager's current directory when omitted
initial_owner_public_key? 
initial_owner_label?
idempotency_key
```

Response: `workspace_info`.

#### `workspace.get`

Request:

```text
workspace_id OR workspace_name
```

Response: `workspace_info`.

#### `workspace.list`

Request may contain pagination and name filtering.

Response: list of workspaces accessible to the authenticated key.

#### `workspace.rename`

Request:

```text
workspace_id
new_name
```

#### `workspace.stop`

Request:

```text
workspace_id
grace_period_ms
force_after_grace
```

The manager asks every live controller in the workspace to terminate its process group. Workspace membership remains intact.

#### `workspace.delete`

Request:

```text
workspace_id
stop_running_processes
remove_retained_processes
```

Deletion must fail with `INVALID_STATE` if live processes exist and `stop_running_processes` is false.

### 9.3 Workspace key methods

#### `workspace.key.add`

Request:

```text
workspace_id
public_key
key_type
label
capabilities[]
```

#### `workspace.key.list`

Returns non-secret public key records and capabilities.

#### `workspace.key.update`

Updates label or capabilities.

#### `workspace.key.revoke`

Revokes future manager operations and future attachment grants for that key in the workspace.

### 9.4 Process methods

#### `process.start`

Request:

```text
workspace_id
friendly_name?            # manager allocates when omitted
mode
stdin_policy              # open or eof
argv[]
environment[]?            # explicit NAME=value entries
inherit_environment?      # default false for remote-safe semantics
cwd?                       # defaults to workspace.directory when omitted
tty_rows?
tty_cols?
retention_policy?
idempotency_key
```

Response: `process_info` in `STARTING`, `RUNNING`, or a terminal failure state.

The manager:

1. authorizes `process.start`,
2. allocates process ID and friendly name,
3. creates durable manager state,
4. launches a thin controller,
5. records its internal route,
6. returns the stable process identity.

#### `process.get`

Request:

```text
process_id
```

or, for interactive convenience:

```text
workspace_id
friendly_name
```

Friendly-name lookup must return `AMBIGUOUS_NAME` if not unique.

#### `process.list`

Filters may include:

```text
workspace_id
states[]
mode
name_prefix
include_completed
pagination
```

#### `process.signal`

Request:

```text
process_id
signal_number
```

The manager resolves the controller and records the action as an auditable event.

#### `process.terminate`

Request:

```text
process_id
grace_period_ms
force_after_grace
```

#### `process.kill`

Immediately requests forceful process-group termination.

#### `process.save`

Marks a process record as saved so cleanup commands skip it.

Request:

```text
process_id
```

Response contains current process information.

#### `process.unsave`

Clears a process record's saved flag.

Request:

```text
process_id
```

Response contains current process information.

#### `process.wait`

Request:

```text
process_id
desired_states[]
timeout_ms
```

Response contains current process information and whether the wait condition was satisfied.

#### `process.remove`

Removes retained process metadata and output. It must not remove a live process.

#### `process.read_output`

Bounded convenience read routed through the manager.

Request:

```text
process_id
stream                 # stdout, stderr, tty
offset
maximum_length
```

Response:

```text
start_offset
next_offset
end_of_stream
data bytes
```

This method is intended for bounded reads and agents. Continuous output uses a direct controller attachment.

### 9.5 Attachment method

#### `attachment.request`

Request:

```text
process_id
channels[]             # stdin, stdout, stderr, tty
mode                   # observer or interactive
stdout_offset?
stderr_offset?
tty_offset?
tty_rows?
tty_cols?
route_preference       # auto, direct, relay
```

Response:

```text
grant_id
endpoint
route                   # direct or relay
signed_token
expires_at
permitted_channels[]
mode
```

The manager may reduce requested permissions but must not silently increase them.

For v0, relay routing may return `UNSUPPORTED` while remaining part of the contract.

### 9.6 Event methods

#### `events.list`

Request:

```text
workspace_id?
process_id?
after_global_sequence?
after_workspace_sequence?
limit
```

Response: ordered event list.

#### `events.subscribe`

Creates a resumable event stream using the same filters and cursor semantics as `events.list`.

A disconnected client resumes from its last acknowledged or observed sequence.

## 10. Event model

```c
typedef enum cubicle_event_type {
    CUBICLE_EVENT_WORKSPACE_CREATED,
    CUBICLE_EVENT_WORKSPACE_RENAMED,
    CUBICLE_EVENT_WORKSPACE_STOPPING,
    CUBICLE_EVENT_WORKSPACE_STOPPED,
    CUBICLE_EVENT_WORKSPACE_DELETED,

    CUBICLE_EVENT_PROCESS_ALLOCATED,
    CUBICLE_EVENT_PROCESS_STARTED,
    CUBICLE_EVENT_PROCESS_STATE_CHANGED,
    CUBICLE_EVENT_PROCESS_EXITED,
    CUBICLE_EVENT_PROCESS_SIGNALLED,
    CUBICLE_EVENT_PROCESS_REMOVED,

    CUBICLE_EVENT_OUTPUT_AVAILABLE,
    CUBICLE_EVENT_CLIENT_ATTACHED,
    CUBICLE_EVENT_CLIENT_DETACHED,
    CUBICLE_EVENT_INPUT_CONTROL_CHANGED,

    CUBICLE_EVENT_CONTROLLER_LOST,
    CUBICLE_EVENT_CONTROLLER_RECOVERED,
    CUBICLE_EVENT_MANAGER_RECOVERED,

    CUBICLE_EVENT_KEY_ADDED,
    CUBICLE_EVENT_KEY_UPDATED,
    CUBICLE_EVENT_KEY_REVOKED
} cubicle_event_type_t;
```

Event structure:

```c
typedef struct cubicle_event {
    uint64_t global_sequence;
    uint64_t workspace_sequence;
    uint64_t process_sequence;
    uint64_t timestamp_ms;

    cubicle_event_type_t type;

    char workspace_id[CUBICLE_ID_MAX];
    char process_id[CUBICLE_ID_MAX];
    char actor_key_id[CUBICLE_ID_MAX];

    char payload_json[CUBICLE_EVENT_PAYLOAD_MAX];
} cubicle_event_t;
```

Output bytes are not embedded in events. `OUTPUT_AVAILABLE` contains stream and offset range.

## 11. Controller API

The controller API is deliberately smaller than the manager API.

A controller understands:

- one stable process identity,
- process-group lifecycle,
- raw streams or PTY state,
- durable output offsets,
- primitive local events,
- manager-signed grants,
- connected attachments.

It does not understand workspace names, roles, restart policy, dependencies, or high-level workflow meaning.

### 11.1 Controller session establishment

A client connects to the endpoint in an attachment grant and presents the signed token.

The controller verifies:

```text
manager signature
manager identity
process ID
expiry
nonce/replay policy
channel permissions
connection limit
```

### 11.2 Attachment operations

#### `controller.attach`

Establishes an observer or interactive attachment and returns accepted channels and current offsets.

#### `controller.read`

Reads framed output from granted stdout, stderr, or tty channels.

#### `controller.snapshot`

Returns the current virtual terminal screen for PTY-backed processes. The
result contains `rows`, `columns`, cursor position/visibility, the raw `offset`
used to produce the screen, and row-major cells with text and render attributes.
Clients that render their own terminal view should draw this snapshot first and
then read live output from `offset`.

#### `controller.write`

Writes bytes to stdin or PTY input when granted.

#### `controller.resize`

Updates canonical PTY rows and columns when the attachment holds input/control authority.

#### `controller.detach`

Closes the attachment without affecting the process.

#### `controller.request_input_control`

Requests interactive control of a TTY process. v0 may implement one controller and multiple observers.

#### `controller.release_input_control`

Relinquishes interactive control.

### 11.3 Manager-to-controller operations

The manager uses an authenticated internal controller session for:

```text
controller.status
controller.signal
controller.terminate
controller.kill
controller.read_output
controller.events_after
controller.retire
```

These internal operations may use the same transport framework but are not directly exposed as normal user-authorized calls.

## 12. Attachment framing requirements

The attachment protocol must support multiplexed channels over transports that preserve ordered bytes.

Each frame contains at least:

```text
frame_type
channel
sequence or offset
payload_length
payload
```

Frame types include:

```text
DATA
ACK
RESIZE
CONTROL_REQUEST
CONTROL_GRANTED
CONTROL_RELEASED
EOF
ERROR
PING
PONG
DETACH
```

The protocol must not rely on packet boundaries from the underlying transport.

Backpressure rule:

- A slow observer must never block the managed process.
- A bounded controller-side queue may be used.
- When limits are exceeded, that attachment is detached with an explicit error.
- Failure of one attachment must not terminate the controller.

## 13. `libcubicle` public role

`libcubicle` is a typed SDK over the manager and controller APIs.

It provides:

- endpoint parsing,
- manager identity verification,
- key-based authentication,
- session management,
- request framing and serialization,
- typed manager calls,
- attachment-grant handling,
- direct controller connections,
- event subscription and cursor recovery,
- error translation,
- memory ownership helpers,
- optional terminal raw-mode helpers.

It must not duplicate manager authorization or lifecycle policy.

## 14. `libcubicle` manager wrappers

Opaque handles:

```c
typedef struct cubicle_manager cubicle_manager_t;
typedef struct cubicle_event_subscription cubicle_event_subscription_t;
```

Connection:

```c
cubicle_error_code_t cubicle_manager_connect(
    const cubicle_manager_options_t *options,
    cubicle_manager_t **manager_out);

void cubicle_manager_disconnect(cubicle_manager_t *manager);

const cubicle_error_t *cubicle_manager_last_error(
    const cubicle_manager_t *manager);
```

Manager:

```c
cubicle_error_code_t cubicle_manager_ping(...);
cubicle_error_code_t cubicle_manager_status(...);
cubicle_error_code_t cubicle_manager_cleanup(...);
cubicle_error_code_t cubicle_manager_reconcile(...);
cubicle_error_code_t cubicle_manager_shutdown(...);
```

Workspace:

```c
cubicle_error_code_t cubicle_workspace_create(...);
cubicle_error_code_t cubicle_workspace_get(...);
cubicle_error_code_t cubicle_workspace_list(...);
cubicle_error_code_t cubicle_workspace_rename(...);
cubicle_error_code_t cubicle_workspace_stop(...);
cubicle_error_code_t cubicle_workspace_delete(...);
```

Workspace keys:

```c
cubicle_error_code_t cubicle_workspace_key_add(...);
cubicle_error_code_t cubicle_workspace_key_list(...);
cubicle_error_code_t cubicle_workspace_key_update(...);
cubicle_error_code_t cubicle_workspace_key_revoke(...);
```

Processes:

```c
cubicle_error_code_t cubicle_process_start(...);
cubicle_error_code_t cubicle_process_get(...);
cubicle_error_code_t cubicle_process_list(...);
cubicle_error_code_t cubicle_process_signal(...);
cubicle_error_code_t cubicle_process_terminate(...);
cubicle_error_code_t cubicle_process_kill(...);
cubicle_error_code_t cubicle_process_wait(...);
cubicle_error_code_t cubicle_process_remove(...);
cubicle_error_code_t cubicle_process_read_output(...);
```

Attachments:

```c
cubicle_error_code_t cubicle_attachment_request(
    cubicle_manager_t *manager,
    const cubicle_attachment_request_t *request,
    cubicle_attachment_grant_t *grant_out);
```

Events:

```c
cubicle_error_code_t cubicle_events_list(...);
cubicle_error_code_t cubicle_events_subscribe(...);
cubicle_error_code_t cubicle_events_next(...);
void cubicle_events_unsubscribe(...);
```

## 15. `libcubicle` direct attachment wrappers

Opaque handle:

```c
typedef struct cubicle_attachment cubicle_attachment_t;
```

Terminal snapshot data:

```c
typedef struct cubicle_terminal_cell {
    char text[32];
    char sgr[96];
} cubicle_terminal_cell_t;

typedef struct cubicle_terminal_snapshot {
    unsigned int rows;
    unsigned int cols;
    unsigned int cursor_row;
    unsigned int cursor_col;
    bool cursor_visible;
    uint64_t offset;
    cubicle_terminal_cell_t *cells;
} cubicle_terminal_snapshot_t;
```

API:

```c
cubicle_error_code_t cubicle_attachment_request(
    cubicle_client_t *client,
    const cubicle_attachment_request_t *request,
    cubicle_attachment_grant_t *grant_out);

cubicle_error_code_t cubicle_attachment_connect(
    const cubicle_attachment_grant_t *grant,
    const cubicle_attachment_options_t *options,
    cubicle_attachment_t **attachment_out);

ssize_t cubicle_attachment_read(
    cubicle_attachment_t *attachment,
    void *buffer,
    size_t length);

ssize_t cubicle_attachment_read_stream(
    cubicle_attachment_t *attachment,
    cubicle_stream_kind_t stream,
    void *buffer,
    size_t length,
    bool *end_of_stream_out);

ssize_t cubicle_attachment_write(
    cubicle_attachment_t *attachment,
    const void *data,
    size_t length);

cubicle_error_code_t cubicle_attachment_resize(
    cubicle_attachment_t *attachment,
    unsigned int rows,
    unsigned int columns);

cubicle_error_code_t cubicle_attachment_resize_tracked(...);
cubicle_error_code_t cubicle_attachment_status(...);
cubicle_error_code_t cubicle_attachment_snapshot(
    cubicle_attachment_t *attachment,
    cubicle_terminal_snapshot_t *snapshot_out);
void cubicle_terminal_snapshot_cleanup(
    cubicle_terminal_snapshot_t *snapshot);
cubicle_error_code_t cubicle_attachment_detach(
    cubicle_attachment_t *attachment);
void cubicle_attachment_disconnect(cubicle_attachment_t *attachment);
```

These calls bypass the manager after grant issuance, except when relay routing is selected.
For TTY attachments, `cubicle_attachment_snapshot` returns the current rendered
terminal cells and advances the attachment TTY read offset to the snapshot
offset, so subsequent reads consume only live output after the snapshot.

## 16. Transport abstraction inside `libcubicle`

```c
typedef struct cubicle_transport_vtable {
    cubicle_error_code_t (*connect)(void *context,
                                     const cubicle_endpoint_t *endpoint);
    cubicle_error_code_t (*send_frame)(void *context,
                                       const void *data,
                                       size_t length);
    cubicle_error_code_t (*receive_frame)(void *context,
                                          void *buffer,
                                          size_t capacity,
                                          size_t *length_out,
                                          int timeout_ms);
    void (*close)(void *context);
} cubicle_transport_vtable_t;
```

Initial implementations may include:

```text
transport_unix
transport_tls
transport_mock
```

Future implementations may include QUIC and manager relay without changing the public semantic API.

## 17. Memory ownership

Rules for the C API:

1. Caller-owned input strings remain valid only for the duration of a synchronous call unless documented otherwise.
2. Fixed-size output records are caller allocated.
3. Variable-length lists and byte buffers are allocated by `libcubicle`.
4. Every allocated result type has a matching `*_free` function.
5. Handles are opaque and released only by their documented close/disconnect functions.
6. No public structure contains internal transport or controller pointers.

## 18. Threading model

Initial contract:

- Distinct manager handles may be used concurrently.
- A single manager handle is not required to support concurrent calls unless created with a thread-safe option.
- Attachment handles may have one reader and one writer concurrently.
- Event subscriptions are independent handles.
- Callback-based APIs, if added, must document callback thread context.

## 19. Retry rules

`libcubicle` may automatically reconnect and retry only when all are true:

- the operation is read-only, or has an idempotency key,
- no partial streaming response has been delivered,
- the deadline has not expired,
- the server error or transport failure is marked retryable.

Attachment byte writes must never be automatically replayed unless the protocol provides acknowledged sequence numbers proving replay safety.

## 20. Audit requirements

The manager records security-relevant operations with authenticated identity:

```text
workspace key changes
workspace stop/delete
process start/signal/terminate/kill/remove
attachment grant issuance
manager administrative operations
```

Controller primitive events may record grant ID and client key ID but should not make authorization decisions beyond grant validation.

## 21. Initial implementation subset

The first parallel manager/library milestone should implement:

```text
manager.ping
manager.status
manager.cleanup

workspace.create
workspace.get
workspace.list
workspace.key.add
workspace.key.list
workspace.key.revoke

process.start
process.get
process.list
process.signal
process.terminate
process.read_output

attachment.request
controller.attach
controller.read
controller.write
controller.detach

events.list
events.subscribe
```

Initial transports:

```text
manager: Unix socket
controller: Unix socket
library tests: mock transport
```

Authentication is required even for Unix sockets. Filesystem permissions and peer credentials are supplementary controls only.

## 22. Parallel development contract

The manager/controller agent and the `libcubicle` implementation should share:

1. method names,
2. request and response schemas,
3. error codes,
4. identity formats,
5. authentication transcript fixtures,
6. attachment grant fixtures,
7. event fixtures,
8. conformance tests.

Recommended workflow:

```text
API fixtures and conformance tests
        |                 |
        v                 v
libcubicle client     manager/controller server
        \                 /
         \               /
          integration tests
```

Neither implementation should treat the initial JSON or framing representation as the public API. The semantic contract in this document is authoritative.

## 23. Open decisions

The following remain intentionally open for focused design:

- exact framed encoding: JSON, CBOR, MessagePack, or protobuf,
- secure remote transport implementation,
- manager relay framing,
- grant signature encoding,
- precise event payload schemas,
- completed-controller retention defaults,
- active attachment revocation,
- input-control arbitration policy,
- environment inheritance policy for remote managers,
- file synchronization and remote workspace provisioning.

These decisions must preserve the architectural and semantic rules defined above.
