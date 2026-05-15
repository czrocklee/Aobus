# Third-Party Type Map

This document maps public types from third-party libraries used in Aobus to their exact declaring header files within the Nix store.

## UI / GTK Suite (gtkmm-4.0 / glibmm-2.68)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `Gtk::Widget` | `<gtkmm/widget.h>` | `Gtk` |
| `Gtk::ApplicationWindow` | `<gtkmm/applicationwindow.h>` | `Gtk` |
| `Gtk::Box` | `<gtkmm/box.h>` | `Gtk` |
| `Gtk::Label` | `<gtkmm/label.h>` | `Gtk` |
| `Gtk::Button` | `<gtkmm/button.h>` | `Gtk` |
| `Gtk::ToggleButton` | `<gtkmm/togglebutton.h>` | `Gtk` |
| `Gtk::ScrolledWindow` | `<gtkmm/scrolledwindow.h>` | `Gtk` |
| `Gtk::Picture` | `<gtkmm/picture.h>` | `Gtk` |
| `Gtk::Stack` | `<gtkmm/stack.h>` | `Gtk` |
| `Gtk::PopoverMenuBar` | `<gtkmm/popovermenubar.h>` | `Gtk` |
| `Gtk::Revealer` | `<gtkmm/revealer.h>` | `Gtk` |
| `Gtk::CssProvider` | `<gtkmm/cssprovider.h>` | `Gtk` |
| `Gtk::Settings` | `<gtkmm/settings.h>` | `Gtk` |
| `Gtk::StyleContext` | `<gtkmm/stylecontext.h>` | `Gtk` |
| `Gtk::Snapshot` | `<gtkmm/snapshot.h>` | `Gtk` |
| `Gtk::Orientation` | `<gtkmm/enums.h>` | `Gtk` |
| `Gtk::Align` | `<gtkmm/enums.h>` | `Gtk` |
| `Gtk::PolicyType` | `<gtkmm/enums.h>` | `Gtk` |
| `Gtk::ResponseType` | `<gtkmm/enums.h>` | `Gtk` |
| `Gtk::SizeRequestMode` | `<gtkmm/enums.h>` | `Gtk` |
| `Glib::RefPtr` | `<glibmm/refptr.h>` | `Glib` |
| `Glib::ustring` | `<glibmm/ustring.h>` | `Glib` |
| `Glib::Dispatcher` | `<glibmm/dispatcher.h>` | `Glib` |
| `Glib::VariantBase` | `<glibmm/variant.h>` | `Glib` |
| `Glib::KeyFile` | `<glibmm/keyfile.h>` | `Glib` |
| Glib::Object | <glibmm/object.h> | Glib |
| `Glib::Error` | `<glibmm/error.h>` | `Glib` |
| `Gio::Menu` | `<giomm/menu.h>` | `Gio` |
| `Gio::MenuModel` | `<giomm/menumodel.h>` | `Gio` |
| `Gio::Application` | `<giomm/application.h>` | `Gio` |
| `Gio::File` | `<giomm/file.h>` | `Gio` |
| `Gdk::Display` | `<gdkmm/display.h>` | `Gdk` |
| `Gdk::Pixbuf` | `<gdk-pixbuf/gdk-pixbuf.h>` | `Gdk` |
| `Gdk::FrameClock` | `<gdkmm/frameclock.h>` | `Gdk` |
| `Gdk::Texture` | `<gdkmm/texture.h>` | `Gdk` |
| `Gdk::ModifierType` | `<gdkmm/enums.h>` | `Gdk` |
| `Gdk::DragAction` | `<gdkmm/enums.h>` | `Gdk` |
| `Gtk::Dialog` | `<gtkmm/dialog.h>` | `Gtk` |
| `Gtk::ColumnView` | `<gtkmm/columnview.h>` | `Gtk` |
| `Gtk::ColumnViewColumn` | `<gtkmm/columnviewcolumn.h>` | `Gtk` |
| `Gtk::ListItem` | `<gtkmm/listitem.h>` | `Gtk` |
| `Gtk::SignalListItemFactory` | `<gtkmm/signallistitemfactory.h>` | `Gtk` |
| `Gtk::SelectionModel` | `<gtkmm/selectionmodel.h>` | `Gtk` |
| `Gtk::SingleSelection` | `<gtkmm/singleselection.h>` | `Gtk` |
| `Gtk::FlowBoxChild` | `<gtkmm/flowboxchild.h>` | `Gtk` |
| `Gtk::ListBox` | `<gtkmm/listbox.h>` | `Gtk` |
| `Gtk::ListBoxRow` | `<gtkmm/listboxrow.h>` | `Gtk` |
| `Gtk::Popover` | `<gtkmm/popover.h>` | `Gtk` |
| `Gtk::Image` | `<gtkmm/image.h>` | `Gtk` |
| `Gtk::GestureLongPress` | `<gtkmm/gesturelongpress.h>` | `Gtk` |
| `Gtk::GestureClick` | `<gtkmm/gestureclick.h>` | `Gtk` |
| `Gtk::EventControllerKey` | `<gtkmm/eventcontrollerkey.h>` | `Gtk` |
| `Gtk::DropTarget` | `<gtkmm/droptarget.h>` | `Gtk` |
| `Gtk::Entry` | `<gtkmm/entry.h>` | `Gtk` |
| `Gtk::SortListModel` | `<gtkmm/sortlistmodel.h>` | `Gtk` |
| `Gtk::Sorter` | `<gtkmm/sorter.h>` | `Gtk` |
| `Gtk::ListHeader` | `<gtkmm/listheader.h>` | `Gtk` |
| `Gtk::MenuButton` | `<gtkmm/menubutton.h>` | `Gtk` |
| `Gtk::MultiSelection` | `<gtkmm/multiselection.h>` | `Gtk` |
| `Gtk::PropagationPhase` | `<gtkmm/eventcontroller.h>` | `Gtk` |
| `Gtk::EventSequenceState` | `<gtkmm/gesture.h>` | `Gtk` |
| `Gtk::SelectionMode` | `<gtkmm/enums.h>` | `Gtk` |
| `Gtk::PositionType` | `<gtkmm/enums.h>` | `Gtk` |
| `Gtk::make_managed` | `<gtkmm/object.h>` | `Gtk` |
| `Pango::EllipsizeMode` | `<pangomm/layout.h>` | `Pango` |
| `Pango::Layout` | `<pangomm/layout.h>` | `Pango` |
| `sigc::signal` | `<sigc++/signal.h>` | `sigc` |
| `sigc::connection` | `<sigc++/connection.h>` | `sigc` |
| `sigc::scoped_connection` | `<sigc++/scoped_connection.h>` | `sigc` |
| `sigc::mem_fun` | `<sigc++/functors/mem_fun.h>` | `sigc` |
| `sigc::slot` | `<sigc++/functors/slot.h>` | `sigc` |
| `Gtk::EditableLabel` | `<gtkmm/editablelabel.h>` | `Gtk` |
| `Gtk::ProgressBar` | `<gtkmm/progressbar.h>` | `Gtk` |
| `Gtk::FlowBox` | `<gtkmm/flowbox.h>` | `Gtk` |
| `Gtk::DropDown` | `<gtkmm/dropdown.h>` | `Gtk` |
| `Gtk::Scale` | `<gtkmm/scale.h>` | `Gtk` |
| `Gtk::TreeView` | `<gtkmm/treeview.h>` | `Gtk` |
| `Gtk::TreeStore` | `<gtkmm/treestore.h>` | `Gtk` |
| `Gtk::TreeModel` | `<gtkmm/treemodel.h>` | `Gtk` |
| `Gtk::TreeModelColumn` | `<gtkmm/treemodelcolumn.h>` | `Gtk` |
| `Gtk::Paned` | `<gtkmm/paned.h>` | `Gtk` |
| `Gtk::Separator` | `<gtkmm/separator.h>` | `Gtk` |
| `Gtk::FileDialog` | `<gtkmm/filedialog.h>` | `Gtk` |
| `Gtk::ListView` | `<gtkmm/listview.h>` | `Gtk` |
| `Gtk::StringList` | `<gtkmm/stringlist.h>` | `Gtk` |
| `Gtk::TreeListModel` | `<gtkmm/treelistmodel.h>` | `Gtk` |
| `Gio::ListStore` | `<giomm/liststore.h>` | `Gio` |
| `Gio::ListModel` | `<giomm/listmodel.h>` | `Gio` |
| `Gio::SimpleAction` | `<giomm/simpleaction.h>` | `Gio` |
| `Gio::SimpleActionGroup` | `<giomm/simpleactiongroup.h>` | `Gio` |
| `Gio::MemoryInputStream` | `<giomm/memoryinputstream.h>` | `Gio` |
| `Gdk::RGBA` | `<gdkmm/rgba.h>` | `Gdk` |
| `Gdk::Rectangle` | `<gdkmm/rectangle.h>` | `Gdk` |
| `Gdk::Texture` | `<gdkmm/texture.h>` | `Gdk` |
| `::GskPath` | `<gsk/gskpath.h>` (C header) | N/A |
| `::GskStroke` | `<gsk/gskstroke.h>` (C header) | N/A |

