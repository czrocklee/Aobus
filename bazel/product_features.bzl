"""Explicit product graph toggles, independent of native SDK admission."""

def _flag_impl(_ctx):
    return []

product_flag = rule(
    implementation = _flag_impl,
    build_setting = config.bool(flag = True),
)
