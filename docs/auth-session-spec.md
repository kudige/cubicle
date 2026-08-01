# Cubicle Authentication, Session, and Access Specification

Status: Draft implementation specification  
Date: 2026-08-01

This document defines the operational and protocol requirements for Cubicle authentication, session reuse, user-space and system managers, automatic identity generation, workspace access enrollment, and controller attachment authorization.

The design goal is strong authentication with low friction:

- expensive public-key work happens rarely,
- normal `cube` commands are lean,
- controller attachments are direct and inexpensive,
- user-space managers work without root,
- `cubicle-manager daemon` works without manual key or certificate setup,
- granting workspace access requires only a public key and one command.

## 1. Design principles

1. Every manager connection is authenticated, including Unix-domain-socket connections.
2. A full public-key handshake should occur only when a client first establishes or refreshes a manager/workspace session.
3. Later CLI commands should resume an existing session using a compact symmetric authenticator.
4. Controller attachments should use short-lived manager-issued grants rather than repeating workspace authentication.
5. The same logical protocol must work for user-space and system-wide managers.
6. Manager and client identities must be generated automatically when absent.
7. No mandatory certificate-generation command or external PKI setup is required for basic operation.
8. Adding a key to a workspace must be a single CLI operation.
9. A request payload never chooses its execution UID or GID.
10. Plain TCP is development/test-only. Production remote connections require authenticated encryption.

## 2. Architectural layers

Cubicle separates transport protection, client authentication, session reuse, workspace authorization, and controller attachment authorization.

```text
Transport
    Unix socket or TLS 1.3
        |
        v
Manager authentication
    manager identity verification
    client Ed25519 challenge-response
        |
        v
Workspace authorization
    key ACL and capability checks
        |
        v
Reusable workspace session
    short-lived session credential
        |
        v
Normal RPC operations
    cheap session resume
        |
        v
Attachment grant
    short-lived manager-signed capability
        |
        v
Direct controller connection
```

## 3. Identities

### 3.1 Manager identity

Each manager has a stable Ed25519 identity.

The manager identity is used for:

- logical server authentication,
- signing controller attachment grants,
- stable manager fingerprinting,
- trust pinning,
- audit correlation.

It is independent of any TLS certificate so that TLS material can rotate without changing the Cubicle manager identity.

### 3.2 Client identity

Each client has an Ed25519 identity.

The client private key may be provided through:

- an automatically generated Cubicle key file,
- an explicit private-key file,
- `ssh-agent`,
- a hardware-backed signer,
- an OS key store in a later phase.

The private key is never transmitted.

### 3.3 Workspace authorization identity

Workspace ACLs authorize client key IDs, not Unix usernames.

A local Unix connection also has kernel-authenticated peer credentials:

```text
uid
gid
pid
```

These credentials are obtained from the Unix socket and may be bound into the session.

### 3.4 Execution identity

For a user-space manager, the manager, controllers, and managed processes run as the manager's Unix user.

For a system manager, the manager derives the process execution UID and GID from trusted local peer credentials and workspace ownership policy. A client RPC field cannot request an arbitrary UID or GID.

## 4. Manager operating modes

### 4.1 User-space manager

Typical paths:

```text
config:   $XDG_CONFIG_HOME/cubicle/config.cfg
state:    $XDG_STATE_HOME/cubicle
runtime:  $XDG_RUNTIME_DIR/cubicle
socket:   unix://$XDG_RUNTIME_DIR/cubicle/manager.sock
keys:     $XDG_STATE_HOME/cubicle/keys
```

If XDG variables are absent, normal XDG fallbacks apply.

Properties:

- no root privileges required,
- manager identity is generated in user-owned storage,
- accepted Unix peers normally must have the same UID as the manager,
- controllers and managed processes inherit the manager UID and GID,
- client session files are stored in the user's runtime directory.

### 4.2 System manager

Typical paths:

```text
config:   /etc/cubicle/config.cfg
state:    /var/lib/cubicle
runtime:  /run/cubicle
socket:   unix:///run/cubicle/manager.sock
keys:     /var/lib/cubicle/keys
```

Properties:

- manager may run as root or a constrained service account with required capabilities,
- Unix peer credentials are read using `SO_PEERCRED` or platform equivalent,
- workspace records include an owning Unix UID,
- controllers are launched after an irreversible credential drop,
- per-user state and runtime subdirectories are isolated by ownership and permissions.

