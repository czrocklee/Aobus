# Shared include directories for all rs_* library targets.
set(RS_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set(RS_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")

# ── Library modules ─────────────────────────────────────────────────────────
add_subdirectory(utility)
add_subdirectory(lmdb)
add_subdirectory(media)
add_subdirectory(tag)
add_subdirectory(library)
add_subdirectory(expr)
add_subdirectory(model)
add_subdirectory(audio)

# ── Umbrella interface library ──────────────────────────────────────────────
add_library(rs INTERFACE)
target_link_libraries(rs INTERFACE
    rs_utility
    rs_lmdb
    rs_media
    rs_tag
    rs_library
    rs_expr
    rs_model
    rs_audio
)
