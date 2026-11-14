"""Native compile contexts for the cached clean/warning/error gate contracts."""

def _canary_impl(ctx):
    ctx.actions.expand_template(template = ctx.file.header_template, output = ctx.outputs.header, substitutions = {})
    ctx.actions.write(ctx.outputs.warning, '#include "CacheControl.h"\n#include <string_view>\nbool hasCachePrefix(std::string_view text) { return text.find(AOBUS_CACHE_PREFIX) == 0; }\n')

    # These assertions distinguish the native wrapper's frontend state from
    # include-path-only adaptation, without generating an object or running code.
    ctx.actions.write(ctx.outputs.clean, '''#include "CacheControl.h"
#include <string_view>
#if defined(__linux__)
#ifndef __SSP_STRONG__
#error Aobus lint lost the native stack-protector frontend macro
#endif
#if defined(NDEBUG) && !defined(_FORTIFY_SOURCE)
#error Aobus lint lost the native Release fortify macro
#endif
#if !defined(NDEBUG) && defined(_FORTIFY_SOURCE)
#error Aobus lint injected fortify into the native Debug profile
#endif
#endif
bool hasCachePrefix(std::string_view text) { return text.starts_with(AOBUS_CACHE_PREFIX); }
''')
    ctx.actions.write(ctx.outputs.error, '#include "CacheControl.h"\n#error Aobus cached lint compiler-error canary\n')
    return [DefaultInfo(files = depset([ctx.outputs.header, ctx.outputs.warning, ctx.outputs.clean, ctx.outputs.error]))]

lint_cache_canaries = rule(
    implementation = _canary_impl,
    attrs = {
        "header_template": attr.label(allow_single_file = True, mandatory = True),
        "header": attr.output(mandatory = True),
        "warning": attr.output(mandatory = True),
        "clean": attr.output(mandatory = True),
        "error": attr.output(mandatory = True),
    },
)