## GDK / GTK C-level Constants

These are C macros/constants that clang-tidy may not be able to resolve through umbrella headers. Use the granular C headers directly, or suppress with NOLINT when they come from `extern "C"` blocks.

| Type | Header | Notes |
| :--- | :--- | :--- |
| `GDK_KEY_*` (GDK_KEY_Up, GDK_KEY_Escape, etc.) | `<gdk/gdkkeysyms.h>` | C header |
| `GDK_BUTTON_PRIMARY` / `GDK_BUTTON_SECONDARY` | `<gdk/gdk.h>` | C header, or use umbrella |
| `GTK_STYLE_PROVIDER_PRIORITY_USER` | `<gtk/gtkstyleprovider.h>` | C header |
| `GTK_INVALID_LIST_POSITION` | `<gtk/gtk.h>` | C macro, NOLINT recommended |
| `gssize` / `guint` / `gpointer` | `<glib.h>` | C typedefs, NOLINT recommended |

## CLI & Configuration

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `CLI::App` | `<CLI/App.hpp>` | `CLI` |
| `CLI::Option` | `<CLI/Option.hpp>` | `CLI` |
| `CLI::ParseError` | `<CLI/Error.hpp>` | `CLI` |
| `CLI::CheckedTransformer` | `<CLI/ExtraValidators.hpp>` | `CLI` |
| `YAML::Node` | `<yaml-cpp/node/node.h>` | `YAML` |
| `YAML::Emitter` | `<yaml-cpp/emitter.h>` | `YAML` |
| `YAML::LoadFile` | `<yaml-cpp/node/parse.h>` | `YAML` |
| `YAML::Key` / `YAML::Value` / `YAML::BeginMap` / `YAML::EndMap` / `YAML::BeginSeq` / `YAML::EndSeq` | `<yaml-cpp/emittermanip.h>` | `YAML` |
| `YAML::Exception` | `<yaml-cpp/exceptions.h>` | `YAML` |

