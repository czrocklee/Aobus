"""Expose configured quote roots and compile command lines for analysis-only native compile checks."""

load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _compile_context_impl(target, ctx):
    if CcInfo not in target:
        return []

    # aquery reports only the reference to an MSVC compile param file, not its
    # contents; Starlark exposes the same action's expanded command line.
    compiles = [
        {"outputs": sorted([output.path for output in action.outputs.to_list()]), "argv": action.argv}
        for action in target.actions
        if action.mnemonic == "CppCompile"
    ]

    # Checked external compiles need their command lines, not quote-root evidence.
    if ctx.label.repo_name and not compiles:
        return []
    contexts = [target[CcInfo].compilation_context.quote_includes]

    # cc_library compiles with its private dependencies without exporting them.
    for dependency in getattr(ctx.rule.attr, "implementation_deps", []):
        contexts.append(dependency[CcInfo].compilation_context.quote_includes)
    output = ctx.actions.declare_file(ctx.label.name + ".aobus-compile-context.json")
    ctx.actions.write(output, json.encode({
        "quote_includes": depset(transitive = contexts).to_list(),
        "compile_argv": compiles,
    }))
    return []

compile_context_aspect = aspect(
    implementation = _compile_context_impl,
    attr_aspects = ["*"],
)
