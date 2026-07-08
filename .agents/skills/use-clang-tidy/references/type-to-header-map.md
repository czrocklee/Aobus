# Third-Party Type Map

This document maps public types from third-party libraries used in Aobus to their exact declaring header files.

## UI / GTK Suite (gtkmm-4.0 / glibmm-2.68)

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `Gtk::Widget` | `<gtkmm/widget.h>` | `Gtk` |
| `Gtk::Window` | `<gtkmm/window.h>` | `Gtk` |
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
| `Glib::VariantContainerBase` | `<glibmm/variant.h>` | `Glib` |
| `Glib::Variant` | `<glibmm/variant.h>` | `Glib` |
| `Glib::KeyFile` | `<glibmm/keyfile.h>` | `Glib` |
| `Glib::Object` | `<glibmm/object.h>` | `Glib` |
| `Glib::Error` | `<glibmm/error.h>` | `Glib` |
| `Glib::Property` | `<glibmm/property.h>` | `Glib` |
| `Glib::PropertyProxy` | `<glibmm/propertyproxy.h>` | `Glib` |
| `Gio::Menu` | `<giomm/menu.h>` | `Gio` |
| `Gio::MenuModel` | `<giomm/menumodel.h>` | `Gio` |
| `Gio::Application` | `<giomm/application.h>` | `Gio` |
| `Gio::File` | `<giomm/file.h>` | `Gio` |
| `Gio::FileMonitor` | `<giomm/filemonitor.h>` | `Gio` |
| `Gio::FileMonitor::Event` | `<giomm/filemonitor.h>` | `Gio` |
| `Gio::DBus::Connection` | `<giomm/dbusconnection.h>` | `Gio` |
| `Gio::Settings` | `<giomm/settings.h>` | `Gio` |
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
| `Gtk::ListItemFactory` | `<gtkmm/listitemfactory.h>` | `Gtk` |
| `Gtk::SignalListItemFactory` | `<gtkmm/signallistitemfactory.h>` | `Gtk` |
| `Gtk::SelectionModel` | `<gtkmm/selectionmodel.h>` | `Gtk` |
| `Gtk::SingleSelection` | `<gtkmm/singleselection.h>` | `Gtk` |
| `Gtk::MultiSelection` | `<gtkmm/multiselection.h>` | `Gtk` |
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
| `Gdk::Surface` | `<gdkmm/surface.h>` | `Gdk` |

> **Note on GSK headers:** Always include the GSK umbrella header `<gsk/gsk.h>` instead of individual C headers like `<gsk/gskpath.h>`. GTK4 requires this, and granular inclusion will trigger compile errors. Use `// NOLINT(misc-include-cleaner)` to suppress clang-tidy warnings about the umbrella header.

## GDK / GTK C-level Constants

These are C macros/constants that clang-tidy may not be able to resolve through umbrella headers. Use the granular C headers directly, or suppress with NOLINT when they come from `extern "C"` blocks.

| Type | Header | Notes |
| :--- | :--- | :--- |
| `GDK_KEY_*` (GDK_KEY_Up, GDK_KEY_Escape, etc.) | `<gdk/gdkkeysyms.h>` | C header |
| `GDK_BUTTON_PRIMARY` / `GDK_BUTTON_SECONDARY` | `<gdk/gdk.h>` | C header, or use umbrella |
| `GTK_STYLE_PROVIDER_PRIORITY_USER` / `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION` | `<gtk/gtkstyleprovider.h>` | C header |
| `GTK_INVALID_LIST_POSITION` | `<gtk/gtk.h>` | C macro, NOLINT recommended |
| `TRUE` / `FALSE` | `<glib/gmacros.h>` | C macros for gboolean |
| `gssize` / `guint` / `gpointer` | `<glib.h>` | C typedefs, NOLINT recommended |
| `::GskPath` / `::GskStroke` | `<gsk/gsk.h>` | Use umbrella header with NOLINT |
| `G_OBJECT_TYPE` | `<glib-object.h>` | GLib C macro, NOLINT recommended |

