---
name: handling-clang-tidy-warnings
description: Resolves clang-tidy warnings and NOLINT suppressions in Aobus C++ code. Use when fixing clang-tidy output, reducing NOLINT usage, deciding whether a warning is a tool limitation, or validating warning cleanup.
---

# Handling clang-tidy Warnings

Use this skill to turn clang-tidy warnings and existing `NOLINT` suppressions into small, correct C++ changes without cargo-culting either the tool or the suppression.

## Required Companion Skills

- Load `generate-cpp-code` before editing any `.cpp`, `.h`, or `.hpp` file.
- Load `check-code-conformance` when auditing style/conformance warnings, broad warning sets, or `NOLINT` cleanup.
- If git commands are needed, load `manage-git-flow` before running them.

## Core Principle

Do not start by asking “how do I silence clang-tidy?” Start by classifying the warning:

1. **True code issue**: fix the code.
2. **Code organization issue**: move the awkward pattern behind a narrow helper or clearer local abstraction.
3. **External API/framework boundary**: keep a narrow suppression with a clear reason.
4. **Tool false positive**: first try a more direct include/type/expression; suppress only after proving the direct form is not understood by the tool.
5. **Low-value style noise**: prefer local constants/names over broad config changes, but do not add heavy abstractions just to satisfy the tool.

## Workflow

### 1. Reproduce the warning narrowly

Prefer the project script for specific files:

```bash
./script/run-clang-tidy.sh path/to/file.cpp path/to/header.h
```

If the user gives a file, run that file and the smallest related set. Avoid a full `./build.sh debug --tidy --clean` unless the task is explicitly full-project cleanup.

When a build target is needed for compile validation, use the project entrypoint with a target, for example:

```bash
./build.sh debug --target ao_test
```

Do not use a clang-tidy build tree as proof of normal compilation if it is failing because clang-tidy is analyzing unrelated target files or system headers.

### 2. Read the warning in code context

Read the full local function or small file section before editing. Identify:

- Which check emitted the warning.
- Whether the code is project-owned or dictated by GTKmm, GLib, PipeWire, ALSA, yaml-cpp, Boost, Catch2, or another external API.
- Whether the warning appears once or is a repeated pattern.
- Whether the current `NOLINT` is narrow or hiding a wider region.

### 3. Choose the smallest honest fix

Use this decision table.

| Warning pattern | Prefer | Avoid |
| --- | --- | --- |
| Magic numbers in UI, timeouts, formatting, buffer sizes | Local `constexpr` with a domain name | Global `Constants.h` for one-use values |
| Protocol offsets, masks, alignment | Named `constexpr` near parser/layout code | Comments plus `NOLINT` when a name would clarify |
| Repeated raw `new/delete` for toolkit data hooks | One helper that owns the C boundary and one narrow suppression | Many copied `new/delete` suppressions in business logic |
| RAII guard special members | Explicitly delete copy/move or define needed operations | `NOLINT(cppcoreguidelines-special-member-functions)` on a guard |
| Short structured-binding dummy names | Use a named pair/object or meaningful binding name | Repeated `NOLINT(readability-identifier-length)` for `_` |
| Include-cleaner missing provider | Add the direct header that provides the symbol | Guessing umbrella includes or broad `NOLINTBEGIN` |
| C varargs API | Local suppression at call site | Wrapping varargs in unsafe homegrown abstractions |
| GTKmm `make_refptr_for_instance(new T)` | Narrow suppression on the construction | Disabling owning-memory for the whole GTK tree |
| Capturing coroutine lambdas | Named coroutine helper/member function with explicit parameters | Long-term suppressions around captured `this`/references |

## Known Good Refactoring Moves

### Replace a one-off magic number

Before:

```cpp
Glib::signal_timeout().connect(callback, 3000); // NOLINT(readability-magic-numbers)
```

After:

```cpp
namespace
{
  constexpr std::uint32_t kPlaylistWriteDelayMs = 3000;
}

Glib::signal_timeout().connect(callback, kPlaylistWriteDelayMs);
```

### Collapse repeated toolkit ownership suppressions

When an external API stores `void*` plus a destroy callback, keep the raw ownership transfer in exactly one helper:

```cpp
void destroyConnectionData(::gpointer data) noexcept
{
  if (data == nullptr)
  {
    return;
  }

  auto conn = std::unique_ptr<sigc::connection>{static_cast<sigc::connection*>(data)};
  conn->disconnect();
}

void setConnectionData(Glib::RefPtr<Gtk::ListItem> const& item, char const* key, sigc::connection connection)
{
  auto stored = std::make_unique<sigc::connection>(std::move(connection));
  item->set_data(key, stored.release(), destroyConnectionData); // NOLINT(cppcoreguidelines-owning-memory)
}
```

