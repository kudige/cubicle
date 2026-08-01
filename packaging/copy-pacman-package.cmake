file(GLOB packages
    "${SOURCE_DIR}/${PACKAGE_NAME}-${PACKAGE_VERSION}-1-${PACKAGE_ARCH}.pkg.tar.*"
)

list(FILTER packages EXCLUDE REGEX "\\.sig$")
list(LENGTH packages package_count)

if(package_count EQUAL 0)
    message(FATAL_ERROR "makepkg did not create a pacman package in ${SOURCE_DIR}")
elseif(package_count GREATER 1)
    message(FATAL_ERROR "makepkg created multiple pacman packages: ${packages}")
endif()

list(GET packages 0 package_path)
get_filename_component(package_file "${package_path}" NAME)
file(COPY_FILE "${package_path}" "${DEST_DIR}/${package_file}" ONLY_IF_DIFFERENT)
message(STATUS "Pacman package: ${DEST_DIR}/${package_file}")