## CLI & Configuration

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `CLI::App` | `<CLI/App.hpp>` | `CLI` |
| `CLI::Option` | `<CLI/Option.hpp>` | `CLI` |
| `CLI::ParseError` | `<CLI/Error.hpp>` | `CLI` |
| `CLI::CheckedTransformer` | `<CLI/ExtraValidators.hpp>` | `CLI` |
| `ryml::Tree` | `<ryml.hpp>` | `ryml` |
| `ryml::NodeRef` | `<ryml.hpp>` | `ryml` |
| `ryml::ConstNodeRef` | `<ryml.hpp>` | `ryml` |
| `ryml::csubstr` | `<ryml.hpp>` | `ryml` |
| `ryml::substr` | `<ryml.hpp>` | `ryml` |
| `ryml::Callbacks` | `<ryml.hpp>` | `ryml` |

> **Note on RapidYaml (ryml):** Always prefer `<ryml.hpp>` for core tree/node operations. If using STL types (like `std::string`) with ryml, also include `<ryml_std.hpp>`.

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
| `spdlog::source_loc` | `<spdlog/common.h>` | `spdlog` |

## Boost

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `boost::interprocess::file_mapping` | `<boost/interprocess/file_mapping.hpp>` | `boost::interprocess` |
| `boost::interprocess::mapped_region` | `<boost/interprocess/mapped_region.hpp>` | `boost::interprocess` |
| `boost::interprocess::mode_t` | `<boost/interprocess/detail/os_file_functions.hpp>` | `boost::interprocess` |
| `boost::interprocess::read_only` | `<boost/interprocess/detail/os_file_functions.hpp>` | `boost::interprocess` |
| `boost::interprocess::read_write" | `<boost/interprocess/detail/os_file_functions.hpp>` | `boost::interprocess` |
| `boost::endian::endian_reverse` | `<boost/endian/detail/endian_reverse.hpp>` | `boost::endian` |
| `boost::endian::order` | `<boost/endian/detail/order.hpp>` | `boost::endian` |
| `boost::lockfree::spsc_queue` | `<boost/lockfree/spsc_queue.hpp>` | `boost::lockfree` |
| `boost::lockfree::capacity` | `<boost/lockfree/policies.hpp>` | `boost::lockfree` |
| `boost::algorithm::trim_copy_if` | `<boost/algorithm/string/trim.hpp>` | `boost::algorithm` |
| `boost::algorithm::is_space` | `<boost/algorithm/string/classification.hpp>` | `boost::algorithm` |
| `boost::asio::co_spawn` | `<boost/asio/co_spawn.hpp>` | `boost::asio` |
| `boost::pfr::for_each_field` | `<boost/pfr/core.hpp>` | `boost::pfr` |
| `boost::pfr::names_as_array` | `<boost/pfr/core_name.hpp>` | `boost::pfr` |

> **Note on Boost.Asio:** `boost::asio::co_spawn` is provided by `<boost/asio/co_spawn.hpp>`, but clang-tidy's include-cleaner may simultaneously flag the header as "not used directly" and the usage as "no header providing" — a known false positive with Boost's template-heavy headers. Add `// NOLINT(misc-include-cleaner)` at both the include and usage site.

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
| `FLAC__StreamMetadata` | `<FLAC/format.h>` | N/A |
| `FLAC__Frame` | `<FLAC/format.h>` | N/A |
| `FLAC__FrameHeader` | `<FLAC/format.h>` | N/A |
| `FLAC__int32` | `<FLAC/ordinals.h>` | N/A |
| `FLAC__uint32` | `<FLAC/ordinals.h>` | N/A |
| `FLAC__uint64` | `<FLAC/ordinals.h>` | N/A |
| `FLAC__bool` | `<FLAC/ordinals.h>` | N/A |
| `ALACDecoder` | `<alac/ALACDecoder.h>` | N/A |
| `pw_context` | `<pipewire/context.h>` | N/A |
| `pw_core` | `<pipewire/core.h>` | N/A |
| `pw_thread_loop` | `<pipewire/thread-loop.h>` | N/A |
| `pw_stream` | `<pipewire/stream.h>` | N/A |
| `pw_stream_state` | `<pipewire/stream.h>` | N/A |
| `spa_audio_info_raw` | `<spa/param/audio/raw.h>` | N/A |
| `spa_format_audio_raw_parse` | `<spa/param/audio/raw-utils.h>` | N/A |
| `spa_pod` | `<spa/pod/pod.h>` | N/A |
| `spa_pod_frame` | `<spa/pod/body.h>` | N/A |
| `spa_pod_is_*` | `<spa/pod/body.h>` | N/A |
| `spa_pod_get_*` | `<spa/pod/body.h>` | N/A |
| `spa_pod_builder` | `<spa/pod/builder.h>` | N/A |
| `spa_pod_iterator` | `<spa/pod/iter.h>` | N/A |
| `spa_pod_foreach` | `<spa/pod/iter.h>` | N/A |
| `spa_source` | `<spa/support/loop.h>` | N/A |
| `spa_hook` | `<spa/utils/hook.h>` | N/A |
| `SPA_POD_*` | `<spa/pod/vararg.h>` | N/A (macro) |
| `SPA_TYPE_*` | `<spa/utils/type.h>` | N/A (constant) |
| `SPA_PARAM_*` | `<spa/param/param.h>` | N/A (constant) |
| `SPA_KEY_*` | `<spa/utils/keys.h>` | N/A (constant) |
| `spa_dict` | `<spa/utils/dict.h>` | N/A |
| `pw_proxy` | `<pipewire/proxy.h>` | N/A |
| `pw_link_info` | `<pipewire/link.h>` | N/A |
| `PW_LINK_INFO_EVENT_*` | `<pipewire/link.h>` | N/A (constant) |
| `pw_node_info` | `<pipewire/node.h>` | N/A |
| `PW_NODE_INFO_EVENT_*` | `<pipewire/node.h>` | N/A (constant) |
| `pw_loop` | `<pipewire/loop.h>` | N/A |
| `snd_pcm_t` | `<alsa/pcm.h>` | N/A |
| `snd_ctl_t" | `<alsa/control.h>` | N/A |
| `snd_pcm_hw_params_t" | `<alsa/pcm.h>` | N/A |

