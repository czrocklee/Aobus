package(default_visibility = ["//visibility:public"])

cc_import(
    name = "qt_core_lib",
    shared_library = "lib/libQt5Core.so",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "qt_core",
    hdrs = glob(["QtCore/**"]),
    includes = ["include"],
    deps = [
        ":qt_core_lib",
#        "@conan_qt//:headers",
    ],
)

cc_import(
    name = "qt_gui_lib",
    shared_library = "lib/libQt5Gui.so",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "qt_gui",
    hdrs = glob(["QtGui/**"]),
    includes = ["include"],
    deps = [
        ":qt_core",
        ":qt_gui_lib",
    ],
)

cc_import(
    name = "qt_widgets_lib",
    shared_library = "lib/libQt5Widgets.so",
    visibility = ["//visibility:private"],
)

cc_library(
    name = "qt_widgets",
    hdrs = glob(["QtWidgets/**"]),
    includes = ["include"],
    deps = [
        ":qt_gui",
        ":qt_widgets_lib",
    ],
)