## Logging & Diagnostics

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `spdlog::logger` | `<spdlog/logger.h>` | `spdlog` |
| `spdlog::async_logger` | `<spdlog/async_logger.h>` | `spdlog` |
| `spdlog::async_overflow_policy` | `<spdlog/async_logger.h>` | `spdlog` |
| `spdlog::level::level_enum` | `<spdlog/common.h>` | `spdlog` |
| `spdlog::level::trace` | `<spdlog/common.h>` | `spdlog` |
| `spdlog::sink_ptr` | `<spdlog/common.h>` | `spdlog` |
| `spdlog::info` | `<spdlog/spdlog.h>` | `spdlog` |
| `spdlog::drop` | `<spdlog/spdlog.h>` | `spdlog` |
| `spdlog::register_logger` | `<spdlog/spdlog.h>` | `spdlog` |
| `spdlog::set_default_logger` | `<spdlog/spdlog.h>` | `spdlog` |
| `spdlog::shutdown` | `<spdlog/spdlog.h>` | `spdlog` |
| `spdlog::init_thread_pool` | `<spdlog/async.h>` | `spdlog` |
| `spdlog::thread_pool` | `<spdlog/async.h>` | `spdlog` |

## Boost

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `boost::interprocess::file_mapping` | `<boost/interprocess/file_mapping.hpp>` | `boost::interprocess` |
| `boost::interprocess::mapped_region` | `<boost/interprocess/mapped_region.hpp>` | `boost::interprocess` |
| `boost::interprocess::mode_t` | `<boost/interprocess/detail/os_file_functions.hpp>` | `boost::interprocess` |
| `boost::interprocess::read_only` | `<boost/interprocess/detail/os_file_functions.hpp>` | `boost::interprocess` |
| `boost::interprocess::read_write` | `<boost/interprocess/detail/os_file_functions.hpp>` | `boost::interprocess` |
| `boost::endian::endian_reverse` | `<boost/endian/detail/endian_reverse.hpp>` | `boost::endian` |
| `boost::algorithm::trim_copy_if` | `<boost/algorithm/string/trim.hpp>` | `boost::algorithm` |
| `boost::algorithm::is_space` | `<boost/algorithm/string/classification.hpp>` | `boost::algorithm` |

## Database

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `MDB_env` | `<lmdb.h>` | N/A |
| `MDB_txn` | `<lmdb.h>` | N/A |
| `MDB_dbi` | `<lmdb.h>` | N/A |
| `MDB_cursor` | `<lmdb.h>` | N/A |
| `mdb_env_create` | `<lmdb.h>` | N/A |
| `mdb_env_set_mapsize` | `<lmdb.h>` | N/A |
| `mdb_env_set_maxdbs` | `<lmdb.h>` | N/A |
| `mdb_env_set_maxreaders` | `<lmdb.h>` | N/A |
| `mdb_env_open` | `<lmdb.h>` | N/A |
| `mdb_txn_begin` | `<lmdb.h>` | N/A |
| `mdb_txn_env` | `<lmdb.h>` | N/A |
| `mdb_txn_commit` | `<lmdb.h>` | N/A |
| `MDB_RDONLY` | `<lmdb.h>` | N/A |

## Audio & Multimedia

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `FLAC__StreamDecoder` | `<FLAC/stream_decoder.h>` | N/A |
| `FLAC__StreamMetadata` / `FLAC__Frame` / `FLAC__FrameHeader` | `<FLAC/format.h>` | N/A |
| `FLAC__int32` / `FLAC__uint32` / `FLAC__uint64` / `FLAC__bool` | `<FLAC/ordinals.h>` | N/A |
| `ALACDecoder` | `<alac/ALACDecoder.h>` | N/A |
| `pw_context` | `<pipewire/context.h>` | N/A |
| `pw_core` | `<pipewire/core.h>` | N/A |
| `pw_thread_loop` | `<pipewire/thread-loop.h>` | N/A |
| `pw_stream` | `<pipewire/stream.h>` | N/A |
| `pw_stream_state` | `<pipewire/stream.h>` | N/A |
| `spa_audio_info_raw` | `<spa/param/audio/raw.h>` | N/A |
| `spa_format_audio_raw_parse` | `<spa/param/audio/raw-utils.h>` | N/A |
| `spa_pod` | `<spa/pod/pod.h>` | N/A |
| `spa_pod_frame` / `spa_pod_is_*` / `spa_pod_get_*` | `<spa/pod/body.h>` | N/A |
| `spa_pod_builder` | `<spa/pod/builder.h>` | N/A |
| `spa_pod_iterator` / `spa_pod_foreach` | `<spa/pod/iter.h>` | N/A |
| `SPA_POD_*` constructor macros | `<spa/pod/vararg.h>` | N/A |
| `SPA_TYPE_*` constants | `<spa/utils/type.h>` | N/A |
| `SPA_PARAM_*` constants | `<spa/param/param.h>` | N/A |
| `SPA_KEY_*` constants | `<spa/utils/keys.h>` | N/A |
| `spa_dict` | `<spa/utils/dict.h>` | N/A |
| `pw_proxy` | `<pipewire/proxy.h>` | N/A |
| `pw_link_info` / `PW_LINK_INFO_EVENT_*` | `<pipewire/link.h>` | N/A |
| `pw_node_info` / `PW_NODE_INFO_EVENT_*` | `<pipewire/node.h>` | N/A |
| `pw_loop` | `<pipewire/loop.h>` | N/A |
| `snd_pcm_t` | `<alsa/pcm.h>` | N/A |
| `snd_ctl_t` | `<alsa/control.h>` | N/A |
| `snd_pcm_hw_params_t` | `<alsa/pcm.h>` | N/A |

## System & Utilities

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `udev` | `<libudev.h>` | N/A |
| `udev_monitor` | `<libudev.h>` | N/A |
| `mi_malloc` | `<mimalloc.h>` | N/A |

