# ADR 0001 — Future third-party modules

**Status** Proposed for future work. The built-in overlay panel experiment
and the media broker proof are internal.

## Boundary

Use an out-of-process adapter for future integrations. The initial UI is a
host-rendered template populated by bounded data. GameHQ retains window
focus, modal priority, controller routing and all capture operations. A
module cannot register global input listeners, call internal objects, access
the database or configuration manager, or load its own QML or DLL into GameHQ.

Make three guarantees explicit:

| Layer | Guarantee and limit |
| --- | --- |
| GameHQ authorization | The host checks the identity it assigned to the connection, the user's grant, and the resource's scope for every request. A message cannot confer its own authority. |
| Process isolation | A helper crash or hung request can be handled without taking down the host. Bound its connections, work and lifetime. |
| OS isolation | A same-user pipe limits which account can connect. A same-user helper still has its ordinary OS privileges, including access the user account already has. Require a verified OS isolation design before allowing untrusted code. |

A malformed or compromised helper must be unable to select another module's
resources through the protocol. A module's same-user access to the host
process, profile files or network is a separate threat that the protocol
cannot prevent. Treat signatures as provenance and review inputs. Execution
isolation requires its own controls. Defer the OS sandbox choice, package
signing, distribution, SDK and marketplace.

## Grants and resources

Start with an empty grant set. Show each permission to the user separately
and require approval before use. Bind grants to the approved module identity
and known protocol version. A change in identity or requested permissions
requires review. Revocation denies new requests and cancels pending work
where possible. It cannot retract an already delivered external action.

| Example capability | Resource scope |
| --- | --- |
| `media.read` | Bounded metadata for a user-selected session. The host resolves the session. |
| `media.play` | Play the selected session only if its advertised capability still permits it. Keep transport grants distinct from metadata. |
| `capture.read` | Read one explicitly selected capture through an expiring, job-scoped host grant. The request carries an opaque resource ID. The host chooses a bounded transfer with no write access or directory enumeration. |

Settings and action IDs live under the module's assigned namespace. Host and
other modules' keys are inaccessible through grants. Capture and media
handles expire on revocation, disconnect, session replacement or job
completion. Avoid payloads that require permanent capture paths, media
histories or access tokens in module logs.

## Protocol and lifecycle

Negotiate a major and minor protocol version at connection setup. Reject
unsupported major versions and grant-dependent features that the negotiated
minor version lacks. Within a negotiated shape, reject unknown fields.
Additions require an explicit compatibility entry and conformance cases for
old and new peers.

The example request grammar below is a design fixture. `v` is the negotiated
major version, `id` is a request ID unique within the connection, `method`
maps to one capability, and `target` is an opaque host-issued resource ID.
IDs are 1–64 ASCII characters drawn from letters, digits, periods, underscores
and hyphens. The example frame limit is 4,096 UTF-8 bytes.

```json
{"v":1,"id":"r-12","method":"media.play","target":"session-7"}
```

The host binds each reply to the requesting connection and request ID.
Reject reuse of an outstanding ID on that connection. Apply byte and queue
limits before dispatch, enforce operation deadlines, and return a bounded
error code for denial, failure, cancellation or expiry. A timeout must never
turn into an implicit retry of a command with external effects.

Scope subscriptions to the active feature that needs them. On deactivation,
disconnect, crash or shutdown, revoke handles and subscriptions and discard
late replies. Use host-owned cancellation and a bounded stop deadline. The
prototype fixture in `tools/testing/test_module_contract.py` checks only the
request grammar. Capabilities, runtime isolation and version negotiation
need separate implementation and acceptance before extension launch.

## Existing integrations

Preserve Playnite's documented IPC protocol and the separate external Share
provider API. The provider registry handles Share providers. It has no
authority to add overlay panels or grant media controls. Both existing
protocols remain on their current versions and conformance paths. External
Share v1 keeps its documented handling of unknown fields.
[Playnite IPC](../integration-protocol.md), [External Share](../share-provider-api-v1.md)

Keep the existing GPL core and MIT integration license notices intact.
Review the licensing of any future SDK, example or distribution artifact
before exposing it externally. [Core license](../../LICENSE), [Protocol license](../integration-protocol.LICENSE), [Playnite license](../../integrations/playnite/LICENSE)
