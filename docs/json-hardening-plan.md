# JSON Protocol Hardening Plan

Status: Proposed implementation plan  
Date: 2026-08-01

This document defines the work required to make Cubicle's JSON and RPC handling suitable for authenticated local and remote use.

## Goals

The JSON layer must:

- parse standards-compliant JSON correctly,
- reject ambiguous or malformed protocol messages,
- enforce strict resource limits,
- decode into typed Cubicle objects,
- support protocol versioning and forward compatibility,
- avoid security decisions based on textual searches,
- provide deterministic encoding for signed data,
- remain fuzzable and thoroughly testable.

The core rule is:

> Raw JSON bytes are parsed once. Manager, controller, and client operations consume validated typed values and never inspect raw JSON text directly.

## 1. Replace textual field searches with a real parser

Remove protocol-facing parsing based on `strstr()`, quote scanning, and direct `strto*()` calls over arbitrary JSON text.

Use a pinned JSON parser with strict read and write support. `yyjson` is the preferred candidate because it is small, fast, strict, and provides both parser and writer APIs. Vendoring a reviewed version is acceptable to keep behavior consistent across platforms.

Target flow:

```text
framed bytes
  -> strict JSON parse
  -> parsed document/tree
  -> RPC envelope validation
  -> method-specific typed decoder
  -> typed C request or response
```

## 2. Separate syntax parsing from Cubicle validation

Valid JSON is not automatically a valid Cubicle message.

Provide distinct layers:

```c
cubicle_json_parse(...);
cubicle_rpc_decode_request(...);
cubicle_rpc_validate_request(...);
```

Every method decoder must check:

- required fields,
- field types,
- string lengths,
- integer ranges,
- enum values,
- nullability,
- array sizes,
- nesting rules,
- semantic relationships between fields.

For example, `process.start` must reject empty workspace IDs, empty `argv`, invalid modes, negative terminal dimensions, or environment arrays above configured limits.

## 3. Add common typed decoder helpers

All endpoint implementations should use one set of checked accessors:

```c
cubicle_json_get_required_string(...);
cubicle_json_get_optional_string(...);
cubicle_json_get_required_u64(...);
cubicle_json_get_optional_bool(...);
cubicle_json_get_required_object(...);
cubicle_json_get_required_array(...);
cubicle_json_get_enum(...);
```

Method-specific decoding should operate on parsed values:

```c
cubicle_error_code_t cubicle_decode_process_start(
    const cubicle_json_value_t *params,
    cubicle_process_start_request_t *request,
    cubicle_error_t *error);
```

Validation failures should consistently report the field path and expected type.

## 4. Reject duplicate keys

Duplicate object members are ambiguous because parsers differ on first-value versus last-value behavior.

Cubicle protocol messages must reject duplicate keys at every object level, especially for:

- `request_id`,
- `session_id`,
- `method`,
- `ok`,
- workspace and process IDs,
- ACL capabilities,
- grant expiry and attachment mode.

Example to reject:

```json
{"workspace_id":"allowed","workspace_id":"forbidden"}
```

## 5. Define unknown-field behavior

The protocol needs explicit forward-compatibility rules.

Recommended policy:

- top-level RPC envelopes reject unknown fields unless placed under an extension namespace,
- ordinary method parameter objects ignore unknown optional fields from newer minor versions,
- security-sensitive objects reject unknown fields,
- tests support a strict mode that rejects all unexpected fields.

Security-sensitive objects include authentication messages, signed transcripts, ACL mutations, capability negotiation, and attachment grants.

Suggested extension shape:

```json
{
  "extensions": {
    "vendor.example.feature": {}
  }
}
```

## 6. Fully support JSON strings and Unicode

The parser must correctly support:

- escaped quotes and backslashes,
- all JSON control escapes,
- `\uXXXX`,
- surrogate pairs,
- valid UTF-8,
- non-ASCII workspace and process names,
- rejection of malformed UTF-8 and isolated surrogates,
- rejection of unescaped control characters.

Human-readable fields may contain Unicode:

- workspace names,
- friendly process names,
- key labels,
- errors and event messages.

Protocol identities must remain restricted ASCII:

- IDs,
- request IDs,
- method names,
- capability names,
- enum names.

No Unicode normalization should be applied to opaque security identities.

## 7. Enforce exact integer handling

Protocol values such as timestamps, stream offsets, event sequences, deadlines, and counts require exact integer handling.

Unsigned fields must reject:

- negative values,
- decimal fractions,
- exponents where exact-integer syntax is required,
- overflow above `UINT64_MAX`.

Signed fields must be range-checked before conversion.

Do not decode important integer fields through `double`.

