if (NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
  message(FATAL_ERROR "CopyAppResources.cmake requires SOURCE_DIR and DEST_DIR")
endif()

if (NOT EXISTS "${SOURCE_DIR}")
  message(FATAL_ERROR "Source resources directory not found: ${SOURCE_DIR}")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")

file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}")
