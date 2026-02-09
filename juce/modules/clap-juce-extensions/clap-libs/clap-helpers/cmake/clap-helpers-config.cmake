if (NOT TARGET clap-helpers)
    add_library(clap-helpers INTERFACE IMPORTED GLOBAL)
    target_include_directories(clap-helpers SYSTEM INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../../../include")
endif()
