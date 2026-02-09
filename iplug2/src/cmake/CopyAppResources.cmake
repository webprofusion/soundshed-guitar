if (NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
  message(FATAL_ERROR "CopyAppResources.cmake requires SOURCE_DIR and DEST_DIR")
endif()

if (NOT EXISTS "${SOURCE_DIR}")
  message(FATAL_ERROR "Source resources directory not found: ${SOURCE_DIR}")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")

file(MAKE_DIRECTORY "${DEST_DIR}/ui")
file(COPY "${SOURCE_DIR}/" DESTINATION "${DEST_DIR}/ui"
  PATTERN "node_modules" EXCLUDE
  PATTERN "ts" EXCLUDE
  PATTERN "package.json" EXCLUDE
  PATTERN "package-lock.json" EXCLUDE
  PATTERN "tsconfig.json" EXCLUDE
)
