# C++ Rule Snippet Index

Use this index to choose focused examples before writing or editing Aobus C++ code. Rule numbers refer to `CONTRIBUTING.md`.

| Rule(s) | Topic | Snippet file |
| --- | --- | --- |
| 1.1 | C++23 target, no modules | `01-style-and-structure.md` |
| 2.1 | Formatting, blank lines around control blocks and test macros | `01-style-and-structure.md` |
| 2.2 | Naming conventions | `01-style-and-structure.md` |
| 2.3 | Header guard policy | `01-style-and-structure.md` |
| 2.4 | Include grouping order | `01-style-and-structure.md` |
| 2.5 | Header access/member order and `.cpp` definition order | `01-style-and-structure.md` |
| 2.6 | Namespaces, anonymous namespaces, C/C++ library qualification | `01-style-and-structure.md` |
| 2.7 | Integer, string, array, and `using` type choices | `02-types-and-modern-cpp.md` |
| 2.8 | Cast style | `02-types-and-modern-cpp.md` |
| 2.9 | Output and runtime diagnostics | `02-types-and-modern-cpp.md` |
| 3.1 | C++20 concepts, format, span, ranges, no-unique-address, prefix/suffix, designated init, `jthread` | `02-types-and-modern-cpp.md` |
| 3.2 | C++17 optional, variant, string_view, if constexpr, structured binding, init-statement, unused handling | `02-types-and-modern-cpp.md` |
| 3.3 | C++23 `std::expected` via `ao::Result` | `03-design-threading-and-errors.md` |
| 3.4 | RAII, `override`, member init, `noexcept`, initialization, return syntax, lambdas | `02-types-and-modern-cpp.md`, `03-design-threading-and-errors.md` |
| 4.1 | Trivial getters/setters inline in headers | `03-design-threading-and-errors.md` |
| 4.2 | Class design, `final`, type exposure, Pimpl | `03-design-threading-and-errors.md` |
| 4.3 | Const correctness | `03-design-threading-and-errors.md` |
| 4.4 | Threading | `03-design-threading-and-errors.md` |
| 5.1 | Three-layer error policy | `03-design-threading-and-errors.md` |
| 5.2 | Error type and success/error construction | `03-design-threading-and-errors.md` |
| 5.3 | Error-handling anti-patterns | `03-design-threading-and-errors.md` |
| Tests | Catch2, FakeIt, sections, generators, matchers, integration setup | `04-test-snippets.md` |

## Detailed coverage checklist

Use this checklist when auditing generated code against `CONTRIBUTING.md`:

- 1.1: C++23 without modules.
- 2.1.1-2.1.2: clang-format style; blank lines around control blocks and top-level test macros; no leading/trailing blank lines inside blocks; comment-specific `if` exception.
- 2.2.1-2.2.6: `PascalCase` types, `camelCase` functions/variables, `_camelCase` class data members, plain struct members, `kCamelCase` constants, enum-value casing by enum kind.
- 2.3.1: `#pragma once` headers.
- 2.4.1: include groups in project/third-party/standard order.
- 2.5.1-2.5.3: `.cpp` definition order matches header; access order is `public` -> `protected` -> `private`; section member order is nested types/aliases, functions, static functions, data members, static data members, friends.
- 2.6.1-2.6.4: nested namespace syntax; anonymous namespaces for internal linkage; `::` prefix for external C APIs; `std::` prefix for standard C functions/types.
- 2.7.1-2.7.4: fixed-width integers, `std::string`, `std::array`/`std::to_array`, `using` aliases.
- 2.8.1-2.8.2: no C-style casts; use `static_cast` for numeric conversions in logs/format strings.
- 2.9.1-2.9.2: prefer `'\n'`; use project logging for diagnostics.
- 3.1.1-3.1.8: concepts, `std::format`, `std::span`, `std::ranges`, `[[no_unique_address]]`, `starts_with`/`ends_with`, designated initializers, `std::jthread`/`std::stop_token`.
- 3.2.1-3.2.8.2: optional-for-absence with `opt` prefix and concise checks; variant; string_view; if constexpr; structured bindings; init-statements; no `[[nodiscard]]`; unused parameters as comments and conditional unused values as `[[maybe_unused]]`.
- 3.3.1: `ao::Result<T>`/`ao::Result<>` for recoverable failures; no bool+lastError, optional failure, or `std::error_code` for new code.
- 3.4.1-3.4.7: RAII/custom deleters, `override` without repeated `virtual`, brace member initialization, `noexcept`, project initialization style, traditional return types, no empty lambda parameter list.
- 4.1.1: trivial one-line getters/setters inline in headers.
- 4.2.1-4.2.3: concrete classes/POD structs `final`, narrow type exposure, Pimpl for complex implementation details.
- 4.3.1: const locals, member functions, pointed-to data, parameters, and mandatory services by reference.
- 4.4.1-4.4.4: name background threads, `std::jthread`/`std::stop_token`, mutex + `std::scoped_lock`, atomics for simple shared flags/counters, no `volatile` synchronization.
- 5.1.1-5.1.3: `ao::Result<T>` for recoverable operations, `AO_THROW`/`AO_THROW_FORMAT` for invariant violations, `std::optional<T>` for legitimate absence.
- 5.2.1-5.2.2: `ao::Error` with `Code` and message; `ao::makeError`; `return {};` for `ao::Result<>` success; `std::unexpected(ao::Error{...})` for explicit errors.
- 5.3.1-5.3.4: avoid bool+lastError, empty-string success, optional-as-error, and low-level exception-to-string conversion.

## Reading guidance

- Read only the snippet files relevant to the target code path, plus a nearby sibling source file in the actual project tree.
- Prefer nearby project code when it is more specific than a snippet; prefer `CONTRIBUTING.md` when a snippet and local code conflict.
- Snippets are partial examples. Do not copy them as complete files.
