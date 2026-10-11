vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO alandtse/CommonLibVR
    REF a66dd5516dc55634856e5f779750455e2f9e8794
    SHA512 8b9be998cbab45d0aa78d93fcd14c0ed3e37efc86b3d2c850672e78d4026d33753af20af6a0a5a27de748119cea68a4a476323068f15d47378411629d79fabac
    HEAD_REF ng
)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH2
    REPO ValveSoftware/openvr
    REF master
    SHA512 8538c9d0c24c2003a694c1e1f27ec643f4eb8178c1c57c22baa3be5654c7f4bbc456e15c3ed970bd36d236d26492a2bbee9abef34eb5598a35808fb0f7c7718d
    HEAD_REF master
)

file(GLOB OPENVR_FILES "${SOURCE_PATH2}/*")
file(COPY ${OPENVR_FILES} DESTINATION "${SOURCE_PATH}/extern/openvr")

vcpkg_configure_cmake(
    SOURCE_PATH "${SOURCE_PATH}"
    PREFER_NINJA
    OPTIONS -DBUILD_TESTS=off -DSKSE_SUPPORT_XBYAK=on
)

vcpkg_install_cmake()
vcpkg_cmake_config_fixup(PACKAGE_NAME CommonLibSSE CONFIG_PATH lib/cmake)
vcpkg_copy_pdbs()

file(INSTALL "${SOURCE_PATH2}/headers/openvr.h" DESTINATION ${CURRENT_PACKAGES_DIR}/include)
file(GLOB CMAKE_CONFIGS "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE/*.cmake")
file(INSTALL ${CMAKE_CONFIGS} DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")
file(INSTALL "${SOURCE_PATH}/cmake/CommonLibSSE.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/CommonLibSSE/CommonLibSSE")


vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH2}/LICENSE")


set(VCPKG_POLICY_SKIP_ABSOLUTE_PATHS_CHECK enabled)