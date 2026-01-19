if(NOT DEFINED SOURCE_DLL OR NOT DEFINED DEST_BUNDLE_DIR OR NOT DEFINED RESOURCE_UI_DIR OR NOT DEFINED RESOURCE_AMPS_DIR OR NOT DEFINED RESOURCE_IR_DIR)
  message(FATAL_ERROR "CopyVst3Bundle.cmake requires SOURCE_DLL, DEST_BUNDLE_DIR, RESOURCE_UI_DIR, RESOURCE_AMPS_DIR, and RESOURCE_IR_DIR.")
endif()

set(contents_dir "${DEST_BUNDLE_DIR}/Contents")
set(arch_dir "${contents_dir}/x86_64-win")
set(res_dir "${contents_dir}/Resources")

execute_process(COMMAND "${CMAKE_COMMAND}" -E make_directory "${arch_dir}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E make_directory "${res_dir}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SOURCE_DLL}" "${arch_dir}/SoundshedGuitar.vst3"
  RESULT_VARIABLE copy_result
)

if(NOT copy_result EQUAL 0)
  message(WARNING "Failed to copy VST3 binary to bundle (likely in use). Skipping bundle update.")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCE_UI_DIR}" "${res_dir}/ui"
  RESULT_VARIABLE res_result
)

if(NOT res_result EQUAL 0)
  message(WARNING "Failed to copy VST3 resources to bundle (likely in use).")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCE_AMPS_DIR}" "${res_dir}/amps"
  RESULT_VARIABLE amps_result
)

if(NOT amps_result EQUAL 0)
  message(WARNING "Failed to copy VST3 amps resources to bundle (likely in use).")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${RESOURCE_IR_DIR}" "${res_dir}/ir"
  RESULT_VARIABLE ir_result
)

if(NOT ir_result EQUAL 0)
  message(WARNING "Failed to copy VST3 IR resources to bundle (likely in use).")
endif()
