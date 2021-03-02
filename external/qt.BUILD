package(default_visibility = ["//visibility:public"])

filegroup(
    name = "qt_platform_plugins",
    srcs = [
        "plugins/platforms/libqxcb.so",
    ],
    visibility = ["//visibility:public"],
)

cc_import(
    name = "qt_dbus_lib",
    shared_library = "lib/libQt6DBus.so",
    visibility = ["//visibility:private"],
)

cc_import(
    name = "qt_qpa_lib",
    shared_library = "lib/libQt6XcbQpa.so",
    visibility = ["//visibility:private"],
)

cc_import(
    name = "qt_core_lib",
    shared_library = "lib/libQt6Core.so",
    visibility = ["//visibility:public"],
)

cc_library(
    name = "qt_core",
    hdrs = glob(["include/QtCore/**"]),
    includes = ["include"],
    linkstatic = False,
    deps = [
        ":qt_core_lib",
    ],
)

cc_import(
    name = "qt_gui_lib",
    shared_library = "lib/libQt6Gui.so",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "qt_gui",
    hdrs = glob(["include/QtGui/**"]),
    includes = ["include"],
    deps = [
        ":qt_core",
        ":qt_gui_lib",
        ":qt_qpa_lib",
        ":qt_dbus_lib",
    ],
)

cc_import(
    name = "qt_widgets_lib",
    shared_library = "lib/libQt6Widgets.so",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "qt_widgets",
    hdrs = glob(["include/QtWidgets/**"]),
    includes = ["include"],
    deps = [
        ":qt_gui",
        ":qt_widgets_lib",
    ],
)