## 5. Zero-setup startup

Running:

```console
cubicle-manager daemon
```

must work without prior key or certificate preparation.

On startup the manager must:

1. Load the effective configuration.
2. Resolve and securely create state, runtime, and key directories.
3. Load the manager identity if present.
4. Generate an Ed25519 manager identity atomically if absent.
5. Store the private key with mode `0600`.
6. Store the public key or fingerprint in a readable companion file.
7. Create the configured Unix listener.
8. If a TLS listener is enabled and no TLS material is configured, generate usable local TLS material automatically.
9. Log the stable Cubicle manager fingerprint.
10. Refuse to use insecure permissions on private-key files.

Concurrent first-start attempts must not create competing identities. Creation must use exclusive files or an equivalent atomic initialization mechanism.

## 6. Automatic client identity

When `cube` first needs a client identity and none is configured, it should automatically generate one.

Typical user paths:

```text
$XDG_CONFIG_HOME/cubicle/keys/client.key
$XDG_CONFIG_HOME/cubicle/keys/client.pub
```

Private-key permissions must be `0600`; the containing directory must be `0700`.

The CLI may print:

```text
Created Cubicle client identity: SHA256:<fingerprint>
```

Users may later configure an SSH-agent or external signer, but ordinary use must not require a separate key-generation command.

## 7. Initial authenticated handshake

A full handshake is performed when:

- no usable cached session exists,
- the cached session expired,
- the manager restarted and invalidated volatile sessions,
- the client changes identity,
- the manager identity changes,
- protocol negotiation changes materially,
- session resumption fails.

### 7.1 Logical exchange

```text
ClientHello
    supported protocol versions
    client nonce
    supported protocol capabilities
    client public key or key ID
    optional workspace reference

ServerHello
    selected protocol version
    manager nonce
    manager identity
    selected capabilities
    connection ID
    authentication challenge

ClientAuthenticate
    public key when required
    signature over deterministic transcript

ServerAuthenticated
    session ID
    session issue and expiry times
    client key ID
    manager ID
    selected capabilities
    session-resume material
```

### 7.2 Transcript binding

The signed transcript must bind at least:

```text
protocol major and minor
manager identity
client key ID or public key
client nonce
manager nonce
connection ID
negotiated capabilities
transport binding
workspace reference when session is workspace-scoped
Unix peer UID/GID/PID when applicable
```

The transcript must use a deterministic, versioned binary encoding. JSON text must not be signed directly.

Suggested domain separator:

```text
CUBICLE-AUTH-V0\0
```

All integers use fixed widths and network byte order. Variable fields are length-prefixed. Golden encoding and signature fixtures are mandatory.

### 7.3 Transport binding

For Unix sockets, bind the session to:

- manager identity,
- connection ID,
- peer UID and GID,
- manager boot/session generation.

For TLS, bind the Cubicle handshake to the exact TLS connection using a TLS exporter or equivalent channel-binding value.

## 8. Workspace sessions

The full handshake creates or refreshes a reusable authenticated workspace session.

A session record includes:

```text
session_id
manager_id
workspace_id
client_key_id
protocol version
negotiated capabilities
issued_at
expires_at
idle expiry
local UID binding when applicable
manager generation
resume secret or derived resume key
```

### 8.1 Client-side storage

Client session state is stored in a user runtime directory, for example:

```text
$XDG_RUNTIME_DIR/cubicle/sessions/<manager-id>/<workspace-id>.session
```

Requirements:

- parent directory mode `0700`,
- file mode `0600`,
- owned by the current user,
- deleted on explicit logout,
- ignored if ownership or permissions are unsafe,
- short-lived,
- never copied into persistent project directories.

### 8.2 Manager-side storage

The manager may initially keep active session secrets only in memory.

Consequences:

- manager restart invalidates resume credentials,
- clients transparently perform a new full handshake,
- no persistent bearer-secret database is required in v0.

Persistent resumable sessions may be considered later, but are not required initially.

### 8.3 Default lifetime

Recommended defaults:

```text
absolute lifetime: 12 hours
idle timeout:       2 hours
handshake timeout:  10 seconds
```

These values must be configurable.

## 9. Lean session resumption

After the first handshake, each new CLI process should resume the existing session without repeating Ed25519 signing and verification.

### 9.1 Resume exchange

