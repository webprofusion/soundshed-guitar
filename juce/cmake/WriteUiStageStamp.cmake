if(NOT DEFINED UI_STAGE_DIR OR NOT DEFINED UI_STAGE_STAMP)
    message(FATAL_ERROR "UI_STAGE_DIR and UI_STAGE_STAMP are required")
endif()

file(GLOB_RECURSE _ui_files LIST_DIRECTORIES false "${UI_STAGE_DIR}/*")
list(SORT _ui_files)

set(_manifest "")
foreach(_ui_file IN LISTS _ui_files)
    file(SHA256 "${_ui_file}" _file_hash)
    file(RELATIVE_PATH _relative_path "${UI_STAGE_DIR}" "${_ui_file}")
    string(APPEND _manifest "${_relative_path}:${_file_hash}\n")
endforeach()

file(WRITE "${UI_STAGE_STAMP}" "${_manifest}")
