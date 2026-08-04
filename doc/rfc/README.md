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
| [RFC 0001: Result-oriented library mutations and write savepoints](0001-library-mutation-savepoints.md) | None | None | None |

## Proposal inventory

- [RFC 0001: Result-oriented library mutations and write savepoints](0001-library-mutation-savepoints.md) proposes one recoverable Result contract for library mutations, nested LMDB savepoints under one writer authority, and a transaction-chain Dictionary journal.
