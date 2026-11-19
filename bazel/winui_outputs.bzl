"""Semantic outputs of one configured WinUI generator, independent of SDK loading."""

WinuiGeneratedInfo = provider(
    doc = "Published XAML bytecode and the separately owned C++ include roots.",
    fields = {
        "headers": "Project-generated header tree (ordinary includes).",
        "sdk_headers": "SDK projection tree (external includes).",
        "xbf": "Compiled XAML files used by resource assembly.",
    },
)
