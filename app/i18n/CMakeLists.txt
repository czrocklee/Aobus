# Interactive localization is a leaf application capability. Its build-time
# compiler and ICU implementation stay out of Core, runtime, and the CLI graph.
add_library(ao_catalog_pattern STATIC
    CatalogPattern.cpp
)
target_include_directories(ao_catalog_pattern PUBLIC "${CMAKE_SOURCE_DIR}/app" "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ao_catalog_pattern PRIVATE ICU::i18n ICU::uc ICU::data)

add_executable(ao_catalog_compiler
    ${CMAKE_SOURCE_DIR}/tool/catalog/CatalogCompiler.cpp
)
aobus_enable_windows_utf8_manifest(ao_catalog_compiler)
target_include_directories(ao_catalog_compiler PRIVATE "${CMAKE_SOURCE_DIR}/app/include")
target_link_libraries(ao_catalog_compiler PRIVATE ao_catalog_pattern ao_utility)

set(AOBUS_CATALOG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/catalog")
set(AOBUS_CATALOG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/catalog")
set(AOBUS_CATALOG_RESW_DIR "${CMAKE_CURRENT_BINARY_DIR}/resw")
# The ICU package inventory owns build inputs, native projections, and the
# selectable locale tags. Root and pseudo keep their existing special roles.
set(AOBUS_CATALOG_PACKAGE_LIST "${AOBUS_CATALOG_SOURCE_DIR}/package.lst")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AOBUS_CATALOG_PACKAGE_LIST}")
file(STRINGS "${AOBUS_CATALOG_PACKAGE_LIST}" _catalog_resources)
set(AOBUS_CATALOG_AUTHORED_RES)
set(AOBUS_CATALOG_AUTHORED_SOURCES)
set(AOBUS_CATALOG_AUTHORED_INPUTS)
set(AOBUS_CATALOG_TRANSLATION_ARGS)
set(AOBUS_CATALOG_WINUI_RESW_FILES)
set(_catalog_locale_tags)
foreach(_catalog_resource IN LISTS _catalog_resources)
  get_filename_component(_catalog_locale "${_catalog_resource}" NAME_WE)
  string(REPLACE "_" "-" _catalog_tag "${_catalog_locale}")
  if(_catalog_locale STREQUAL "root")
    set(_catalog_tag "en")
  elseif(_catalog_locale STREQUAL "qps_Ploc")
    set(_catalog_tag "qps-ploc")
  endif()
  set(_catalog_resw "${AOBUS_CATALOG_RESW_DIR}/${_catalog_tag}/Resources.resw")
  file(MAKE_DIRECTORY "${AOBUS_CATALOG_RESW_DIR}/${_catalog_tag}")
  list(APPEND AOBUS_CATALOG_WINUI_RESW_FILES "${_catalog_resw}")
  if(NOT _catalog_locale STREQUAL "qps_Ploc")
    list(APPEND AOBUS_CATALOG_AUTHORED_RES "${AOBUS_CATALOG_GENERATED_DIR}/${_catalog_resource}")
    list(APPEND AOBUS_CATALOG_AUTHORED_SOURCES "${AOBUS_CATALOG_SOURCE_DIR}/${_catalog_locale}.txt")
    list(APPEND AOBUS_CATALOG_AUTHORED_INPUTS "${_catalog_locale}.txt")
    list(APPEND _catalog_locale_tags "${_catalog_tag}")
    if(NOT _catalog_locale STREQUAL "root")
      list(APPEND AOBUS_CATALOG_TRANSLATION_ARGS --translation "${_catalog_resource}" "${_catalog_resw}")
    endif()
  endif()
endforeach()

# Keep English first and other choices in stable tag order, independent of the
# packaging order. The frontend adds its own follow-system choice.
list(SORT _catalog_locale_tags)
list(REMOVE_ITEM _catalog_locale_tags "en")
list(PREPEND _catalog_locale_tags "en")
set(AOBUS_CATALOG_LOCALE_TAG_LITERALS)
foreach(_catalog_tag IN LISTS _catalog_locale_tags)
  string(APPEND AOBUS_CATALOG_LOCALE_TAG_LITERALS "    \"${_catalog_tag}\",\n")
endforeach()
configure_file(CatalogLocaleTags.h.in "${AOBUS_CATALOG_GENERATED_DIR}/CatalogLocaleTags.h" @ONLY)

set(AOBUS_CATALOG_PSEUDO_SOURCE "${AOBUS_CATALOG_GENERATED_DIR}/qps_Ploc.txt")
set(AOBUS_CATALOG_PSEUDO_RES "${AOBUS_CATALOG_GENERATED_DIR}/qps_Ploc.res")
set(AOBUS_CATALOG_DATA "${AOBUS_CATALOG_GENERATED_DIR}/aobus_messages.dat")
set(AOBUS_CATALOG_EMBEDDED_CPP "${AOBUS_CATALOG_GENERATED_DIR}/EmbeddedCatalogData.cpp")

add_custom_command(
    OUTPUT ${AOBUS_CATALOG_AUTHORED_RES}
    COMMAND "${AOBUS_ICU_GENRB_EXECUTABLE}"
            --strict
            --encoding UTF-8
            --sourcedir "${AOBUS_CATALOG_SOURCE_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            ${AOBUS_CATALOG_AUTHORED_INPUTS}
    DEPENDS ${AOBUS_CATALOG_AUTHORED_SOURCES}
    COMMENT "Compiling canonical ICU message catalogs"
    VERBATIM)

add_custom_command(
    OUTPUT
            "${AOBUS_CATALOG_PSEUDO_SOURCE}"
            ${AOBUS_CATALOG_WINUI_RESW_FILES}
    COMMAND $<TARGET_FILE:ao_catalog_compiler>
            --root-res root.res
            --pseudo-source qps_Ploc.txt
            --en-resw ../resw/en/Resources.resw
            --pseudo-resw ../resw/qps-ploc/Resources.resw
            ${AOBUS_CATALOG_TRANSLATION_ARGS}
    DEPENDS ao_catalog_compiler ${AOBUS_CATALOG_AUTHORED_RES}
    WORKING_DIRECTORY "${AOBUS_CATALOG_GENERATED_DIR}"
    COMMENT "Validating catalogs and generating pseudo/WinUI resources"
    VERBATIM)

add_custom_command(
    OUTPUT "${AOBUS_CATALOG_PSEUDO_RES}"
    COMMAND "${AOBUS_ICU_GENRB_EXECUTABLE}"
            --strict
            --encoding UTF-8
            --sourcedir "${AOBUS_CATALOG_GENERATED_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            qps_Ploc.txt
    DEPENDS "${AOBUS_CATALOG_PSEUDO_SOURCE}"
    COMMENT "Compiling generated pseudo-localization catalog"
    VERBATIM)

add_custom_command(
    OUTPUT "${AOBUS_CATALOG_DATA}"
    COMMAND "${AOBUS_ICU_PKGDATA_EXECUTABLE}"
            --quiet
            --force-prefix
            --name aobus_messages
            --mode common
            --sourcedir "${AOBUS_CATALOG_GENERATED_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            --tempdir "${AOBUS_CATALOG_GENERATED_DIR}"
            "${AOBUS_CATALOG_PACKAGE_LIST}"
    DEPENDS ${AOBUS_CATALOG_AUTHORED_RES}
            "${AOBUS_CATALOG_PSEUDO_RES}"
            "${AOBUS_CATALOG_PACKAGE_LIST}"
    COMMENT "Packaging embedded ICU message data"
    VERBATIM)

add_custom_command(
    OUTPUT "${AOBUS_CATALOG_EMBEDDED_CPP}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${AOBUS_CATALOG_DATA}"
            "-DOUTPUT=${AOBUS_CATALOG_EMBEDDED_CPP}"
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    DEPENDS "${AOBUS_CATALOG_DATA}" "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake"
    COMMENT "Embedding ICU message data"
    VERBATIM)

add_library(ao_app_i18n STATIC
    MessageCatalog.cpp
    EmbeddedCatalogData.h
    MessageIds.h
    SystemLocale.h
    "${AOBUS_CATALOG_EMBEDDED_CPP}"
)
if(WIN32)
  target_sources(ao_app_i18n PRIVATE SystemLocaleWindows.cpp)
else()
  target_sources(ao_app_i18n PRIVATE SystemLocalePosix.cpp)
endif()
target_include_directories(ao_app_i18n
    PUBLIC
      "${CMAKE_SOURCE_DIR}/app/include"
      "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}" "${AOBUS_CATALOG_GENERATED_DIR}")
target_link_libraries(ao_app_i18n
    PRIVATE ao_catalog_pattern ao_unicode_text ao_utility ICU::i18n ICU::uc ICU::data)

# Locale collation stays in an interactive leaf. The policy interface itself
# contains no ICU types, so runtime/UIModel consumers do not inherit ICU::i18n.
add_library(ao_app_i18n_ordering STATIC
    IcuTextOrdering.cpp
)
target_include_directories(ao_app_i18n_ordering
    PUBLIC
      "${CMAKE_SOURCE_DIR}/app/include"
      "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/app")
target_link_libraries(ao_app_i18n_ordering
    PRIVATE ao_unicode_text ICU::i18n ICU::uc ICU::data)

# Completion aliases use the same ICU leaf boundary as locale ordering. The
# runtime policy carries no ICU types and the CLI must not reach this target.
add_library(ao_app_i18n_completion STATIC
    IcuCompletionAliases.cpp
)
target_include_directories(ao_app_i18n_completion
    PUBLIC
      "${CMAKE_SOURCE_DIR}/app/include"
      "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(ao_app_i18n_completion
    PRIVATE ao_unicode_text ICU::i18n ICU::uc ICU::data)

# WinUI consumes the generated resource paths from its sibling directory.
set(AOBUS_CATALOG_WINUI_RESW_FILES "${AOBUS_CATALOG_WINUI_RESW_FILES}" PARENT_SCOPE)
