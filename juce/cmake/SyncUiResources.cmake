if(NOT DEFINED UI_STAGE_DIR OR NOT DEFINED UI_DESTINATION_DIR OR NOT DEFINED UI_STAGE_STAMP)
    message(FATAL_ERROR "UI_STAGE_DIR, UI_DESTINATION_DIR, and UI_STAGE_STAMP are required")
endif()

if(NOT EXISTS "${UI_STAGE_DIR}/index.html")
    message(FATAL_ERROR "Staged UI is incomplete: ${UI_STAGE_DIR}/index.html is missing")
endif()

set(_destination_stamp "${UI_DESTINATION_DIR}/.soundshed-ui-stage")

if(EXISTS "${UI_STAGE_STAMP}" AND EXISTS "${_destination_stamp}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${UI_STAGE_STAMP}" "${_destination_stamp}"
        RESULT_VARIABLE _compare_result)

    if(_compare_result EQUAL 0)
        return()
    endif()
endif()

file(REMOVE_RECURSE "${UI_DESTINATION_DIR}")
file(COPY "${UI_STAGE_DIR}/" DESTINATION "${UI_DESTINATION_DIR}")
file(COPY_FILE "${UI_STAGE_STAMP}" "${_destination_stamp}")