Examples to reject for an unsigned offset:

```json
{"stdout_offset":1.5}
{"stdout_offset":1e20}
{"stdout_offset":-1}
{"stdout_offset":18446744073709551616}
```

## 8. Apply central resource limits

All parsing must run under hard limits. Initial proposed maxima:

```c
#define CUBICLE_JSON_MAX_DOCUMENT_BYTES       (16U * 1024U * 1024U)
#define CUBICLE_JSON_MAX_DEPTH                32
#define CUBICLE_JSON_MAX_OBJECT_MEMBERS       256
#define CUBICLE_JSON_MAX_ARRAY_ELEMENTS       4096
#define CUBICLE_JSON_MAX_STRING_BYTES         (1024U * 1024U)
#define CUBICLE_JSON_MAX_METHOD_BYTES         128
#define CUBICLE_JSON_MAX_ERROR_MESSAGE_BYTES  4096
#define CUBICLE_JSON_MAX_ARGC                 4096
#define CUBICLE_JSON_MAX_ENV_COUNT             4096
```

Limits defend against memory exhaustion, pathological nesting, oversized strings and arrays, and excessive parser work. Deployments may configure lower limits, while global safety ceilings remain fixed.

## 9. Replace formatted JSON construction with a structured writer

Avoid constructing protocol messages with `snprintf()` and manually escaped fragments.

Provide a checked writer interface or wrap the selected JSON library writer:

```c
cubicle_json_writer_begin_object(...);
cubicle_json_writer_string(...);
cubicle_json_writer_u64(...);
cubicle_json_writer_bool(...);
cubicle_json_writer_begin_array(...);
cubicle_json_writer_array_string(...);
cubicle_json_writer_end_array(...);
cubicle_json_writer_end_object(...);
```

The writer must guarantee:

- valid escaping,
- correct comma and nesting handling,
- overflow and allocation checks,
- valid UTF-8 where required,
- no accidental duplicate fields.

## 10. Do not sign arbitrary JSON text

RPC messages may use ordinary JSON, but authentication transcripts and attachment grants require deterministic bytes.

Equivalent JSON documents can differ in whitespace, member ordering, escaping, and number representation. Therefore signed data should use one of:

1. a fixed deterministic binary encoding,
2. a documented canonical JSON encoding,
3. a fixed binary transcript assembled field by field.

Preferred approach:

> Keep JSON for RPC, but use deterministic binary structures for authentication transcripts and signed attachment grants.

If signed grants remain JSON, the specification must define key ordering, whitespace, UTF-8 encoding, number formatting, escaping, and absent-versus-null semantics.

## 11. Parse the RPC envelope once

Introduce typed envelope structures:

```c
typedef struct cubicle_rpc_response {
    char request_id[CUBICLE_ID_STRING_LENGTH];
    uint32_t protocol_major;
    uint32_t protocol_minor;
    bool ok;
    cubicle_json_value_t result;
    cubicle_error_t error;
} cubicle_rpc_response_t;
```

Processing flow:

```text
parse document
  -> decode envelope
  -> verify protocol version
  -> verify request ID
  -> select result or error object
  -> invoke method-specific decoder
```

Method decoders must not search the complete JSON document for commonly repeated fields such as `id`, `state`, or `message`.

## 12. Strictly validate request-response correlation

Clients must reject responses when:

- `request_id` is missing or the wrong type,
- `request_id` does not match the outstanding request,
- both result and error are present,
- neither result nor error is present,
- `ok=true` carries an error,
- `ok=false` carries a result,
- a duplicate response is received.

This logic belongs in one shared RPC-envelope validator.

## 13. Add schemas for every method

Define the expected fields for every request and response. Initially this may be a table-driven C schema:

```c
static const cubicle_json_field_schema_t process_start_fields[] = {
    {"workspace_id", CUBICLE_JSON_STRING, true, 1, 32},
    {"friendly_name", CUBICLE_JSON_STRING, false, 0, CUBICLE_NAME_MAX - 1},
    {"mode", CUBICLE_JSON_STRING, true, 1, 64},
    {"argv", CUBICLE_JSON_ARRAY, true, 1, CUBICLE_ARGC_MAX},
    {"env", CUBICLE_JSON_ARRAY, false, 0, CUBICLE_ENV_MAX},
};
```

Also add machine-readable JSON Schema files:

```text
schemas/v0/
  rpc-request.schema.json
  rpc-response.schema.json
  workspace-create.schema.json
  process-start.schema.json
  attachment-grant.schema.json
  event.schema.json
```

These schemas should drive documentation, mock-server validation, fixtures, compatibility tests, and fuzz seed generation.

## 14. Centralize enum conversions

