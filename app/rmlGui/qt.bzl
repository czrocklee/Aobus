def qt_cc_library(
        name,
        srcs,
        hdrs,
        normal_hdrs = [],
        uis = [],
        qrcs = [],
        **kwargs):

    for hdr in hdrs:
        base = hdr.split(".")[0]
        native.genrule(
            name = "%s_moc" % base,
            srcs = [hdr],
            outs = ["moc_%s.cpp" % base],
            cmd = "moc $(location %s) -o $@ -f'%s'" %
                (hdr, "%s/%s" % (native.package_name(), hdr)),
        )
        srcs.append(":%s_moc" % base)

    for ui in uis:
        base = ui.split(".")[0]
        native.genrule(
            name = "%s_uic" % base,
            srcs = [ui],
            outs = ["ui_%s.h" % base],
            cmd = "uic $(locations %s) -o $@" % ui,
        )
        hdrs.append(":%s_uic" % base)

    hdrs += normal_hdrs

    for qrc in qrcs:
        base = qrc.split(".")[0]
        native.genrule(
            name = "%s_qrc" % base,
            srcs = [qrc, ":%s" % base],
            outs = ["%s_qrc.cpp" % base],
            cmd = "rcc -name %s $(locations %s) -o $@" % (base, qrc),
        )
        srcs.append(":%s_qrc" % base)

    native.cc_library(
        name = name,
        srcs = srcs,
        hdrs = hdrs,
        **kwargs
    )