## Testing

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `Catch::Approx` | `<catch2/catch_approx.hpp>` | `Catch` |
| `Catch::Matchers` | `<catch2/matchers/catch_matchers_all.hpp>` | `Catch` |
| `CHECK_THAT` | `<catch2/matchers/catch_matchers.hpp>` | N/A (macro) |
| `WARN` | `<catch2/catch_message.hpp>` | N/A (macro) |
| `GENERATE` | `<catch2/generators/catch_generators.hpp>` | N/A (macro) |

## Standard Library (STL)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `std::string` | `<string>` | `std` |
| `std::string_view` | `<string_view>` | `std` |
| `std::vector` | `<vector>` | `std` |
| `std::map` | `<map>` | `std` |
| `std::set` | `<set>` | `std` |
| `std::unordered_map` | `<unordered_map>` | `std` |
| `std::unordered_set` | `<unordered_set>` | `std` |
| `std::optional` | `<optional>` | `std` |
| `std::nullopt` | `<optional>` | `std` |
| `std::expected` | `<expected>` | `std` |
| `std::unexpected` | `<expected>` | `std` |
| `std::unique_ptr` | `<memory>` | `std` |
| `std::shared_ptr` | `<memory>` | `std` |
| `std::make_unique` | `<memory>` | `std` |
| `std::make_shared` | `<memory>` | `std` |
| `std::span` | `<span>` | `std` |
| `std::byte` | `<cstddef>` | `std` |
| `std::size_t` | `<cstddef>` | `std` |
| `std::uint8_t` | `<cstdint>` | `std` |
| `std::uint16_t` | `<cstdint>` | `std` |
| `std::uint32_t` | `<cstdint>` | `std` |
| `std::uint64_t` | `<cstdint>` | `std` |
| `std::int32_t` | `<cstdint>` | `std` |
| `std::move` | `<utility>` | `std` |
| `std::forward` | `<utility>` | `std` |
| `std::swap` | `<utility>` | `std` |
| `std::pair` | `<utility>` | `std` |
| `std::move_only_function` | `<functional>` | `std` |
| `std::function` | `<functional>` | `std` |
| `std::jthread` | `<thread>` | `std` |
| `std::mutex` | `<mutex>` | `std` |
| `std::deque` | `<deque>` | `std` |
| `std::list` | `<list>` | `std` |
| `std::ignore` | `<tuple>` | `std` |
| `std::format` | `<format>` | `std` |
| `std::filesystem::path` | `<filesystem>` | `std` |
| `std::filesystem::file_size` | `<filesystem>` | `std` |
| `std::filesystem::last_write_time` | `<filesystem>` | `std` |
| `std::exception` | `<exception>` | `std` |
| `std::errc` | `<system_error>` | `std` |
| `std::from_chars` | `<charconv>` | `std` |
| `std::less` | `<functional>` | `std` |
| `std::plus` | `<functional>` | `std` |
| `std::dynamic_pointer_cast` | `<memory>` | `std` |
| `std::numbers::pi` | `<numbers>` | `std` |
| `std::operator""ms` / `std::chrono_literals` | `<chrono>` | `std` |
| `std::ostream` | `<ostream>` | `std` |
| `std::hex` / `std::dec` | `<ios>` | `std` |
| `std::tolower` | `<cctype>` | `std` |
| `std::cout` | `<iostream>` | `std` |
| `std::ranges::transform` | `<algorithm>` | `std::ranges` |
| `std::ranges::sort` | `<algorithm>` | `std::ranges` |
| `std::ranges::find_if` | `<algorithm>` | `std::ranges` |
| `std::ranges::find` | `<algorithm>` | `std::ranges` |
| `std::ranges::fold_left` | `<algorithm>` | `std::ranges` |
| `std::ranges::to` | `<algorithm>` | `std::ranges` |
| `std::ranges::views::enumerate` | `<ranges>` | `std::ranges::views` |
| `std::ranges::views::iota` | `<ranges>` | `std::ranges::views` |
| `std::ranges::views::filter` | `<ranges>` | `std::ranges::views` |
| `std::ranges::views::transform` | `<ranges>` | `std::ranges::views` |
| `std::visit` | `<variant>` | `std` |
| `std::monostate` | `<variant>` | `std` |
| `std::decay_t` / `std::is_same_v` / `std::is_constructible_v` | `<type_traits>` | `std` |
| `std::begin` / `std::end` / `std::next` / `std::distance` | `<iterator>` | `std` |
| `std::stop_token` / `std::stop_source` | `<stop_token>` | `std` |
| `std::flat_set` | `<flat_set>` | `std` |
| `std::sorted_unique` | `<ranges>` | `std` |

