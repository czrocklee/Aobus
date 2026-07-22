---
id: rfc.index
type: index
status: current
domain: documentation
summary: Defines and indexes Aobus proposals that still require a decision or implementation.
---
# Requests for comments

RFCs are reviewable proposals for consequential changes.
They own future work only while that work remains undecided or unimplemented and never override current architecture, specifications, or reference.

Delete an implemented or rejected RFC after moving current facts to their authoritative documents.
Move historical rationale to a decision only when that rationale remains useful; do not retain completed RFCs as a second architecture archive.

File names use a four-digit sequence and a concise noun phrase.
Execution details belong in the local ignored `doc/plan/` tree.
Use the [RFC template](../template/rfc.md).

## Dependency map

The dependency contract and category definitions are owned by the [documentation system](../README.md#rfc-dependencies).
Each row records the outgoing direct edges of one active proposal; sequence numbers alone imply no order.

| RFC | Hard | Conditional | Integration |
|---|---|---|---|
| [0004](0004-scan-file-revalidation.md) | None | None | None |
| [0021](0021-nonblocking-cover-art.md) | None | None | None |
| [0033](0033-nonblocking-playback-preparation.md) | None | None | None |

## Proposal inventory

- [RFC 0004: Scan file revalidation](0004-scan-file-revalidation.md) rechecks actionable paths before a prepared scan mutates the library.
- [RFC 0021: Non-blocking cover-art delivery](0021-nonblocking-cover-art.md) moves interactive resource reads and transforms off frontend event-loop threads.
- [RFC 0033: Non-blocking playback preparation](0033-nonblocking-playback-preparation.md) opens and prepares audio candidates away from the runtime callback executor.
