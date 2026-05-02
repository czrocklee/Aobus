# Shared include directories for all ao_* library targets.
set(AO_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set(AO_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")

# ── Library modules ─────────────────────────────────────────────────────────
add_subdirectory(utility)
add_subdirectory(lmdb)
add_subdirectory(media)
add_subdirectory(tag)
add_subdirectory(library)
add_subdirectory(query)
add_subdirectory(model)
add_subdirectory(audio)

# ── Umbrella interface library ──────────────────────────────────────────────
add_library(ao INTERFACE)
target_link_libraries(ao INTERFACE
    ao_utility
    ao_lmdb
    ao_media
    ao_tag
    ao_library
    ao_query
    ao_model
    ao_audio
)
