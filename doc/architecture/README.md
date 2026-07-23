---
id: architecture.index
type: index
status: current
domain: documentation
summary: Maps architecture roles, relationships, capability coverage, ownership, and dependency direction across Aobus.
---
# Architecture landscape

Architecture describes how Aobus is divided and how its parts compose.
It owns dependency direction, state ownership, executor and lifetime boundaries, system-wide data flow, and cross-cutting models such as errors, persistence, and cancellation.

Architecture does not own exhaustive schemas, command lists, or subsystem state-machine details.
Those belong in reference and specifications.

This landscape is the portfolio-level owner for how the current architecture set is assigned to roles, related, and checked for coverage.
The [system architecture](system-overview.md) separately owns the code-layer model and composition roots.

## Reading order

Read the architecture set in this order when learning the whole system:

1. [System architecture](system-overview.md) establishes the layer map, composition roots, and dependency direction.
2. [Runtime execution architecture](runtime-execution.md) establishes executor affinity, worker execution, dedicated threads, cancellation ownership, and teardown order.
3. [Failure and reporting architecture](failure-and-reporting.md) establishes outcome channels, recovery ownership, runtime reporting, and application-leaf presentation.
4. Continue with the domain or application architecture that owns the change being made.
5. Read a vertical-slice architecture only when the change crosses the complete path that slice owns.

## Portfolio roles

