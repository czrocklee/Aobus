load(":qt.bzl", "qt_cc_library")

qt_cc_library(
    name = "rmlQt",
    hdrs = ["AddTrackDialog.h", "MainWindow.h", "TrackView.h", "TableModel.h", "TrackSortFilterProxyModel.h"],
    srcs = ["AddTrackDialog.cpp", "MainWindow.cpp", "TrackView.cpp", "TableModel.cpp", "TrackSortFilterProxyModel.cpp"],
    uis = ["AddTrackDialog.ui", "MainWindow.ui", "TrackView.ui"],
    deps = [
        "//ml:rs-ml",
        "@qt//:qt_widgets",
    ],
    copts = ["-std=c++17"],
)

cc_binary(
    name = "rmlGui",
    #includes = ["."],

    srcs = [
        "rmlGui.cpp"
        # "rmlGui/TrackSortFilterProxyModel.h",
        # "rmlGui/TrackSortFilterProxyModel.cpp",
        # "rmlGui/MainWindow.h",
        # "rmlGui/MainWindow.cpp",
        # "rmlGui/AddTrackDialog.h",
        # "rmlGui/AddTrackDialog.cpp"
    ],
    copts = ["-std=c++17"],
    deps = [
        ":rmlQt"

    ],
)
