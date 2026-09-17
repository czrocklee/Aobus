---
id: library.spec-index
---
# Library

The library connects durable Track, List, dictionary, file, and resource facts to live application views:

```text
MusicLibrary storage and transactions
  -> runtime Library reads, commands, jobs, and changes
  -> leased TrackSource membership and order
  -> track-list and track-detail projections
  -> workspace, playback, and presentation consumers
```

One `CoreRuntime` owns the storage, runtime library facade, change bus, and source cache for a music root.
Interactive `AppRuntime` composition adds `ViewService`, live projections, workspace, and playback consumers above that core; CLI operations do not construct the interactive projection layer.
[Structure and storage ownership](structure.md) explains these capabilities, dependency boundaries, and lifetime owners; the [system overview](../overview.md) places them within Aobus.

## Change or investigate a behavior

| Task | Contract |
|---|---|
| Add a read or mutation command | [Reads and mutation](mutation.md), then [change publication](change-publication.md) |
| Schedule a long-running operation | [Task execution](task-execution.md), then the operation's own contract |
| Scan, reconcile, relink, or recover file identity | [Scan and identity](scan-and-identity.md) |
| Export, restore, merge, or preview library YAML | [Transfer](yaml-transfer.md) and [YAML format](../../reference/library/format/yaml.md) |
| Change ordered membership, leases, or cached sources | [Track sources](track-source.md) |
| Change rows, grouping, deltas, or invalidation | [Track-list projection](track-list-projection.md) |
| Change detail snapshots or field aggregation | [Track-detail projection](track-detail-projection.md) |
| Change metadata or storage representation | [Track](../../reference/library/model/track.md), [List](../../reference/library/model/list.md), [field vocabulary](../../reference/library/model/track-field.md), and [database format](../../reference/library/storage/database.md) |

The mutation and publication contracts remain separate because transaction settlement and callback observation have independent ordering and failure guarantees.
Do not hold a transaction across coroutine suspension or reconstruct these guarantees from a frontend adapter.

## Neighboring concerns

- [Media reading](../media/README.md) supplies encoded-file evidence; [resource delivery](../resource/README.md) reads verified content without storing cover bytes in the library.
- [Expressions](../query/README.md) define membership predicates and scalar formatting; [workspace](../workspace/README.md) owns interactive view identity and navigation.
- [Presentation](../presentation/README.md) owns authoring sessions and display policy, not committing transaction authority.
- [LMDB operations](../persistence/lmdb-operation.md) define the reusable native transaction mechanism below library policy.

Public runtime code is under `app/include/ao/rt/library/`, `source/`, and `projection/`, with implementations under the matching `app/runtime/` paths.
Core storage is under `include/ao/library/` and `lib/library/`; focused tests are in `test/unit/library/` and the matching runtime directories.