> **Note on `std::sorted_unique`:** This is a C++23 tag type in `<ranges>`, but clang-tidy's include-cleaner may false-positive flag it even when `<algorithm>` is included. Use `// NOLINTNEXTLINE(misc-include-cleaner)` at the usage site.
>
> **Note on C headers with Linux-specific extensions:** `ESTRPIPE` (Linux errno 86) is only available via `<errno.h>`, not `<cerrno>`. `pollfd`, `POLLIN`, `poll`, `nfds_t` are only available via `<poll.h>`. These C headers may cause false positives from clang-tidy's `misc-include-cleaner`. Suppress with `NOLINTBEGIN(misc-include-cleaner)` / `NOLINTEND(misc-include-cleaner)` around the affected block.
>
> **Note on GLib C typedefs:** `gssize`, `guint`, and `gpointer` are C typedefs from `<glib.h>`. clang-tidy's include-cleaner often cannot resolve symbols defined inside `extern "C"` blocks in C headers. When these types are the ONLY reason for including a C header, suppress with `// NOLINT(misc-include-cleaner)` at each usage site, or keep the umbrella `<glib.h>` with a NOLINT on the include.
>
> **Note on GTK/GDK C macros:** `GTK_INVALID_LIST_POSITION` (from `<gtk/gtk.h>`) and `GDK_BUTTON_SECONDARY` (from `<gdk/gdk.h>`) are C macros. If the granular C header (e.g., `<gtk/gtkenums.h>` or `<gdk/gdktypes.h>`) doesn't resolve the warning, suppress with NOLINT — these are the same `extern "C"` resolution limitation.
>
> **Note on incomplete type false positives:** clang-tidy may flag `<gtkmm/adjustment.h>` as unused even when `get_adjustment()->get_upper()` is called, because the type `Gtk::Adjustment` is accessed through a `Glib::RefPtr` indirection. Similarly, headers providing YAML template specializations (like `YAML::convert<LayoutValue>`) may be flagged as unused because the template is only implicitly instantiated. If removing such a header causes a compile error, restore it and add `// NOLINT(misc-include-cleaner)`.
>
> **Note on UDL (user-defined literal) operators:** clang-tidy may fail to associate `std::operator""ms` with `<chrono>` when the literal operator is brought into scope via `using namespace std::chrono_literals`. If `<chrono>` is flagged as unused but `10ms` style literals are used, add NOLINT at the include or usage site.

## Aobus Internal Types

### Core (`ao::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::TrackId` | `<ao/Type.h>` | `ao` |
| `ao::ListId` | `<ao/Type.h>` | `ao` |
| `ao::ResourceId` | `<ao/Type.h>` | `ao` |
| `ao::DictionaryId` | `<ao/Type.h>` | `ao` |
| `ao::Exception` | `<ao/Exception.h>` | `ao` |
| `ao::Error` | `<ao/Error.h>` | `ao` |
| `ao::Result` | `<ao/Error.h>` | `ao` |
| `ao::IMainThreadDispatcher` | `<ao/utility/IMainThreadDispatcher.h>` | `ao` |

### Library (`ao::library::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::library::MusicLibrary` | `<ao/library/MusicLibrary.h>` | `ao::library` |
| `ao::library::TrackStore` | `<ao/library/TrackStore.h>` | `ao::library` |
| `ao::library::TrackView` | `<ao/library/TrackView.h>` | `ao::library` |
| `ao::library::ListView` | `<ao/library/ListView.h>` | `ao::library` |
| `ao::library::ListStore` | `<ao/library/ListStore.h>` | `ao::library` |
| `ao::library::TrackBuilder` | `<ao/library/TrackBuilder.h>` | `ao::library` |
| `ao::library::ListBuilder` | `<ao/library/ListBuilder.h>` | `ao::library` |
| `ao::library::DictionaryStore` | `<ao/library/DictionaryStore.h>` | `ao::library` |
| `ao::library::ResourceStore` | `<ao/library/ResourceStore.h>` | `ao::library` |
| `ao::library::MetaStore` | `<ao/library/MetaStore.h>` | `ao::library` |
| `ao::library::MetaHeader` | `<ao/library/Meta.h>` | `ao::library` |
| `ao::library::TrackHotHeader` | `<ao/library/TrackLayout.h>` | `ao::library` |
| `ao::library::TrackColdHeader` | `<ao/library/TrackLayout.h>` | `ao::library` |
| `ao::library::ListHeader` | `<ao/library/ListLayout.h>` | `ao::library` |
| `ao::library::Exporter` | `<ao/library/Exporter.h>` | `ao::library` |
| `ao::library::ExportMode` | `<ao/library/Exporter.h>` | `ao::library` |
| `ao::library::Importer` | `<ao/library/Importer.h>` | `ao::library` |
| `ao::library::ImportWorker` | `<ao/library/ImportWorker.h>` | `ao::library` |

