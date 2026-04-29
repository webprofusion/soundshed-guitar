include_guard(GLOBAL)

function(guitarfx_detect_windows_arch out_var)
    set(_guitarfx_windows_arch "")
    foreach(_guitarfx_windows_arch_var_name
            CMAKE_GENERATOR_PLATFORM
            MSVC_CXX_ARCHITECTURE_ID
            CMAKE_VS_PLATFORM_NAME
            CMAKE_SYSTEM_PROCESSOR)
        set(_guitarfx_windows_arch_candidate_value "${${_guitarfx_windows_arch_var_name}}")
        if(NOT _guitarfx_windows_arch_candidate_value STREQUAL "")
            set(_guitarfx_windows_arch "${_guitarfx_windows_arch_candidate_value}")
            break()
        endif()
    endforeach()
    string(TOLOWER "${_guitarfx_windows_arch}" _guitarfx_windows_arch)
    set(${out_var} "${_guitarfx_windows_arch}" PARENT_SCOPE)
endfunction()
