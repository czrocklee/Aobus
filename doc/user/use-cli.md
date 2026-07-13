---
id: user.use-cli
type: user-guide
status: current
domain: cli
summary: Initializes and automates an Aobus library with dry-run and structured CLI output.
---
# Use the Aobus CLI

## Outcome

You can initialize, inspect, scan, query, and mutate a library from a shell without depending on a graphical session.

## Steps

1. Select the library root on every command with `-C`, or export `AOBUS_ROOT` once for the shell.
2. Initialize a new root and inspect it:

   ```bash
   aobus -C /music init
   aobus -C /music lib show
   ```

3. Preview and apply the first scan:

   ```bash
   aobus -C /music scan --dry-run --verbose
   aobus -C /music scan
   ```

4. Query tracks using either explicit ids or a filter:

   ```bash
   aobus -C /music track show --filter '$artist == "Miles Davis"'
   ```

5. Add `-O json` or `-O yaml` when another program consumes the result:

   ```bash
   aobus -C /music -O json lib stats
   ```

6. Use `--dry-run` before a supported mutation, review the exact target count, then remove the flag to commit.
7. Use `--help-all` for the complete command tree and the [CLI command reference](../reference/cli/command.md) for stable field and exit-code lookup.

Standard output contains command payloads.
Diagnostics and verbose progress use standard error, allowing structured standard output to remain machine-readable.
Success, including an empty result or no-op, exits `0`; domain and internal failures exit `1`; command-line usage errors use a nonzero CLI parser status.

## Verify the result

- `aobus -C /music lib show` reports the intended library identity.
- A structured command parses as one complete JSON or YAML document.
- A committed mutation is visible through a subsequent read command.
- Repeating its dry-run reports the expected no-op or already-applied state when the operation is idempotent.

## Related documents

- [CLI command reference](../reference/cli/command.md)
- [CLI execution specification](../spec/cli/execution.md)
- [Predicate language reference](../reference/query/predicate-language.md)
- [Format language reference](../reference/query/format-language.md)