### Audio (`ao::audio::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::audio::Format` | `<ao/audio/Format.h>` | `ao::audio` |
| `ao::audio::BackendId` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::ProfileId` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::DeviceId` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::Device` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::DeviceCapabilities` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::SampleFormatCapability` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::Quality` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::RouteAnchor` | `<ao/audio/Backend.h>` | `ao::audio` |
| `ao::audio::Transport` | `<ao/audio/Types.h>` | `ao::audio` |
| `ao::audio::Sample` | `<ao/audio/Types.h>` | `ao::audio` |
| `ao::audio::TrackPlaybackDescriptor` | `<ao/audio/Types.h>` | `ao::audio` |
| `ao::audio::PcmBlock` | `<ao/audio/DecoderTypes.h>` | `ao::audio` |
| `ao::audio::DecodedStreamInfo` | `<ao/audio/DecoderTypes.h>` | `ao::audio` |
| `ao::audio::Player` | `<ao/audio/Player.h>` | `ao::audio` |
| `ao::audio::Player::Status` | `<ao/audio/Player.h>` | `ao::audio` |
| `ao::audio::IBackend` | `<ao/audio/IBackend.h>` | `ao::audio` |
| `ao::audio::IBackendProvider` | `<ao/audio/IBackendProvider.h>` | `ao::audio` |
| `ao::audio::IDecoderSession` | `<ao/audio/IDecoderSession.h>` | `ao::audio` |
| `ao::audio::IRenderTarget` | `<ao/audio/IRenderTarget.h>` | `ao::audio` |
| `ao::audio::ISource` | `<ao/audio/ISource.h>` | `ao::audio` |
| `ao::audio::Subscription` | `<ao/audio/Subscription.h>` | `ao::audio` |
| `ao::audio::PropertyId` | `<ao/audio/Property.h>` | `ao::audio` |
| `ao::audio::PropertyValue` | `<ao/audio/Property.h>` | `ao::audio` |
| `ao::audio::PropertyInfo` | `<ao/audio/Property.h>` | `ao::audio` |
| `ao::audio::NullBackend` | `<ao/audio/NullBackend.h>` | `ao::audio` |
| `ao::audio::PcmRingBuffer` | `<ao/audio/PcmRingBuffer.h>` | `ao::audio` |
| `ao::audio::PcmConverter` | `<ao/audio/PcmConverter.h>` | `ao::audio` |
| `ao::audio::MemorySource` | `<ao/audio/MemorySource.h>` | `ao::audio` |
| `ao::audio::StreamingSource` | `<ao/audio/StreamingSource.h>` | `ao::audio` |
| `ao::audio::FormatNegotiator` | `<ao/audio/FormatNegotiator.h>` | `ao::audio` |
| `ao::audio::RenderPlan` | `<ao/audio/FormatNegotiator.h>` | `ao::audio` |
| `ao::audio::QualityResult` | `<ao/audio/QualityAnalyzer.h>` | `ao::audio` |
| `ao::audio::analyzeAudioQuality` | `<ao/audio/QualityAnalyzer.h>` | `ao::audio` |
| `ao::audio::flow::Graph` | `<ao/audio/flow/Graph.h>` | `ao::audio::flow` |
| `ao::audio::flow::Node` | `<ao/audio/flow/Graph.h>` | `ao::audio::flow` |
| `ao::audio::flow::Connection` | `<ao/audio/flow/Graph.h>` | `ao::audio::flow` |
| `ao::audio::flow::NodeType` | `<ao/audio/flow/Graph.h>` | `ao::audio::flow` |
| `ao::audio::Engine` | `<ao/audio/Engine.h>` | `ao::audio` |
| `ao::audio::Engine::RouteStatus` | `<ao/audio/Engine.h>` | `ao::audio` |

### Model (`ao::model::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::model::ListDraft` | `<ao/model/ListDraft.h>` | `ao::model` |
| `ao::model::ListKind` | `<ao/model/ListDraft.h>` | `ao::model` |

### Query (`ao::query::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::query::Expression` | `<ao/query/Expression.h>` | `ao::query` |
| `ao::query::parse` | `<ao/query/Parser.h>` | `ao::query` |
| `ao::query::serialize` | `<ao/query/Serializer.h>` | `ao::query` |
| `ao::query::ExecutionPlan` | `<ao/query/ExecutionPlan.h>` | `ao::query` |
| `ao::query::PlanEvaluator` | `<ao/query/PlanEvaluator.h>` | `ao::query` |

### Runtime (`ao::rt::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::rt::AppSession` | `<runtime/AppSession.h>` | `ao::rt` |
| `ao::rt::ViewService` | `<runtime/ViewService.h>` | `ao::rt` |
| `ao::rt::ViewId` | `<runtime/CorePrimitives.h>` | `ao::rt` |
| `ao::rt::NotificationId` | `<runtime/CorePrimitives.h>` | `ao::rt` |
| `ao::rt::Range` | `<runtime/CorePrimitives.h>` | `ao::rt` |
| `ao::rt::Subscription` | `<runtime/CorePrimitives.h>` | `ao::rt` |
| `ao::rt::IControlExecutor` | `<runtime/CorePrimitives.h>` | `ao::rt` |
| `ao::rt::Signal` | `<runtime/CorePrimitives.h>` | `ao::rt` |
| `ao::rt::PlaybackService` | `<runtime/PlaybackService.h>` | `ao::rt` |
| `ao::rt::TrackSource` | `<runtime/TrackSource.h>` | `ao::rt` |
| `ao::rt::ConfigStore` | `<runtime/ConfigStore.h>` | `ao::rt` |
| `ao::rt::WorkspaceService` | `<runtime/WorkspaceService.h>` | `ao::rt` |
| `ao::rt::LibraryMutationService` | `<runtime/LibraryMutationService.h>` | `ao::rt` |
| `ao::rt::NotificationService` | `<runtime/NotificationService.h>` | `ao::rt` |
| `ao::rt::SmartListEvaluator` | `<runtime/SmartListEvaluator.h>` | `ao::rt` |
| `ao::rt::SmartListSource` | `<runtime/SmartListSource.h>` | `ao::rt` |
| `ao::rt::PlaybackState` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::OutputSelection` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::OutputBackendSnapshot` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::LayoutState` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::NotificationSeverity` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::NotificationEntry` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::NotificationFeedState` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackGroupKey` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackSortField` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackSortTerm` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackPresentationField` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackListPresentationState` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackListViewState` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::ViewRecord` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::SessionSnapshot` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::MetadataPatch` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::UpdateTrackMetadataReply` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::EditTrackTagsReply` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::ImportFilesReply` | `<runtime/StateTypes.h>` | `ao::rt` |
| `ao::rt::TrackPresentationSpec` | `<runtime/TrackPresentationPreset.h>` | `ao::rt` |
| `ao::rt::TrackPresentationPreset` | `<runtime/TrackPresentationPreset.h>` | `ao::rt` |
| `ao::rt::ITrackListProjection` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::TrackListPresentationSnapshot` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::FilterStatusChanged` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::TrackListProjectionDeltaBatch` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::ITrackDetailProjection` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::TrackDetailSnapshot` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::AudioPropertySnapshot` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::SelectionKind` | `<runtime/ProjectionTypes.h>` | `ao::rt` |
| `ao::rt::TrackListProjection` | `<runtime/TrackListProjection.h>` | `ao::rt` |
| `ao::rt::TrackDetailProjection` | `<runtime/TrackDetailProjection.h>` | `ao::rt` |

