if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "copy_optional_dir.cmake requires SOURCE_DIR and DEST_DIR")
endif()

if(EXISTS "${SOURCE_DIR}")
    file(MAKE_DIRECTORY "${DEST_DIR}")
    file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}/")
endif()