Provide one conversion implementation per protocol enum:

```c
cubicle_error_code_t cubicle_process_mode_from_json(...);
const char *cubicle_process_mode_json_name(...);
```

Apply this pattern to process state, event type, attachment mode, stream kind, error code, route type, and capabilities.

Unknown enum values must fail unless the protocol explicitly marks the field as forward-compatible.

## 15. Define null semantics

For each optional field, specify absent and null behavior.

Recommended rule:

- absent means optional/default behavior,
- null is invalid unless the operation explicitly permits clearing a field.

For example, `workspace.rename` must reject a null `new_name` rather than silently treating it as empty.

## 16. Improve validation errors

Parsing and validation failures should report safe structured details:

```c
typedef struct cubicle_validation_error {
    cubicle_error_code_t code;
    char field_path[256];
    char expected[64];
    char message[256];
} cubicle_validation_error_t;
```

Example:

```text
field=params.argv[3]
expected=string
observed=object
```

Parser offsets may be logged locally, but remote responses must not expose memory contents or internal implementation details.

## 17. Version-aware decoding

Every envelope decoder must know the negotiated protocol major and minor versions.

Rules:

- major mismatch is rejected,
- compatible older minor versions are accepted,
- unknown optional fields from newer minors may be ignored,
- unsupported required capabilities cause rejection.

Method decoders may gate new optional fields by minor version while avoiding complete duplicated decoder implementations.

## 18. Fuzz testing

Add dedicated fuzz targets:

```text
fuzz_json_parser
fuzz_rpc_envelope
fuzz_rpc_error
fuzz_process_start_request
fuzz_process_info_response
fuzz_attachment_grant
fuzz_event
```

Run them with libFuzzer and sanitizers. Each target must guarantee:

- no crashes,
- no out-of-bounds access,
- no leaks,
- bounded processing time,
- stable error returns,
- round-trip invariants where applicable.

AFL++ may be added later for complementary coverage.

## 19. Malicious and boundary fixtures

Add mandatory fixtures for:

### Syntax errors

- truncated documents,
- trailing garbage,
- invalid escapes,
- malformed UTF-8,
- excessive nesting,
- unterminated strings,
- invalid numbers.

### Structural errors

- duplicate keys,
- missing required fields,
- wrong types,
- unknown required capabilities,
- mismatched request IDs,
- result and error conflicts.

### Boundaries

- maximum-length names,
- one byte beyond limits,
- `UINT64_MAX`,
- integer overflow,
- maximum argument and environment counts,
- zero-length and maximum-size frames.

### Adversarial nesting and string confusion

Messages must correctly distinguish top-level fields from nested or quoted lookalikes.

## 20. Round-trip and property tests

For every typed protocol object:

```text
typed C object
  -> encode JSON
  -> parse JSON
  -> decode typed object
  -> compare with original
```

Properties should verify:

- encoding always produces valid JSON,
- `decode(encode(x)) == x`,
- field ordering and whitespace do not affect decoding,
- unknown optional fields preserve known values,
- invalid enums always fail,
- duplicate keys always fail.

## 21. Proposed source organization

```text
src/json/
  parser.c
  writer.c
  value.c
  schema.c
  unicode.c

src/rpc/
  envelope.c
  errors.c
  request.c
  response.c

src/protocol/
  workspace_decode.c
  workspace_encode.c
  process_decode.c
  process_encode.c
  attachment_decode.c
  attachment_encode.c
  events_decode.c
  events_encode.c
```

The selected JSON library remains private. Public `libcubicle` callers only see typed Cubicle APIs.

## 22. Implementation sequence

1. Select and pin the JSON parser/writer.
2. Add parser limits and duplicate-key rejection.
3. Introduce parsed document/value wrappers.
4. Replace RPC envelope parsing.
5. Replace structured error parsing.
6. Add checked primitive accessors.
7. Convert method decoders one API group at a time.
8. Remove all protocol-facing textual searches.
9. Introduce deterministic signed-structure encoding.
10. Add schemas and malicious fixtures.
11. Add property tests, ASan, UBSan, and fuzz targets.
12. Make malformed-message and sanitizer tests mandatory in CI.

## Acceptance criteria

The hardening work is complete when:

- no protocol-facing code uses textual JSON field searches,
- every request and response has a typed decoder,
- duplicate keys and malformed Unicode are rejected,
- exact integer bounds and resource limits are enforced,
- signed structures have deterministic byte encoding,
- all RPC envelopes verify request correlation,
- malicious fixtures pass under ASan and UBSan,
- fuzz targets run without reproducible crashes,
- mock and real manager implementations pass the same schema and conformance tests.