### Media (`ao::media::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::media::flac::MetadataBlockLayout` | `<ao/media/flac/MetadataBlockLayout.h>` | `ao::media::flac` |
| `ao::media::flac::MetadataBlockType` | `<ao/media/flac/MetadataBlockLayout.h>` | `ao::media::flac` |
| `ao::media::mp4::AtomLayout` | `<ao/media/mp4/AtomLayout.h>` | `ao::media::mp4` |

### Tag (`ao::tag::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::tag::TagFile` | `<ao/tag/TagFile.h>` | `ao::tag` |
| `ao::tag::mpeg::VersionID` | `<ao/tag/mpeg/FrameLayout.h>` | `ao::tag::mpeg` |
| `ao::tag::mpeg::LayerDescription` | `<ao/tag/mpeg/FrameLayout.h>` | `ao::tag::mpeg` |
| `ao::tag::mpeg::ChannelMode` | `<ao/tag/mpeg/FrameLayout.h>` | `ao::tag::mpeg` |
| `ao::tag::mpeg::FrameLayout` | `<ao/tag/mpeg/FrameLayout.h>` | `ao::tag::mpeg` |

### LMDB (`ao::lmdb::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::lmdb::Environment` | `<ao/lmdb/Environment.h>` | `ao::lmdb` |
| `ao::lmdb::Transaction` | `<ao/lmdb/Transaction.h>` | `ao::lmdb` |
| `ao::lmdb::ReadTransaction` | `<ao/lmdb/Transaction.h>` | `ao::lmdb` |
| `ao::lmdb::WriteTransaction` | `<ao/lmdb/Transaction.h>` | `ao::lmdb` |

### Utility (`ao::utility::`)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::utility::ScopedTimer` | `<ao/utility/ScopedTimer.h>` | `ao::utility` |
| `ao::utility::bytes::view` | `<ao/utility/ByteView.h>` | `ao::utility::bytes` |
| `ao::utility::bytes::stringView` | `<ao/utility/ByteView.h>` | `ao::utility::bytes` |
| `ao::utility::layout::view` | `<ao/utility/ByteView.h>` | `ao::utility::layout` |
| `ao::utility::layout::viewArray` | `<ao/utility/ByteView.h>` | `ao::utility::layout` |
| `ao::utility::layout::asPtr` | `<ao/utility/ByteView.h>` | `ao::utility::layout` |
| `ao::utility::layout::viewAt` | `<ao/utility/ByteView.h>` | `ao::utility::layout` |
| `ao::utility::unsafeDowncast` | `<ao/utility/ByteView.h>` | `ao::utility` |
| `ao::utility::makeUniquePtr` | `<ao/utility/UniquePtr.h>` | `ao::utility` |

### GTK/App (`ao::gtk::`)

#### App Shell

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::MainWindow` | `"app/MainWindow.h"` | `ao::gtk` |
| `ao::gtk::WindowController` | `"app/WindowController.h"` | `ao::gtk` |
| `ao::gtk::MenuController` | `"app/MenuController.h"` | `ao::gtk` |
| `ao::gtk::ShellLayoutController` | `"app/ShellLayoutController.h"` | `ao::gtk` |
| `ao::gtk::GtkControlExecutor` | `"app/GtkControlExecutor.h"` | `ao::gtk` |
| `ao::gtk::AobusSoul` | `"app/AobusSoul.h"` | `ao::gtk` |
| `ao::gtk::ThemeBus` / `signalThemeRefresh` / `emitThemeRefresh` | `"app/ThemeBus.h"` | `ao::gtk` |
| `ao::gtk::WindowState` | `"app/UIState.h"` | `ao::gtk` |
| `ao::gtk::TrackViewState` | `"app/UIState.h"` | `ao::gtk` |
| `ao::gtk::CustomTrackPresentationState` | `"app/UIState.h"` | `ao::gtk` |
| `ao::gtk::TrackPresentationStoreState` | `"app/UIState.h"` | `ao::gtk` |

#### Playback

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::AobusSoulBinding` | `"playback/AobusSoulBinding.h"` | `ao::gtk` |
| `ao::gtk::AobusSoulWindow` | `"playback/AobusSoulWindow.h"` | `ao::gtk` |
| `ao::gtk::OutputSelector` | `"playback/OutputSelector.h"` | `ao::gtk` |
| `ao::gtk::OutputListItems` / `BackendItem` / `DeviceItem` | `"playback/OutputListItems.h"` | `ao::gtk` |
| `ao::gtk::PlaybackDetailsWidget` | `"playback/PlaybackDetailsWidget.h"` | `ao::gtk` |
| `ao::gtk::PlaybackSequenceController` | `"playback/PlaybackSequenceController.h"` | `ao::gtk` |
| `ao::gtk::ActivePlaybackSequence` | `"playback/PlaybackSequenceController.h"` | `ao::gtk` |
| `ao::gtk::SeekControl` | `"playback/SeekControl.h"` | `ao::gtk` |
| `ao::gtk::TimeLabel` | `"playback/TimeLabel.h"` | `ao::gtk` |
| `ao::gtk::TransportButton` | `"playback/TransportButton.h"` | `ao::gtk` |
| `ao::gtk::VolumeBar` | `"playback/VolumeBar.h"` | `ao::gtk` |
| `ao::gtk::VolumeControl` | `"playback/VolumeControl.h"` | `ao::gtk` |
| `ao::gtk::NowPlayingFieldLabel` | `"playback/NowPlayingFieldLabel.h"` | `ao::gtk` |
| `ao::gtk::NowPlayingStatusLabel` | `"playback/NowPlayingStatusLabel.h"` | `ao::gtk` |
| `ao::gtk::PlaybackPositionInterpolator` | `"playback/PlaybackPositionInterpolator.h"` | `ao::gtk` |