## Parsing & DSL (lexy)

lexy uses highly granular headers. Map core entry points and common DSL components.

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `lexy::parse` | `<lexy/action/parse.hpp>` | `lexy` |
| `lexy::string_input` | `<lexy/input/string_input.hpp>` | `lexy` |
| `lexy::dsl::*` | `<lexy/dsl/*.hpp>` | `lexy::dsl` |
| `lexy::callback` | `<lexy/callback/adapter.hpp>` | `lexy` |
| `lexy::symbol_table` | `<lexy/dsl/symbol.hpp>` | `lexy` |

> **Note on lexy:** Always refer to the specific DSL component header (e.g., `<lexy/dsl/ascii.hpp>` for `dsl::ascii`) instead of a single umbrella header.

## System & Utilities

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `udev` | `<libudev.h>` | N/A |
| `udev_monitor` | `<libudev.h>` | N/A |
| `mi_malloc` | `<mimalloc.h>` | N/A |
| `gsl::span` | `<gsl-lite/gsl-lite.hpp>` | `gsl` |
| `gsl::not_null` | `<gsl-lite/gsl-lite.hpp>` | `gsl` |
| `gsl_Expects` | `<gsl-lite/gsl-lite.hpp>` | N/A (macro) |
| `gsl_Ensures` | `<gsl-lite/gsl-lite.hpp>` | N/A (macro) |

## Testing

