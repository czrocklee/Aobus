---
id: development.macos-portability
type: development
status: current
domain: development
summary: Registers every deviation the macOS build requires, with the condition and procedure for removing each one.
---
# macOS portability compromises

## Scope

This guide owns the deviations the repository carries so the macOS build
compiles, links, and passes its tests. Each entry records what the deviation
is, what forces it, and the exact condition under which it stops being
necessary, so a future toolchain upgrade can retire it deliberately rather than
leaving it in place because nobody remembers why it exists.

Machine setup, portal behavior, local state, and the SMB workflow belong to
[macOS development](macos.md).

## Current support

The macOS profile builds the shared core libraries, the CLI, the FTXUI terminal
application, and the native test suites. There is no GTK or Cocoa desktop
frontend. Audio playback uses the native Core Audio shared backend, and the TUI
can render through any live Core Audio output device published by macOS.

The toolchain is Clang 22 from Homebrew's `llvm@22` formula, targeting
`-mmacosx-version-min=14.0`. Compilation uses that formula's libc++ headers and
links Apple's system `/usr/lib/libc++.1.dylib`. Apple's availability annotations
are load-bearing and must not be suppressed. Defining
`_LIBCPP_DISABLE_AVAILABILITY` makes the build succeed and then fails at process
load with a missing symbol.

## Removable deviations

Every entry below exists only because of a defect or gap outside this
repository. None of them is a design choice.

| # | Deviation | Location | Retire when |
|---|---|---|---|
| 1 | `ao::compat::MoveOnlyFunction` | `include/ao/compat/MoveOnlyFunction.h` | libc++ defines `__cpp_lib_move_only_function` |
| 2 | `ao::compat::views::enumerate` | `include/ao/compat/Enumerate.h` | libc++ defines `__cpp_lib_ranges_enumerate` |
| 3 | `ao::compat::AtomicSharedPtr` | `include/ao/compat/AtomicSharedPtr.h` | libc++ defines `__cpp_lib_atomic_shared_ptr` |
| 4 | libc++ `expected` shim | `script/ao/macos-vcpkg-bootstrap.sh`, `cmake/CompilerOptions.cmake` | libc++ constrains `expected`'s equality operators without self-reference |
| 5 | `-Wno-c2y-extensions` | `cmake/CompilerOptions.cmake` | fakeit stops expanding `__COUNTER__`, or Clang stops reporting it |
| 6 | `StringMaker<file_time_type>` | `test/unit/FilesystemTestSupport.h` | Catch2 can stringify `__int128`, or Darwin stops using it for the file clock |
| 7 | c4core `C4_CPP=17` header mode | `cmake/Dependencies.cmake` | c4core accepts C++26 consumers under Darwin Clang without invalid likelihood attributes |

### 1-3: standard-library seams

libc++ does not implement `std::move_only_function`, `std::views::enumerate`,
or `std::atomic<std::shared_ptr<T>>`. This was verified against libc++ 21.1.6
and a locally built 22.1.8, not assumed from documentation.

Each gap has an `ao::compat::detail::Portable*` implementation plus an
`ao::compat` public alias, adaptor, or backend-selecting wrapper that uses the standard facility where
the feature-test macro reports it. Two
properties matter when removing them:

- The portable implementation is compiled and unit tested on **every** platform,
  not only macOS. Deleting a seam therefore changes Linux and Windows test
  coverage, not just macOS.
- The public surface already switches automatically. When libc++ implements a feature,
  macOS silently starts using the standard implementation and the portable code becomes
  dead. Nothing breaks, and nothing announces it either, which is why the
  retirement condition is written down here.
- A seam must cover the whole API surface the project uses, not the subset that
  happens to compile. Two seams were short of that and neither failed a build:
  `AtomicSharedPtr` omitted compare-exchange while
  `app/tui/SignalExitWatcherWindows.cpp` calls it, and `PortableEnumerateView`
  omitted const traversal and `size()`. Both hid for the same reason -- the
  translation unit that would have caught them never selects the portable side.
  Each portable test now also runs the same operations through the
  `ao::compat` surface, so the two implementations are compared on every
  platform rather than only where one of them is chosen.

Removal is mechanical: migrate call sites back to the standard spelling, delete
the header and its test, and drop the test registration from
`test/CMakeLists.txt`. Use `rg` to enumerate current call sites rather than
preserving a count here that will drift as the codebase changes.

### 4: libc++ `expected` shim

libc++ constrains `std::expected`'s equality operators only in C++26 mode, and
those constraints are self-referential:

```cpp
template <class _T2>
friend constexpr bool operator==(const expected& __x, const _T2& __v)
  requires(!__is_std_expected<_T2>::value) && requires {
    { *__x == __v } -> __core_convertible_to<bool>;
  }
```

The operator is a hidden friend, so ADL makes it a candidate for any comparison
whose operands associate `std::expected`. `std::reverse_iterator<expected<T, E>*>`
does, and `std::vector`'s reallocation guard compares exactly those; so does
Boost.Asio's `promise_executor` over a `Result`-returning coroutine, which
`asio::traits::equality_comparable` compares explicitly. Checking the constraint
re-enters the same specialisation, and satisfaction that depends on itself is an
error rather than a substitution failure.

The first parameter is the injected class name and therefore non-deduced, and
the value type converts to `expected`, so the candidate is viable and the
constraint is checked before the conversion is rejected. There is no way to
avoid it from the call site.

