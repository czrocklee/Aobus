"""Keep a binary's configured link closure away from forbidden libraries."""

_ClosureInfo = provider(doc = "Labels reached through linked dependency edges.", fields = ["labels"])

# The linked dependency edges of rules_cc libraries and binaries.
_EDGES = ["deps", "implementation_deps"]

def _closure_aspect_impl(target, ctx):
    reached = [
        dependency[_ClosureInfo].labels
        for name in _EDGES
        for dependency in getattr(ctx.rule.attr, name, [])
        if _ClosureInfo in dependency
    ]
    return [_ClosureInfo(labels = depset([target.label], transitive = reached))]

_closure_aspect = aspect(implementation = _closure_aspect_impl, attr_aspects = _EDGES)

def _link_closure_guard_impl(ctx):
    # Dependency labels resolve aliases exactly as the aspect's reached labels do.
    forbidden = {dependency.label: None for dependency in ctx.attr.forbidden}
    violations = [
        label
        for label in ctx.attr.binary[_ClosureInfo].labels.to_list()
        if label in forbidden or (label.repo_name == "" and label.package in ctx.attr.forbidden_packages)
    ]
    if violations:
        fail("%s reaches forbidden link dependencies: %s" % (
            ctx.attr.binary.label,
            ", ".join([str(label) for label in violations]),
        ))
    return [DefaultInfo()]

link_closure_guard = rule(
    implementation = _link_closure_guard_impl,
    doc = "Fails analysis when the binary's configured link closure reaches a forbidden library.",
    attrs = {
        "binary": attr.label(mandatory = True, aspects = [_closure_aspect]),
        "forbidden": attr.label_list(doc = "Libraries the closure must not reach."),
        "forbidden_packages": attr.string_list(doc = "Main-repository packages the closure must not reach."),
    },
)