| Type | Header | Namespace |
| :--- | :--- | :--- |
| `TEST_CASE` | `<catch2/catch_test_macros.hpp>` | N/A (macro) |
| `SECTION` | `<catch2/catch_test_macros.hpp>` | N/A (macro) |
| `REQUIRE` | `<catch2/catch_test_macros.hpp>` | N/A (macro) |
| `CHECK` | `<catch2/catch_test_macros.hpp>` | N/A (macro) |
| `STATIC_REQUIRE` | `<catch2/catch_test_macros.hpp>` | N/A (macro) |
| `GENERATE` | `<catch2/generators/catch_generators.hpp>` | N/A (macro) |
| `SUCCEED` | `<catch2/catch_message.hpp>` | N/A (macro) |
| `FAIL` | `<catch2/catch_message.hpp>` | N/A (macro) |
| `INFO` | `<catch2/catch_message.hpp>` | N/A (macro) |
| `WARN` | `<catch2/catch_message.hpp>` | N/A (macro) |
| `Catch::Approx` | `<catch2/catch_approx.hpp>` | `Catch` |
| `Catch::Matchers` | `<catch2/matchers/catch_matchers_all.hpp>` | `Catch` |
| `fakeit::Mock` | `<fakeit.hpp>` | `fakeit` |
| `fakeit::When` | `<fakeit.hpp>` | `fakeit` |
| `fakeit::Verify` | `<fakeit.hpp>` | `fakeit` |
| `fakeit::Fake` | `<fakeit.hpp>` | `fakeit` |

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
| `std::ptrdiff_t` | `<cstddef>` | `std` |
| `std::uint8_t` | `<cstdint>` | `std` |
| `std::uint16_t` | `<cstdint>` | `std` |
| `std::uint32_t" | `<cstdint>` | `std` |
| `std::uint64_t" | `<cstdint>` | `std` |
| `std::int32_t" | `<cstdint>` | `std` |
| `std::move` | `<utility>` | `std` |
| `std::forward` | `<utility>` | `std` |
| `std::swap` | `<utility>` | `std` |
| `std::pair` | `<utility>` | `std` |
| `std::move_only_function" | `<functional>` | `std` |
| `std::function` | `<functional>` | `std` |
| `std::less` | `<functional>` | `std` |
| `std::plus` | `<functional>` | `std` |
| `std::jthread` | `<thread>` | `std` |
| `std::mutex` | `<mutex>` | `std` |
| `std::deque` | `<deque>` | `std` |
| `std::list` | `<list>` | `std` |
| `std::ignore` | `<tuple>` | `std` |
| `std::format` | `<format>` | `std` |
| `std::filesystem::path" | `<filesystem>` | `std` |
| `std::filesystem::file_size" | `<filesystem>` | `std` |
| `std::filesystem::last_write_time" | `<filesystem>` | `std` |
| `std::exception` | `<exception>` | `std` |
| `std::source_location" | `<source_location>` | `std` |
| `std::errc` | `<system_error>` | `std` |
| `std::from_chars" | `<charconv>` | `std` |
| `std::dynamic_pointer_cast" | `<memory>` | `std` |
| `std::numbers::pi" | `<numbers>` | `std` |
| `std::numbers::phi" | `<numbers>` | `std` |
| `std::operator""ms" | `<chrono>` | `std` |
| `std::chrono_literals" | `<chrono>` | `std` |
| `std::ostream` | `<ostream>` | `std` |
| `std::ofstream` | `<fstream>` | `std` |
| `std::hex` | `<ios>` | `std` |
| `std::dec` | `<ios>` | `std` |
| `std::tolower` | `<cctype>` | `std` |
| `std::cout` | `<iostream>` | `std` |
| `std::ranges::distance" | `<iterator>` | `std::ranges` |
| `std::ranges::find` | `<algorithm>` | `std::ranges` |
| `std::ranges::find_if" | `<algorithm>` | `std::ranges` |
| `std::ranges::fold_left" | `<algorithm>` | `std::ranges` |
| `std::ranges::sort` | `<algorithm>` | `std::ranges` |
| `std::ranges::to` | `<algorithm>` | `std::ranges` |
| `std::ranges::transform" | `<algorithm>` | `std::ranges` |
| `std::ranges::views::enumerate" | `<ranges>` | `std::ranges::views` |
| `std::ranges::views::iota" | `<ranges>` | `std::ranges::views` |
| `std::ranges::views::filter" | `<ranges>` | `std::ranges::views` |
| `std::ranges::views::transform" | `<ranges>` | `std::ranges::views` |
| `std::visit` | `<variant>` | `std` |
| `std::monostate` | `<variant>` | `std` |
| `std::decay_t` | `<type_traits>` | `std` |
| `std::is_same_v` | `<type_traits>` | `std` |
| `std::is_constructible_v" | `<type_traits>` | `std` |
| `std::begin` | `<iterator>` | `std` |
| `std::end` | `<iterator>` | `std` |
| `std::next` | `<iterator>` | `std` |
| `std::distance` | `<iterator>` | `std` |
| `std::forward_iterator_tag" | `<iterator>` | `std::forward_iterator_tag` |
| `std::stop_token` | `<stop_token>` | `std` |
| `std::stop_source" | `<stop_token>` | `std` |
| `std::flat_set` | `<flat_set>` | `std` |
| `std::sorted_unique" | `<ranges>` | `std` |

> **Note on standard types in headers:** Always prefer `<cstddef>` for `std::size_t`, `std::byte`, and `std::ptrdiff_t`. Prefer `<cstdint>` for fixed-width integers like `std::uint32_t`.
