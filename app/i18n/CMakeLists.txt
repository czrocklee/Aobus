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
file(MAKE_DIRECTORY
    "${AOBUS_CATALOG_GENERATED_DIR}"
    "${AOBUS_CATALOG_RESW_DIR}/en"
    "${AOBUS_CATALOG_RESW_DIR}/de"
    "${AOBUS_CATALOG_RESW_DIR}/zh-Hans"
    "${AOBUS_CATALOG_RESW_DIR}/zh-Hant"
    "${AOBUS_CATALOG_RESW_DIR}/ja"
    "${AOBUS_CATALOG_RESW_DIR}/es"
    "${AOBUS_CATALOG_RESW_DIR}/fr"
    "${AOBUS_CATALOG_RESW_DIR}/qps-ploc")

set(AOBUS_CATALOG_ROOT_RES "${AOBUS_CATALOG_GENERATED_DIR}/root.res")
set(AOBUS_CATALOG_DE_RES "${AOBUS_CATALOG_GENERATED_DIR}/de.res")
set(AOBUS_CATALOG_ZH_HANS_RES "${AOBUS_CATALOG_GENERATED_DIR}/zh_Hans.res")
set(AOBUS_CATALOG_ZH_HANT_RES "${AOBUS_CATALOG_GENERATED_DIR}/zh_Hant.res")
set(AOBUS_CATALOG_JA_RES "${AOBUS_CATALOG_GENERATED_DIR}/ja.res")
set(AOBUS_CATALOG_ES_RES "${AOBUS_CATALOG_GENERATED_DIR}/es.res")
set(AOBUS_CATALOG_FR_RES "${AOBUS_CATALOG_GENERATED_DIR}/fr.res")

set(AOBUS_CATALOG_PSEUDO_SOURCE "${AOBUS_CATALOG_GENERATED_DIR}/qps_Ploc.txt")
set(AOBUS_CATALOG_PSEUDO_RES "${AOBUS_CATALOG_GENERATED_DIR}/qps_Ploc.res")
set(AOBUS_CATALOG_DATA "${AOBUS_CATALOG_GENERATED_DIR}/aobus_messages.dat")
set(AOBUS_CATALOG_EMBEDDED_CPP "${AOBUS_CATALOG_GENERATED_DIR}/EmbeddedCatalogData.cpp")

set(AOBUS_CATALOG_EN_RESW "${AOBUS_CATALOG_RESW_DIR}/en/Resources.resw")
set(AOBUS_CATALOG_DE_RESW "${AOBUS_CATALOG_RESW_DIR}/de/Resources.resw")
set(AOBUS_CATALOG_ZH_HANS_RESW "${AOBUS_CATALOG_RESW_DIR}/zh-Hans/Resources.resw")
set(AOBUS_CATALOG_ZH_HANT_RESW "${AOBUS_CATALOG_RESW_DIR}/zh-Hant/Resources.resw")
set(AOBUS_CATALOG_JA_RESW "${AOBUS_CATALOG_RESW_DIR}/ja/Resources.resw")
set(AOBUS_CATALOG_ES_RESW "${AOBUS_CATALOG_RESW_DIR}/es/Resources.resw")
set(AOBUS_CATALOG_FR_RESW "${AOBUS_CATALOG_RESW_DIR}/fr/Resources.resw")
set(AOBUS_CATALOG_PSEUDO_RESW "${AOBUS_CATALOG_RESW_DIR}/qps-ploc/Resources.resw")

set(AOBUS_CATALOG_WINUI_RESW_FILES
    "${AOBUS_CATALOG_EN_RESW}"
    "${AOBUS_CATALOG_DE_RESW}"
    "${AOBUS_CATALOG_ZH_HANS_RESW}"
    "${AOBUS_CATALOG_ZH_HANT_RESW}"
    "${AOBUS_CATALOG_JA_RESW}"
    "${AOBUS_CATALOG_ES_RESW}"
    "${AOBUS_CATALOG_FR_RESW}"
    "${AOBUS_CATALOG_PSEUDO_RESW}")

add_custom_command(
    OUTPUT
            "${AOBUS_CATALOG_ROOT_RES}"
            "${AOBUS_CATALOG_DE_RES}"
            "${AOBUS_CATALOG_ZH_HANS_RES}"
            "${AOBUS_CATALOG_ZH_HANT_RES}"
            "${AOBUS_CATALOG_JA_RES}"
            "${AOBUS_CATALOG_ES_RES}"
            "${AOBUS_CATALOG_FR_RES}"
    COMMAND "${AOBUS_ICU_GENRB_EXECUTABLE}"
            --strict
            --encoding UTF-8
            --sourcedir "${AOBUS_CATALOG_SOURCE_DIR}"
            --destdir "${AOBUS_CATALOG_GENERATED_DIR}"
            root.txt de.txt zh_Hans.txt zh_Hant.txt ja.txt es.txt fr.txt
    DEPENDS
            "${AOBUS_CATALOG_SOURCE_DIR}/root.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/de.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/zh_Hans.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/zh_Hant.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/ja.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/es.txt"
            "${AOBUS_CATALOG_SOURCE_DIR}/fr.txt"
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
            --translation de.res ../resw/de/Resources.resw
            --translation zh_Hans.res ../resw/zh-Hans/Resources.resw
            --translation zh_Hant.res ../resw/zh-Hant/Resources.resw
            --translation ja.res ../resw/ja/Resources.resw
            --translation es.res ../resw/es/Resources.resw
            --translation fr.res ../resw/fr/Resources.resw
    DEPENDS ao_catalog_compiler
            "${AOBUS_CATALOG_ROOT_RES}"
            "${AOBUS_CATALOG_DE_RES}"
            "${AOBUS_CATALOG_ZH_HANS_RES}"
            "${AOBUS_CATALOG_ZH_HANT_RES}"
            "${AOBUS_CATALOG_JA_RES}"
            "${AOBUS_CATALOG_ES_RES}"
            "${AOBUS_CATALOG_FR_RES}"
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
            "${AOBUS_CATALOG_SOURCE_DIR}/package.lst"
    DEPENDS
            "${AOBUS_CATALOG_ROOT_RES}"
            "${AOBUS_CATALOG_DE_RES}"
            "${AOBUS_CATALOG_ZH_HANS_RES}"
            "${AOBUS_CATALOG_ZH_HANT_RES}"
            "${AOBUS_CATALOG_JA_RES}"
            "${AOBUS_CATALOG_ES_RES}"
            "${AOBUS_CATALOG_FR_RES}"
            "${AOBUS_CATALOG_PSEUDO_RES}"
            "${AOBUS_CATALOG_SOURCE_DIR}/package.lst"
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
    PRIVATE "${CMAKE_SOURCE_DIR}")
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
foreach(_catalog_output IN ITEMS
    AOBUS_CATALOG_EN_RESW
    AOBUS_CATALOG_DE_RESW
    AOBUS_CATALOG_ZH_HANS_RESW
    AOBUS_CATALOG_ZH_HANT_RESW
    AOBUS_CATALOG_JA_RESW
    AOBUS_CATALOG_ES_RESW
    AOBUS_CATALOG_FR_RESW
    AOBUS_CATALOG_PSEUDO_RESW
    AOBUS_CATALOG_WINUI_RESW_FILES)
  set(${_catalog_output} "${${_catalog_output}}" PARENT_SCOPE)
endforeach()