The macOS portal builds a copy of `__expected/expected.h` with the five C++26
gates disabled, and `cmake/CompilerOptions.cmake` puts that directory ahead of
the real one with `-isystem`. Two mechanics are easy to get wrong:

- **Only a command-line `-isystem` works.** The real libc++ directory is built
  into the compiler driver rather than passed as an argument. Environment-only
  include injection was measured to lose that race and produces a silent
  no-op: the build fails exactly as before, with nothing pointing at the cause.
- **Only that one header may be copied.** libc++'s own `<cstdint>` reaches the
  SDK through `#include_next`, and a duplicate include tree on the search path
  captures that hop, leaving `intmax_t` unresolved.
- **The SDK C include directory must remain driver-owned.** `LLVMSupport`
  transitively links CMake's `ZLIB::ZLIB` target. On Xcode installations,
  FindZLIB can export `MacOSX.sdk/usr/include` through that target using a
  symlink spelling that CMake does not recognize as one of Clang's implicit
  directories. The lint target filters that redundant entry because making it
  an explicit `-isystem` path places it ahead of libc++'s C wrappers; headers
  such as `<cstdlib>` then reach the SDK before libc++ can establish its wrapper
  contract. Clang still discovers the same SDK directory through its sysroot,
  after libc++, which is the required order.

The portal transform asserts that exactly five gates are present and fails the
build otherwise, so an `llvm@22` update that reworks the file stops the build
instead of silently producing an unpatched header. That failure is the signal
to re-test whether the shim is still needed.

Disabling the constraints restores the C++23 form of the operators. That is a
step away from C++26 conformance, but it is what libstdc++ and the MSVC STL
compile against today, so macOS behaves exactly like the other platforms rather
than a third way.

To retire it: delete the transform and export from
`script/ao/macos-vcpkg-bootstrap.sh`, delete the `if(APPLE)` block from
`cmake/CompilerOptions.cmake`, and confirm the core test suite still builds on
macOS. A minimal check is a `std::vector<std::expected<T, E>>` that reallocates,
compiled with `-std=gnu++26`.

### 5: `-Wno-c2y-extensions`

fakeit's `Method()` macro expands to `__COUNTER__`, which Clang 22 reports as a
C2y extension under `-Wpedantic`. The construct is third-party. The flag is
gated on Clang because only Clang has both the diagnostic and the option, and it
applies to normal and sanitizer builds alike.

### 6: `StringMaker<file_time_type>`

Darwin counts `std::filesystem::file_time_type` in `__int128` nanoseconds, and
no `operator<<` accepts that type, so Catch2's default stringifier fails to
compile as soon as a comparison of two file times is decomposed. The
specialisation formats the tick count by hand. It is defined unconditionally so
the failure text is identical on every platform.

### 7: c4core C++17 header mode

c4core 0.5.0 classifies a C++26 consumer as C++23 and selects standard
likelihood attributes for `C4_LIKELY` and `C4_UNLIKELY`. rapidyaml uses those
macros in expression positions where Darwin Clang 22 rejects the resulting
attribute placement.

`PkgRapidYaml` therefore defines `C4_CPP=17` for Darwin Clang consumers while
leaving Aobus itself in C++26 mode. The pre-existing MSVC compatibility mode is
retained separately. Linux Clang is deliberately outside the Darwin condition,
so this workaround cannot silently change its dependency headers.

To retire it: remove the Darwin condition from `cmake/Dependencies.cmake`,
build the affected rapidyaml consumers with the current Homebrew Clang in
C++26 mode, and run the complete macOS gate. A dependency upgrade is not enough
evidence unless those consumers compile without the definition.

## Permanent platform differences

These are not compromises and must not be removed. They are genuine differences
between Darwin and Linux, and deleting them breaks macOS again.

| Difference | Location |
|---|---|
| `mdb_mode_t` is 16-bit on Darwin, 32-bit on Linux | `include/ao/lmdb/Environment.h` |
| `sigemptyset` and `sigaddset` are function-like macros | `lib/utility/Fatal.cpp`, `app/tui/SignalExitWatcherPosix.cpp` |
| `environ` is declared only for the main executable | `test/fatal/ProbeProcessPosix.cpp` |
| No procfs, so `/proc/self/exe` does not resolve | `test/fatal/ProbeProcessPosix.cpp` |

The `Environment.h` mapping is guarded by a `static_assert` against
`::mdb_mode_t` in `lib/lmdb/Environment.cpp`, so a wrong branch fails the build
rather than corrupting a mode.

The affected translation units `#undef` the function-like macros after
including `<signal.h>` and then call the SDK's ordinary POSIX declarations.
C11 7.1.4 explicitly defines this as the way to ensure that an actual function
is referenced when a standard header also supplies a macro form.

## Validation

A deviation is retired only after the macOS core suite builds and passes
without it. Removing one is not a Linux-only change even when it looks like one,
because the seams are compiled and tested on every platform.

Record the command, commit, platform, and pass/fail result when validating a
retirement. Do not pin exact Catch2 case or assertion totals here: those totals
change whenever unrelated tests are added and become stale evidence. Platform
differences in discovered cases are expected because GTK and Linux-specific
tests are not part of the macOS profile; skipped or failed cases are not.
