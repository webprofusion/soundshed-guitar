# ---------------------------------------------------------------------------
# Host toolchain provisioning for cross-compiled (Android) builds on Windows.
#
# JUCE builds a helper tool, juceaide, during the configure step by re-invoking
# CMake with the *same generator* but no toolchain file, so the tool comes out
# as a host binary (extras/Build/juceaide/CMakeLists.txt). Gradle always drives
# the Android build with the Ninja generator, and Ninja + MSVC only works when
# the MSVC environment variables are already set — which they are not unless the
# build was launched from a Developer Command Prompt.
#
# Requiring developers (and CI) to launch Gradle from a developer prompt is a
# poor contract, and Android Studio would never satisfy it. Instead, locate the
# MSVC installation with vswhere, run vcvars64.bat, and import the resulting
# PATH/INCLUDE/LIB into this configure run. JUCE's nested configure inherits our
# environment, so it then finds a host compiler on its own.
#
# No-op when a host compiler is already reachable, and on non-Windows hosts.
# ---------------------------------------------------------------------------

function(ssg_provision_msvc_host_environment)
    if(NOT CMAKE_HOST_WIN32)
        return()
    endif()

    # If the caller already has a developer environment, respect it.
    find_program(_ssg_existing_cl NAMES cl NO_CACHE)
    if(_ssg_existing_cl)
        message(STATUS "Host toolchain: cl.exe already on PATH (${_ssg_existing_cl})")
        return()
    endif()

    set(_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")

    if(NOT EXISTS "${_vswhere}")
        message(FATAL_ERROR
            "Cannot find vswhere.exe at '${_vswhere}'.\n"
            "The Android build needs a host C++ compiler to build JUCE's juceaide "
            "tool. Install the Visual Studio C++ build tools, or run the build from "
            "a Developer Command Prompt.")
    endif()

    execute_process(
        COMMAND "${_vswhere}"
                -latest -prerelease -products *
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                -property installationPath
        OUTPUT_VARIABLE _vs_install
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(NOT _vs_install)
        message(FATAL_ERROR
            "vswhere found no Visual Studio installation with the C++ x64 tools.\n"
            "Install the 'Desktop development with C++' workload — the Android "
            "build needs a host compiler to build JUCE's juceaide tool.")
    endif()

    # vswhere prints Windows paths. Normalise for CMake's own file tests, then
    # convert back to native form for the batch file cmd.exe will run.
    file(TO_CMAKE_PATH "${_vs_install}" _vs_install)
    set(_vcvars "${_vs_install}/VC/Auxiliary/Build/vcvars64.bat")

    if(NOT EXISTS "${_vcvars}")
        message(FATAL_ERROR "Expected vcvars64.bat at '${_vcvars}' but it is missing.")
    endif()

    file(TO_NATIVE_PATH "${_vcvars}" _vcvars_native)

    message(STATUS "Host toolchain: importing MSVC environment from ${_vs_install}")

    # vcvars64 only sets variables in its own process, so have it echo the ones
    # we need and read them back. It inherits our PATH, so the captured PATH is
    # ours with the MSVC directories prepended — safe to adopt wholesale.
    #
    # The batch prints each value under a fixed SSG_ name rather than dumping
    # the whole environment: cmd's own `set` output varies in case and the
    # values are full of semicolons, which is exactly what CMake would try to
    # read as list separators.
    set(_wanted PATH INCLUDE LIB LIBPATH)

    set(_dump_script "${CMAKE_CURRENT_BINARY_DIR}/ssg-capture-vcvars.bat")
    set(_dump_body
        "@echo off\r\n"
        "call \"${_vcvars_native}\" >nul\r\n"
        "if errorlevel 1 exit /b 1\r\n")

    foreach(_var IN LISTS _wanted)
        string(APPEND _dump_body "echo SSG_${_var}=%${_var}%\r\n")
    endforeach()

    file(WRITE "${_dump_script}" ${_dump_body})

    execute_process(
        COMMAND cmd.exe /c "${_dump_script}"
        OUTPUT_VARIABLE _vcvars_env
        RESULT_VARIABLE _vcvars_result
        ERROR_VARIABLE _vcvars_error)

    if(NOT _vcvars_result EQUAL 0)
        message(FATAL_ERROR "Running vcvars64.bat failed:\n${_vcvars_error}")
    endif()

    set(_imported "")

    foreach(_var IN LISTS _wanted)
        # Match against the raw output rather than splitting it into a list, so
        # the semicolons inside each value are left alone.
        if(_vcvars_env MATCHES "(^|\n)SSG_${_var}=([^\r\n]*)")
            set(_value "${CMAKE_MATCH_2}")

            # An unset variable comes back as the literal "%NAME%".
            if(NOT _value STREQUAL "%${_var}%" AND NOT _value STREQUAL "")
                set(ENV{${_var}} "${_value}")
                list(APPEND _imported "${_var}")
            endif()
        endif()
    endforeach()

    foreach(_required PATH INCLUDE LIB)
        if(NOT _required IN_LIST _imported)
            message(FATAL_ERROR
                "vcvars64.bat produced no ${_required}; cannot set up a host compiler.")
        endif()
    endforeach()

    find_program(_ssg_provisioned_cl NAMES cl NO_CACHE)

    if(NOT _ssg_provisioned_cl)
        message(FATAL_ERROR
            "Imported the MSVC environment but cl.exe is still not on PATH. "
            "Check the Visual Studio installation at '${_vs_install}'.")
    endif()

    message(STATUS "Host toolchain: using ${_ssg_provisioned_cl}")
endfunction()
