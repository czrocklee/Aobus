# Alignment Fix Plan for @app

## 1. Replace `int` with fixed-width integers (`std::int32_t`, `std::uint32_t`, etc.)
The style guide (2.7.1) requires using `std::` integer types and avoiding `int` or `unsigned`.
- `app/core/AppConfig.h` / `.cpp`: `kDefaultWindowWidth`, `kDefaultWindowHeight`, `kDefaultPanedPosition`, `kAppConfigVersion`, and `WindowState` members.
- `app/core/playback/FfmpegDecoderSession.h` / `.cpp`: `_audioStreamIndex`, `errorCode`, `ret`, `rawBitDepth`, `convertedSamples`.
- `app/platform/linux/playback/AlsaExclusiveBackend.cpp`: `err`.
- `app/platform/linux/playback/PipeWireBackend.h` / `.cpp`: `coreSyncSeq`, `oldState`, `newState`, `seq`, `spaFormat`.
- `app/platform/linux/ui/ImportProgressDialog.h` / `.cpp`: `maxItems`, `_maxItems`, `itemIndex`, and layout constants.
- `app/platform/linux/ui/ListRow.h` / `.cpp`: `getDepth()`, `setDepth()`, `_depth`.
- `app/platform/linux/ui/MainWindow.cpp`: `PanedInitialPosition` (also rename to `kPanedInitialPosition`).
- `app/platform/linux/ui/SmartListDialog.cpp` & `TagPromptDialog.cpp`: Layout constants like `kDialogWidth`, `kDialogHeight`, etc.

## 2. Constant Naming Conventions
The style guide (2.2.5) requires constants to use `kCamelCase`.
- `app/platform/linux/ui/MainWindow.cpp`: Rename `constexpr int PanedInitialPosition = 330;` to `constexpr std::int32_t kPanedInitialPosition = 330;`.

## 3. Class Member Declaration Order
The style guide (2.5.2) mandates: typedef/using → non-static member functions → static functions → non-static member variables → static member variables.
- `app/core/AppConfig.h`: Move `static AppConfig load();` below non-static member functions.
- `app/platform/linux/ui/TrackRow.h`: Move `static Glib::RefPtr<TrackRow> create(...)` below non-static getters.
- `app/platform/linux/ui/ListTreeNode.h`: Move `static Glib::RefPtr<ListTreeNode> create(...)` below non-static methods.

## 4. Header Include Order
The style guide (2.4.1) dictates: Paired header and project local -> Third-party -> Standard library.
- `app/platform/linux/ui/MainWindow.h`: Move local includes (`core/AppConfig.h`, `platform/linux/ui/StatusBar.h`, etc.) above third-party `<gtkmm.h>` and standard library includes `<vector>`, `<cstdint>`, etc.
- `app/platform/linux/ui/ListTreeNode.h`: Move `#include "platform/linux/ui/ListRow.h"` above standard library `<memory>`.
- `app/platform/linux/ui/StatusBar.h`: Move `#include "core/playback/PlaybackTypes.h"` above standard library `<chrono>`.

## 5. String Types and Parameters
The style guide (2.7.2, 2.7.3, 3.2.3) prefers `std::string_view` for non-owning strings and avoiding `char*`.
- `app/platform/linux/playback/PipeWireBackend.h`: Consider using `std::string_view` or `std::string const&` for the `errorMessage` parameter in `handleStreamStateChanged`.

## 6. Using `std::span` over container types for views
The style guide (3.1.3) prefers `std::span` for data buffer views.
- `app/platform/linux/ui/CoverArtWidget.h`: Update `void setCoverFromBytes(std::vector<std::byte> const& bytes);` to take `std::span<std::byte const> bytes`.

## 7. Use C++17 `if` with initialization
The style guide (3.2.6) dictates using init statements: `if (auto var = get(); condition)`.
There are numerous occurrences across the codebase where a variable is initialized via `auto` and then immediately checked in an `if` statement on the next line.
- `app/platform/linux/ui/MainWindow.cpp`: e.g. `if (auto folder = dialog->select_folder_finish(result); folder) { ... }`
- `app/platform/linux/ui/TrackViewPage.cpp`: e.g. `if (auto header = std::dynamic_pointer_cast<Gtk::ListHeader>(object); !header) { ... }`
- `app/core/playback/PlaybackEngine.cpp`: e.g. `if (auto decoder = FfmpegDecoderSession{outputFormat}; !decoder.open(...)) { ... }`
- `app/core/model/SmartListEngine.cpp`: e.g. `if (auto it = _states.find(id); it == _states.end()) { ... }`

## 8. Use `auto` with uniform initialization
The style guide (3.3.5) dictates preferring `auto x = T{};` over `T x;` and `auto x = T{a, b};` over `T x{a, b};` for non-primitive types.
- `app/core/model/TrackRowDataProvider.cpp`: `RowData row;` -> `auto row = RowData{};`
- `app/core/playback/FfmpegDecoderSession.cpp`: `PcmBlock block;` -> `auto block = PcmBlock{};`
- `app/platform/linux/playback/PipeWireBackend.cpp`: `NodeRecord record;` -> `auto record = NodeRecord{};`
- `app/platform/linux/ui/SmartListDialog.cpp`: `app::core::model::ListDraft draftData;` -> `auto draftData = app::core::model::ListDraft{};`