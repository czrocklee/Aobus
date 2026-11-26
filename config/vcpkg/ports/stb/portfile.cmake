# This overlay shadows the complete stb port. Freeze the default registry's
# remaining headers while replacing stb_image_resize2 v2.10 with shell.nix's
# v2.18 pin. The rejected resolver alternatives and tracked removal gate are
# owned by doc/development/dependency-governance.md#temporary-overlay-ports.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO nothings/stb
    REF f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31 # committed on 2024-07-29
    SHA512 4a733aefb816a366c999663e3d482144616721b26c321ee5dd0dce611a34050b6aef97d46bd2c4f8a9631d83b097491a7ce88607fd9493d880aaa94567a68cce
    HEAD_REF master
)

vcpkg_download_distfile(
    STB_IMAGE_RESIZE2_HEADER
    URLS https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image_resize2.h
    FILENAME stb_image_resize2-2c980bb59875b0d32144a71867fbdebb2f77cd20.h
    SHA512 13bbda31dd82180873a7b8634cd35f1ceb9aeec62c4982f24be274eb931b5434e0df6454104a3bf828b7f1d92500e13c07b569e5ed739160c558cc85ee7bf595
)

file(GLOB HEADER_FILES "${SOURCE_PATH}/*.h" "${SOURCE_PATH}/stb_vorbis.c")
list(REMOVE_ITEM HEADER_FILES "${SOURCE_PATH}/stb_image_resize2.h")
file(COPY ${HEADER_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/include")
file(INSTALL "${STB_IMAGE_RESIZE2_HEADER}"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include"
     RENAME stb_image_resize2.h)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/FindStb.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