```text
ClientResume
    session_id
    fresh client nonce
    resume authenticator

ServerResume
    accepted or rejected
    fresh server nonce
    connection ID
    selected session metadata
```

The authenticator should be a MAC derived from the session secret, for example conceptually:

```text
HMAC(resume_key,
     domain_separator ||
     manager_id ||
     session_id ||
     client_nonce ||
     connection_id ||
     transport_binding)
```

The exact MAC algorithm and encoding must be pinned in implementation specifications and fixtures.

### 9.2 Resume properties

- symmetric-key operation only,
- fresh nonce on every connection,
- bound to manager identity,
- bound to the current transport connection,
- bound to local UID for Unix sessions,
- rejected after key revocation,
- rejected after expiry,
- rejected after manager generation changes,
- automatic fallback to a full handshake.

### 9.3 CLI behavior

The user should not manually manage normal session renewal.

Commands such as:

```console
cube ps
cube --bg make
cube connect editor
cube stop build
```

must transparently:

1. locate the current manager/workspace session,
2. attempt session resume,
3. fall back to full authentication if necessary,
4. execute the requested operation.

## 10. Controller attachment authorization

Controller attachments do not repeat the manager handshake or query workspace ACLs directly.

Flow:

```text
cube connect process
    -> resume manager session
    -> request attachment grant
    -> receive short-lived signed grant
    -> connect directly to controller
    -> controller verifies grant
    -> attachment begins
```

### 10.1 Attachment grant fields

The signed grant includes:

```text
grant_id
manager_id
workspace_id
process_id
process generation
client_key_id
permitted channels
attachment mode
issued_at
expires_at
connection limit
nonce
controller endpoint identity
```

Suggested signature domain:

```text
CUBICLE-ATTACHMENT-GRANT-V0\0
```

The encoding must be deterministic and covered by golden fixtures.

### 10.2 Grant defaults

Recommended defaults:

```text
grant establishment lifetime: 30 seconds
active attachment lifetime:    until disconnect
```

Grant expiry prevents new establishment but does not automatically terminate an already established attachment.

### 10.3 Controller behavior

The controller verifies:

- manager signature,
- manager identity,
- process ID and generation,
- permitted channels,
- observer versus interactive mode,
- expiry,
- connection limit,
- nonce/replay status where applicable.

The controller does not maintain workspace ACLs.

## 11. Workspace access enrollment

Adding access should require one command and a public key.

Suggested commands:

```console
cube access add ~/.ssh/id_ed25519.pub
cube access add alice.pub --role observer
cube access add alice.pub --role operator --label "Alice laptop"
cube access add alice.pub --role owner
```

When no workspace is specified, the current workspace is used.

Explicit workspace form:

```console
cube access add --workspace Shogun alice.pub --role operator
```

Related commands:

```console
cube access list
cube access remove KEY_ID
cube access set-role KEY_ID observer
```

### 11.1 Roles

Initial convenience roles:

```text
observer
operator
owner
```

Roles map to explicit capability masks. Capabilities remain the authoritative authorization primitive.

Current v0 role mapping:

- `owner`: all workspace, process, event, and key-management capabilities.
- `operator`: workspace read/stop, process start/read/observe/input/signal/remove,
  and event read. Operators cannot manage workspace keys or delete workspaces.
- `observer`: workspace read, process read/observe, and event read.

### 11.2 Enrollment processing

The manager must:

1. Parse and validate the supplied public key.
2. Derive a stable key ID/fingerprint.
3. Check caller permission to manage workspace keys.
4. Add or update the ACL atomically.
5. Return the resulting key ID, label, role, and capabilities.
6. Record an audit event.

### 11.3 Revocation

Key revocation must immediately prevent:

- new full sessions,
- session resumption,
- new attachment grants.

Initial v0 behavior may leave existing process attachments active until disconnect. Immediate active-attachment revocation is optional for a later phase.

## 12. Workspace bootstrap

When a local user creates a workspace, the first owner key should be installed atomically.

Example:

```console
cube workspace Shogun
```

If the workspace does not exist:

1. Ensure the client has an identity, generating one if required.
2. Authenticate to the manager.
3. Create the workspace.
4. Set the workspace's local Unix owner where applicable.
5. Add the authenticated client key as workspace owner.
6. Create and cache the workspace session.

No separate enrollment command should be required for the creator.

Remote unauthenticated clients may not self-authorize. Remote enrollment requires an existing owner/administrator, a manager administrative path, or a future explicit invitation mechanism.