The [documentation system](../README.md#architecture-portfolio) defines the available roles and the threshold for creating a focused architecture.
Each current architecture document has one navigation role assigned here.
The roles do not create additional document types or directories, and a role does not override the authority declared by an individual document's scope.

### System map

| Document | Owns |
|---|---|
| [System architecture](system-overview.md) | Top-level layers, composition roots, dependency direction, and major system flows. |

### Foundation and cross-cutting concerns

These documents constrain several systems without becoming their behavioral owner.

| Document | Owns |
|---|---|
| [Runtime execution architecture](runtime-execution.md) | Callback affinity, worker and dedicated-thread execution, cancellation ownership, and teardown order. |
| [Failure and reporting architecture](failure-and-reporting.md) | Failure channels, boundary translation, recovery ownership, notifications, diagnostics, and application-leaf reporting. |
| [Persistence and managed-state architecture](persistence-and-managed-state.md) | Durable truth, managed state, schemas, stores, paths, save/restore flow, and lifecycle boundaries. |

### Domain systems

These documents own reusable product or core capabilities with independent state and dependency graphs.

| Document | Owns |
|---|---|
| [Encoded media architecture](encoded-media.md) | Encoded-file reading, reusable container primitives, consumer dependency direction, and zero-copy view lifetimes. |
| [Library architecture](library.md) | Storage/runtime access roles, revisioned changes, tasks, sources, projections, and their ownership graph. |
| [Track expression architecture](track-expression.md) | Expression authoring, core compilation, source membership, completion composition, and scalar formatting boundaries. |
| [Playback architecture](playback.md) | Succession, transport, session persistence, audio execution, output, and their cross-domain protocols. |

### Application systems

These documents own interactive composition or shared application policy above the domain systems.

| Document | Owns |
|---|---|
| [Workspace architecture](workspace.md) | Runtime view identity, source and projection ownership, open/focused workspace state, navigation, and semantic sessions. |
| [Interactive session lifecycle architecture](interactive-session-lifecycle.md) | Interactive runtime construction, restoration, checkpointing, active-library replacement, and teardown. |
| [Application shell architecture](application-shell.md) | Declarative shell documents, catalogs, GTK construction, actions, component state, editing, shortcuts, and teardown. |
| [Presentation architecture](presentation.md) | Runtime, UIModel, GTK, TUI, and CLI presentation responsibilities. |

### End-to-end vertical slices

A vertical slice exists only when a focused concern has its own evidence, translation, or lifetime model across several layers.
It refines its broader domain and application owners instead of becoming a peer replacement for them.

| Document | Broader owners | Owns |
|---|---|---|
| [Audio quality architecture](audio-quality.md) | [Playback](playback.md) and [presentation](presentation.md) | Quality evidence, graph composition, runtime publication, shared UIModel interpretation, and frontend adaptation. |
| [Resource delivery architecture](resource-delivery.md) | [Encoded media](encoded-media.md), [library](library.md), [playback](playback.md), and [presentation](presentation.md) | Immutable resource identity, runtime materialization, projections, frontend transforms, caches, stale-result suppression, and external artifacts. |

## Architecture relationships

This table records primary structural relationships rather than every document link.
“Consumes or refines” identifies the authority whose values or constraints enter the document; “feeds or constrains” identifies the principal downstream architecture consumers.

| Architecture | Consumes or refines | Feeds or constrains |
|---|---|---|
| [System](system-overview.md) | Build graph and composition roots | Every focused architecture |
| [Runtime execution](runtime-execution.md) | System layer and lifetime model | Library tasks and projections, playback, workspace lifecycle, persistence scheduling, and failure delivery |
| [Failure and reporting](failure-and-reporting.md) | Failures from every subsystem plus runtime execution boundaries | Recovery owners, runtime notifications, UIModel status, frontend and CLI reporting |
| [Persistence and managed state](persistence-and-managed-state.md) | Filesystem, LMDB, YAML, and state-owner boundaries | Library durability, workspace and playback sessions, application and UI-local state |
| [Encoded media](encoded-media.md) | Encoded bytes and filesystem mappings | Library ingestion and identity, audio decoding, resource persistence, and their lifetime boundaries |
| [Library](library.md) | Core storage, query, and async mechanisms | Track expressions, workspace views, playback resolution, and presentation projections |
| [Track expression](track-expression.md) | Query language and library values | Smart membership, filtering, completion, scalar formatting, presentation, and CLI output |
| [Playback](playback.md) | Library identities and sources, runtime execution, managed state | Audio quality, presentation, and platform output adapters |
| [Workspace](workspace.md) | Library sources, track expressions, presentation values, runtime execution, and managed-state boundaries | Interactive session lifecycle, application-level reveal composition, and presentation consumers |
| [Interactive session lifecycle](interactive-session-lifecycle.md) | Runtime execution, persistence, library, workspace, playback, and failure authorities | Presentation and frontend composition roots |
| [Application shell](application-shell.md) | Presentation boundaries, UIModel layout values, runtime services, managed state, and GTK lifecycle | GTK widget tree, actions, editor, component state, and shortcuts |
| [Presentation](presentation.md) | Runtime snapshots and commands from domain and application systems | GTK, TUI, and non-interactive CLI adaptation |
| [Audio quality](audio-quality.md) | Playback route evidence and execution generations | Runtime quality state, shared presentation policy, GTK, and TUI |
| [Resource delivery](resource-delivery.md) | Media cover evidence, library blobs and references, playback/projection identities | GTK images, TUI artwork, MPRIS art URLs, and CLI export |

## Capability coverage

Coverage means that one current architecture owns the structural question and delegates behavior and exact surfaces to specifications and reference.
“Partial” identifies a documentation ownership gap; it does not assert that the production design is incomplete or authorize a new architecture document by itself.
The table tracks capability families with architecture-bearing boundaries, not every product feature.

| Capability | Current structural owner | Coverage | Remaining documentation question |
|---|---|---|---|
| Layering and composition roots | [System](system-overview.md) | Current | Focused architectures must continue to refine rather than redefine the layer map. |
| Execution, cancellation, and teardown | [Runtime execution](runtime-execution.md) | Current | Subsystems still own their more specific execution and lifetime refinements. |
| Failure propagation and reporting | [Failure and reporting](failure-and-reporting.md) | Current | Media, YAML, storage, and audio failure behavior remains delegated to the media, persistence, storage, and playback specification owners. |
| Durable library data and managed state | [Persistence and managed state](persistence-and-managed-state.md) | Current | Exact schemas, paths, and state-owner behavior remain delegated to reference and specifications. |
| Library storage, tasks, live sources, and projections | [Library](library.md) | Current | Scan, transfer, mutation, source, and projection behavior is split into library specifications. |
| Encoded-media reading and container reuse | [Encoded media](encoded-media.md) | Current | Encoded media owns `ao_media`, its zero-copy lifetime model, and its library/audio consumer directions; exact reader behavior and surfaces belong to specification and reference. |
| Smart Lists, filtering, completion, and scalar formatting | [Track expression](track-expression.md) | Current | Presentation remains a separate owner for track-list shape and rendering. |
| Interactive playback and platform audio output | [Playback](playback.md) | Current | Session persistence and audio-execution behavior remains delegated to the playback specification and reference owners. |
| Audio-quality evidence and presentation | [Audio quality](audio-quality.md) | Current | The slice remains subordinate to playback and presentation ownership. |
| Resource and cover-art delivery | [Resource delivery](resource-delivery.md) | Current | Interactive reads are bounded and asynchronous; GTK, TUI, and MPRIS own worker transforms, cancellation, and stale-result suppression. |
| Workspace views, navigation, and semantic sessions | [Workspace](workspace.md) | Current | Exact navigation and restore behavior remains delegated to workspace specifications. |
| Interactive startup, checkpointing, switching, and shutdown | [Interactive session lifecycle](interactive-session-lifecycle.md) | Current | GTK uses prepare-before-release replacement; GTK and TUI intentionally retain separate lifecycle owners. |
| Application shell, layout document, actions, component state, and widget construction | [Application shell](application-shell.md) | Current | Current declarative construction is GTK-specific; TUI retains an independent terminal shell. |
| Shared presentation policy and frontend adaptation | [Presentation](presentation.md) | Current | Exact UI behavior remains delegated to the presentation, shell, and frontend specification and reference owners. |
| Metadata ingestion and editing from frontend intent through library publication | [Presentation](presentation.md) | Current | Presentation owns edit-session and frontend adaptation structure and delegates revision-bound admission, commit, and publication to the library architecture. |
| Platform services such as portals and MPRIS | [Presentation](presentation.md) owns the adapter edge | Partial | Native file-dialog completions now have a coordinator-scoped lifetime boundary; remaining portal and MPRIS contracts still require focused migration. |

## Coverage workflow

Use this order when extending the architecture set:

1. Record the capability and current endpoint owners in the coverage table.
2. Trace its code, tests, legacy documents, and cross-boundary lifetime.
3. Keep it under an existing owner when that owner can explain the structure without absorbing unrelated responsibilities.
4. Create a focused architecture only when the threshold in the [documentation system](../README.md#architecture-portfolio) is met.
5. Update this landscape's role, relationship, and coverage tables in the same change.

The current follow-up order is the remaining exact platform-service contracts.
An audit may conclude that specifications and reference under an existing architecture are sufficient.

Architecture coverage is complete for a capability when:

- one current architecture owns its structural question;
- upstream authorities, downstream consumers, forbidden dependencies, and lifetime boundaries are explicit;
- stable implementation and test maps support those claims;
- observable behavior and exact surfaces are delegated to specification and reference owners; and
- this landscape exposes the owner and its primary relationships.

## Working with this area

Use the [architecture template](../template/architecture.md) for a new architecture concern.
Do not create a subsystem architecture document merely because a subsystem exists or a feature crosses layers.
Apply the focused-architecture threshold and coverage workflow before creating a new owner.

Observable behavior belongs in [specifications](../spec/README.md), and exhaustive surfaces belong in [reference](../reference/README.md).
Architecture links those owners instead of restating their behavior or field tables.

## Authority boundary

The documents above own current architecture facts.
Observable behavior is delegated to the [specification index](../spec/README.md), and exact surfaces are delegated to the [reference index](../reference/README.md).
Contributor workflow and repository policy belong to [development documentation](../development/README.md).