This is not about hiding a warning; it makes the ownership boundary explicit and removes repeated manual `delete` code.

### Fix a RAII guard instead of suppressing it

For a local guard with a destructor and reference members, declare intent:

```cpp
struct ParenthesisGuard final
{
  ParenthesisGuard(std::ostringstream& out, bool apply);
  ParenthesisGuard(ParenthesisGuard const&) = delete;
  ParenthesisGuard& operator=(ParenthesisGuard const&) = delete;
  ParenthesisGuard(ParenthesisGuard&&) = delete;
  ParenthesisGuard& operator=(ParenthesisGuard&&) = delete;
  ~ParenthesisGuard();

  std::ostringstream& out;
  bool apply;
};
```

## Best Practices & Common Pitfalls

Based on project-wide cleanups, follow these battle-tested rules:

### 1. Precise Symbol Resolution with Pkg-Config
*   **The Problem**: Blindly searching `/nix/store` or the whole project for third-party symbols is extremely slow and prone to finding the wrong version.
*   **The Solution**: Use `pkg-config` to locate the exact include directory first:
    ```bash
    nix-shell --run "pkg-config --variable=includedir libpipewire-0.3"
    ```
    Then, search only within that specific path for the symbol (e.g., `grep -r "struct spa_source" /path/to/include`).

## Suppression Policy

Use `NOLINT` only after attempting the real fix.

Preferred order:

1. Change code so the warning disappears while behavior is clearer.
2. Add a direct include or named type/expression that clang-tidy can understand.
3. Move an unavoidable external API boundary into a small helper and suppress only that boundary.
4. Use `NOLINTNEXTLINE` or inline `NOLINT(check-name)` at the exact call/expression.
5. Use `NOLINTBEGIN/END` only for a compact, contiguous framework/DSL region that cannot be made clearer locally.
6. Avoid directory-wide check disables unless the check is systematically wrong for the entire directory and would hide no useful signal.

Good suppressions are:

- Narrow.
- Check-specific.
- Near the unavoidable API shape.
- Preferably accompanied by code that already communicates why the exception exists.

Bad suppressions are:

- Broad `NOLINTBEGIN` around normal project code.
- “Because clang-tidy complains” comments.
- Repeated at many call sites for the same ownership or include pattern.

## Include-cleaner Triage

For `misc-include-cleaner`:

1. Search for the provider header if the symbol is from a third-party library.
2. Prefer the most specific direct header.
3. If the specific header still fails because of template-heavy or macro-heavy libraries, keep a narrow suppression.
4. Do not replace precise project includes with broad umbrella includes just to appease include-cleaner.
5. If a header is required only for YAML/Boost/Asio template specialization visibility, document that by keeping the suppression on the include line rather than the whole file.

**Type-to-header lookup:** Use `references/type-to-header-map.md` to find the exact header for any third-party, standard library, or internal Aobus type. The map covers GTKmm, YAML, Boost, Catch2, STL, audio, LMDB, and all `ao::` namespaces. Always check the map before guessing an include path or adding a NOLINT.

## Coroutine Warning Triage

Treat coroutine warnings as higher risk than ordinary style warnings.

- `cppcoreguidelines-avoid-capturing-lambda-coroutines` is often a real lifetime smell.
- Prefer a named coroutine helper/member function where parameters are explicit and copied/moved into the coroutine frame.
- Reference coroutine parameters may be acceptable for mandatory services whose lifetime is externally guaranteed, but keep the suppression narrow and re-check the ownership path.
- Do not “fix” by changing references to raw pointers unless that makes the lifetime contract clearer.

## Verification

Run the narrowest useful command.

For warning cleanup:

```bash
./script/run-clang-tidy.sh touched/file1.cpp touched/file2.h
```

For compile validation of production changes:

```bash
./build.sh debug --target <target>
```

For broader shared-library or cross-module changes, use the relevant unit-test target or the default debug build. Report unrelated pre-existing clang-tidy/system-header failures separately instead of treating them as proof the change failed.

## Reporting

Summarize outcomes by pattern, not by dumping every file:

- How many suppressions or warnings were removed.
- Which suppressions remain and why they are real boundaries.
- Exact verification commands and pass/fail status.
- Any unrelated warnings or toolchain limitations encountered.

When the change demonstrates a larger point, say so explicitly: for example, “This was not a clang-tidy limitation; the repeated raw ownership was better expressed as one helper.”

## References

- **`references/type-to-header-map.md`** — exact header paths for third-party, STL, and internal Aobus types. Consult this before adding or removing any `#include`.