## 13. TLS and remote transport

Production remote transport uses TLS 1.3 plus the Cubicle client-key handshake inside the secured channel.

Recommended division of responsibility:

```text
TLS:
    confidentiality
    integrity
    server transport authentication
    forward secrecy

Cubicle handshake:
    stable manager identity
    client Ed25519 identity
    workspace authorization
    protocol negotiation
    session establishment
```

mTLS is not required initially because Cubicle already has its own client-key identity and ACL model.

### 13.1 Automatic TLS material

If a TLS listener is enabled without configured certificate paths, the manager automatically creates usable TLS key/certificate material.

No manual certificate-generation step is required.

Administrators may later configure externally managed certificates. Replacing TLS material must not change the stable Cubicle manager identity.

### 13.2 Trust modes

Supported trust policies may include:

- preconfigured manager identity pin,
- administrator-provisioned trust,
- interactive trust-on-first-use for developer/user-manager scenarios,
- CA validation plus manager identity verification.

Noninteractive clients must not silently trust an unknown manager.

### 13.3 Plain TCP

`tcp://` remains available only for tests or explicit insecure-development builds.

A production manager must not expose unauthenticated, unencrypted RPC over raw TCP.

## 14. Configuration schema additions

Illustrative configuration:

```ini
[manager.identity]
private_key=/var/lib/cubicle/keys/manager.key
public_key=/var/lib/cubicle/keys/manager.pub
generate_if_missing=true

[manager.auth]
handshake_timeout_seconds=10
session_max_seconds=43200
session_idle_seconds=7200
allow_unregistered_local_keys=false

[manager.tls]
enabled=false
listen=cubicle+tls://0.0.0.0:7443
certificate=
private_key=
generate_if_missing=true
minimum_version=1.3

[client.identity]
private_key=${XDG_CONFIG_HOME}/cubicle/keys/client.key
public_key=${XDG_CONFIG_HOME}/cubicle/keys/client.pub
generate_if_missing=true
use_ssh_agent=false

[client.session]
runtime_dir=${XDG_RUNTIME_DIR}/cubicle/sessions

[client.trust]
manager_identity=
ca_file=
trust_on_first_use=false
```

The effective path defaults differ between user-space and system-manager operation. The configuration layer resolves XDG and system paths rather than relying on arbitrary shell expansion inside config values.

## 15. Session and identity CLI

Normal operation is automatic, but diagnostic and administrative commands should exist.

```console
cube identity show
cube identity fingerprint
cube session show
cube session list
cube session refresh
cube session logout
```

`cube session logout` removes cached session state for the current workspace. It does not revoke the underlying public key.

## 16. Audit requirements

The manager records security-relevant events including:

- successful and failed full authentication,
- successful and failed session resume,
- manager identity creation,
- client key enrollment,
- key role/capability changes,
- key revocation,
- session creation and invalidation,
- attachment grant creation,
- rejected expired or invalid grants,
- workspace bootstrap ownership.

Audit records must not contain private keys, resume secrets, raw session secrets, or reusable authentication material.

## 17. Security invariants

1. Private keys are never transmitted.
2. Session IDs alone are not bearer credentials.
3. Resume authenticators include fresh nonces and transport binding.
4. Local sessions are bound to Unix peer credentials where supported.
5. Manager identity changes invalidate cached trust/session state unless explicitly reapproved.
6. Revoked keys cannot resume sessions.
7. Controllers authorize attachments only through manager-signed grants.
8. Signed structures use deterministic binary encodings, not raw JSON text.
9. No RPC parameter may select an arbitrary Unix execution identity.
10. Raw TCP is not a production security mode.

## 18. Phased implementation

### Phase 1: Automatic local identities

Implement:

- manager Ed25519 identity generation/loading,
- automatic user-space paths,
- automatic system paths,
- automatic client key generation,
- strict private-key ownership and permission checks,
- manager fingerprint reporting,
- identity unit tests.

Acceptance criteria:

- `cubicle-manager daemon` starts from an empty writable configuration/state location,
- no key-generation command is required,
- restarting preserves manager identity,
- unsafe key permissions are rejected.

### Phase 2: Unix full authentication

Implement:

- deterministic handshake transcript encoding,
- golden transcript/signature fixtures,
- Unix socket peer credential collection,
- manager proof and client Ed25519 authentication,
- protocol/capability negotiation,
- authenticated connection/session creation,
- user-space same-UID policy,
- system-manager peer identity binding.

