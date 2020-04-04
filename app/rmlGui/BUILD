load(":qt.bzl", "qt_cc_library")

filegroup(
    name = "resources",
    srcs = [
        "images/nocoverart.jpg",
    ],
)

qt_cc_library(
    name = "rmlQt",
    srcs = [
        "AddTrackDialog.cpp",
        "CoverArtLabel.cpp",
        "MainWindow.cpp",
        "NewListDialog.cpp",
        "TableModel.cpp",
        "TrackSortFilterProxyModel.cpp",
        "TrackView.cpp",
    ],
    hdrs = [
        "AddTrackDialog.h",
        "CoverArtLabel.h",
        "MainWindow.h",
        "NewListDialog.h",
        "TableModel.h",
        "TrackSortFilterProxyModel.h",
        "TrackView.h",
    ],
    copts = ["-std=c++17"],
    includes = ["."],
    qrcs = ["resources.qrc"],
    uis = [
        "AddTrackDialog.ui",
        "MainWindow.ui",
        "TrackView.ui",
        "NewListDialog.ui",
    ],
    deps = [
        "//ml:rs-ml",
        "@qt//:qt_widgets",
    ],
)

cc_binary(
    name = "rmlGui",
    #includes = ["."],
    srcs = [
        "rmlGui.cpp",
    ],
    copts = ["-std=c++17"],
    linkstatic = True,
    deps = [
        ":rmlQt",
    ],
)