#### Track

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::TrackRowCache` | `"track/TrackRowCache.h"` | `ao::gtk` |
| `ao::gtk::TrackRowObject` | `"track/TrackRowObject.h"` | `ao::gtk` |
| `ao::gtk::TrackListAdapter` | `"track/TrackListAdapter.h"` | `ao::gtk` |
| `ao::gtk::TrackFilterMode` | `"track/TrackListAdapter.h"` | `ao::gtk` |
| `ao::gtk::ProjectionTrackModel` | `"track/TrackListModel.h"` | `ao::gtk` |
| `ao::gtk::TrackViewPage` | `"track/TrackViewPage.h"` | `ao::gtk` |
| `ao::gtk::TrackPageManager` | `"track/TrackPageManager.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnViewHost` | `"track/TrackColumnViewHost.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnController` | `"track/TrackColumnController.h"` | `ao::gtk` |
| `ao::gtk::TrackSelectionController` | `"track/TrackSelectionController.h"` | `ao::gtk` |
| `ao::gtk::TrackFilterController` | `"track/TrackFilterController.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnLayoutModel` | `"track/TrackPresentation.h"` | `ao::gtk` |
| `ao::gtk::TrackColumn` | `"track/TrackPresentation.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnDefinition` | `"track/TrackPresentation.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnLayout` | `"track/TrackPresentation.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnState` | `"track/TrackPresentation.h"` | `ao::gtk` |
| `ao::gtk::TrackPresentationStore` | `"track/TrackPresentationStore.h"` | `ao::gtk` |
| `ao::gtk::TrackColumnFactoryBuilder` / `buildColumnFactory` | `"track/TrackColumnFactoryBuilder.h"` | `ao::gtk` |
| `ao::gtk::ColumnVisibilityModel` | `"track/ColumnVisibilityModel.h"` | `ao::gtk` |
| `ao::gtk::SelectionInfoLabel` | `"track/SelectionInfoLabel.h"` | `ao::gtk` |
| `ao::gtk::StatusNotificationLabel` | `"track/StatusNotificationLabel.h"` | `ao::gtk` |
| `ao::gtk::LibraryTrackCountLabel` | `"track/LibraryTrackCountLabel.h"` | `ao::gtk` |
| `ao::gtk::TrackCustomViewDialog` | `"track/TrackCustomViewDialog.h"` | `ao::gtk` |

#### List / Sidebar

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::ListSidebarController` | `"list/ListSidebarController.h"` | `ao::gtk` |
| `ao::gtk::ListSidebarPanel` | `"list/ListSidebarPanel.h"` | `ao::gtk` |
| `ao::gtk::ListTreeItem` | `"list/ListTreeItem.h"` | `ao::gtk` |
| `ao::gtk::ListRowObject` | `"list/ListRowObject.h"` | `ao::gtk` |
| `ao::gtk::ListTreeModelBuilder` | `"list/ListTreeModelBuilder.h"` | `ao::gtk` |
| `ao::gtk::SmartListDialog` | `"list/SmartListDialog.h"` | `ao::gtk` |
| `ao::gtk::QueryExpressionBox` | `"list/QueryExpressionBox.h"` | `ao::gtk` |

#### Inspector / Cover Art

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::TrackInspectorPanel` | `"inspector/TrackInspectorPanel.h"` | `ao::gtk` |
| `ao::gtk::CoverArtWidget` | `"inspector/CoverArtWidget.h"` | `ao::gtk` |
| `ao::gtk::CoverArtCache` | `"inspector/CoverArtCache.h"` | `ao::gtk` |

#### Tag Editing

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::TagEditController` | `"tag/TagEditController.h"` | `ao::gtk` |
| `ao::gtk::TagEditor` | `"tag/TagEditor.h"` | `ao::gtk` |
| `ao::gtk::TagPopover` | `"tag/TagPopover.h"` | `ao::gtk` |

#### Library I/O

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::ImportExportCoordinator` | `"library_io/ImportExportCoordinator.h"` | `ao::gtk` |
| `ao::gtk::ImportExportCallbacks` | `"library_io/ImportExportCoordinator.h"` | `ao::gtk` |
| `ao::gtk::ImportProgressDialog` | `"library_io/ImportProgressDialog.h"` | `ao::gtk` |
| `ao::gtk::ImportProgressIndicator` | `"library_io/ImportProgressIndicator.h"` | `ao::gtk` |
| `ao::gtk::PlaylistExporter` | `"library_io/PlaylistExporter.h"` | `ao::gtk` |

#### Layout System

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `ao::gtk::layout::LayoutDocument` | `"layout/document/LayoutDocument.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::LayoutNode` | `"layout/document/LayoutNode.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::LayoutValue` | `"layout/document/LayoutNode.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::ComponentRegistry` | `"layout/runtime/ComponentRegistry.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::ComponentDescriptor` | `"layout/runtime/ComponentRegistry.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::PropertyDescriptor` | `"layout/runtime/ComponentRegistry.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::ILayoutComponent` | `"layout/runtime/ILayoutComponent.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::LayoutDependencies` | `"layout/runtime/LayoutDependencies.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::LayoutHost` | `"layout/runtime/LayoutHost.h"` | `ao::gtk::layout` |
| `ao::gtk::layout::LayoutRuntime` | `"layout/runtime/LayoutRuntime.h"` | `ao::gtk::layout` |
| YAML::convert specializations for LayoutValue, LayoutNode, LayoutDocument | `"layout/document/LayoutYaml.h"` | `YAML` |
| `ao::gtk::layout::editor::LayoutEditorDialog` | `"layout/editor/LayoutEditorDialog.h"` | `ao::gtk::layout::editor` |