Acceptance criteria:

- all Unix manager RPCs require authentication,
- forged and replayed handshakes fail,
- manager and client identities are mutually bound to the connection,
- fixtures are stable across implementations.

### Phase 3: Workspace bootstrap and key access commands

Implement:

- atomic owner-key installation during workspace creation,
- `cube access add`, `list`, `remove`, and `set-role`,
- public-key parsing and fingerprinting,
- role-to-capability mappings,
- revocation checks.

Deferred:

- audit records,
- active session invalidation for revoked keys.

Acceptance criteria:

- a user can create a workspace with one command,
- another user can be authorized with one public-key command,
- revoked keys cannot start new sessions.

### Phase 4: Workspace session caching

Implement:

- client runtime session files,
- permission and ownership checks,
- configurable lifetime and idle timeout,
- manager in-memory session records,
- automatic session lookup,
- explicit logout/refresh diagnostics.

Acceptance criteria:

- only the first workspace connection performs a full public-key handshake,
- session data is inaccessible to other local users,
- manager restart safely causes transparent full reauthentication.

### Phase 5: Lean session resumption

Implement:

- resume-key derivation,
- deterministic resume-authenticator encoding,
- fresh nonces and connection IDs,
- transport binding,
- UID binding for Unix sockets,
- transparent fallback to full authentication,
- replay and expiry tests.

Acceptance criteria:

- normal `cube` commands avoid public-key signing and verification,
- copied or replayed resume messages fail,
- revocation blocks resume immediately,
- resume latency is negligible relative to the RPC itself.

### Phase 6: Signed controller attachment grants

Implement:

- deterministic grant encoding,
- manager grant signing,
- controller grant verification,
- short establishment expiry,
- process-generation binding,
- channel and mode restrictions,
- direct attachment path,
- golden grant fixtures.

Acceptance criteria:

- controllers do not query workspace ACLs,
- attachment setup requires only grant verification,
- grants cannot be reused for another process or generation,
- observer grants cannot send input.

### Phase 7: Automatic TLS transport

Implement:

- TLS 1.3 client and server transport,
- automatic TLS key/certificate generation when enabled,
- administrator-supplied certificate overrides,
- TLS exporter/channel binding in Cubicle authentication,
- manager identity pinning and trust policy,
- explicit insecure test-only raw TCP mode.

Acceptance criteria:

- enabling a TLS listener requires no manual certificate-generation step,
- remote traffic is encrypted and authenticated,
- Cubicle client authentication is bound to the exact TLS connection,
- raw TCP is disabled in normal production builds/configuration.

### Phase 8: Signer and trust enhancements

Implement:

- SSH-agent signer,
- hardware/OS signer abstraction support,
- interactive TOFU for user-manager/development use,
- noninteractive strict trust behavior,
- key labels and identity selection,
- trust diagnostics.

### Phase 9: Advanced revocation and lifecycle

Implement as needed:

- immediate active-session invalidation notifications,
- optional active-attachment revocation,
- persistent resume sessions if justified,
- enrollment invitations,
- key rotation workflows,
- manager identity migration/recovery tooling,
- TLS certificate rotation monitoring.

## 19. Testing requirements

Mandatory tests include:

- deterministic transcript fixtures,
- deterministic grant fixtures,
- signature success/failure vectors,
- nonce reuse rejection,
- wrong-manager rejection,
- wrong-workspace rejection,
- wrong-UID session resume rejection,
- expired session and grant rejection,
- key revocation behavior,
- manager restart fallback,
- user-space first-run identity creation,
- system-mode peer credential binding,
- unsafe file-permission rejection,
- malformed key handling,
- TLS channel-binding mismatch rejection,
- fuzzing of handshake and grant decoders,
- ASan/UBSan CI for auth parsers and encoders.

## 20. Initial implementation boundary

The first secure milestone consists of Phases 1 through 5 over Unix sockets:

```text
automatic identities
full Ed25519 handshake
workspace key authorization
session caching
cheap session resumption
```

Remote TLS and direct signed controller grants may follow after these local conformance tests are stable.

The core user experience must remain:

```console
cubicle-manager daemon
cube workspace Shogun
cube access add alice.pub --role operator
cube --bg make
cube ps
```

No separate key-generation ceremony is required, and only the initial workspace connection performs the full authentication handshake.
