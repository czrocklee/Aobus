<p align="center">
  <a href="asset/brand/Soul.md">
    <img src="asset/brand/Soul.svg" width="240" alt="Aobus Soul Logo">
  </a>
</p>

<h1 align="center">Aobus</h1>

<p align="center">
  <strong>A C++26 music player with a shared audio engine and music library.</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-26-blue.svg" alt="C++26">
  <img src="https://img.shields.io/badge/License-MIT-green.svg" alt="License">
  <img src="https://img.shields.io/badge/Status-Active-brightgreen.svg" alt="Status">
</p>

---

Aobus (pronounced /'eɪ.oʊ.bʌs/) combines an LMDB-backed music library with platform audio output and graphical, terminal, and command-line frontends.

## What it does

- **Organize music:** index audio files, author Playlists and Smart Lists, and edit library metadata without writing back to the audio files.
- **Find and present tracks:** use filter expressions, grouping, sorting, and configurable columns.
- **Inspect playback quality:** select output routes and distinguish verified sample preservation from resampling, gain changes, or unavailable evidence. Bit-perfect output depends on the active path and device; a lossless source alone does not guarantee it.
- **Choose an interface:** use GTK on Linux, WinUI on Windows, or the TUI and CLI. The native AppKit frontend is an incremental macOS development slice.

## Documentation

- **Use Aobus:** [get started](doc/user/get-started.md), [play music](doc/user/play-music.md), [manage your library](doc/user/manage-library.md), and [back up your data](doc/user/backup-and-restore.md).
- **Choose a frontend:** [Windows desktop](doc/user/use-windows-desktop.md), [TUI](doc/user/use-tui.md), or [CLI](doc/user/use-cli.md); [GTK customization](doc/user/customize-application.md) covers the Linux desktop.
- **Understand the system:** [system overview](doc/system/overview.md), [subsystem contracts](doc/system/README.md), and [exact reference](doc/reference/README.md).
- **Contribute:** [contributor workflow](CONTRIBUTING.md) and [development tasks](doc/development/README.md).

The [documentation home](doc/README.md) connects these reading paths.

## Build and run

Aobus uses CMake with pinned Nix dependencies on Linux and the shared governed vcpkg manifest on macOS and Windows.
On Linux, the portal selects the development environment automatically:

```bash
./ao build
./ao run tui
```

See [build and setup](doc/development/build.md) for build trees and tooling, or the native [macOS](doc/development/macos.md) and [Windows](doc/development/windows.md) guides for prerequisites.
Use `ao.bat` on Windows. The [native AppKit desktop](doc/development/macos.md#native-desktop-development-slice) is available through `./ao run appkit` and remains under development.

For testing, start with the [testing guide](doc/development/test.md); [validation and review](doc/development/test/validation-and-review.md) selects the required checks for a change.

## 🤖 AI Agents

If you are an AI agent working on this project, please read [AGENTS.md](AGENTS.md) for critical environment setup and coding standards.

## 📄 License

The Aobus source code is licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

**Brand Assets Exception:**
The Aobus logo and its associated design documentation under `asset/brand/` are the personal intellectual property of YANG LI and are **NOT** covered by the MIT License.
Their [brand asset license](asset/brand/LICENSE.txt) permits unmodified bundled distribution with Aobus while reserving modification, standalone reuse, and other brand rights.

---

<p align="center">
  <i>"Where audio structure meets artistic resonance."</i>
</p>
