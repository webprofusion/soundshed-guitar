# When present, use Intel IPP for performance on Windows, Linux, and macOS (x86_64 only)
if(WIN32)
    if (DEFINED intelipp_static_win_x64_SOURCE_DIR)
        set(IPP_ROOT "${intelipp_static_win_x64_SOURCE_DIR}")
    else()
        set(IPP_ROOT "$ENV{USERPROFILE}/.nuget/packages/intelipp.static.win-x64/2022.3.1.8")
    endif()
    set(IPP_INC "${IPP_ROOT}/build/native/include/ipp")
    set(IPP_LIB "${IPP_ROOT}/build/native/win-x64/")
    set(IPP_LIBS ippsmt ippcoremt ippimt ippcvmt ippvmmt)
elseif(APPLE)
    # IPP is x86_64 only. For universal builds, we use -Xarch_x86_64 to conditionally
    # compile with PAMPLEJUCE_IPP and link IPP libraries only for the x86_64 slice.
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR
       (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64"))
        include(FetchContent)

        # IPP wheels are just zip files, FetchContent can download and extract them
        FetchContent_Declare(ipp_include
            URL https://files.pythonhosted.org/packages/f0/a0/3b0329735b28f05cbb87ce86be09567a2ecdf9bcd92c50b05fd0a30b34b9/ipp_include-2021.9.1-py2.py3-none-macosx_10_15_x86_64.macosx_11_0_x86_64.whl
            URL_HASH SHA256=5d01a621a9ca758180dffac9677d37ad23759e3c606a376c4bc85adc79a86e6c
        )
        FetchContent_Declare(ipp_static
            URL https://files.pythonhosted.org/packages/fa/19/8b5d54390389b5a9910a111bc033cb0045dc2e1614745c2c98dc54061370/ipp_static-2021.9.1-py2.py3-none-macosx_10_15_x86_64.macosx_11_0_x86_64.whl
            URL_HASH SHA256=d0e1f6643c1e1cf64722dd57b71aaa2692e40406abcecc3c3d575c519326390f
        )
        FetchContent_MakeAvailable(ipp_include ipp_static)

        set(IPP_ROOT "${ipp_include_SOURCE_DIR}")
        set(IPP_INC "${ipp_include_SOURCE_DIR}/ipp_include-2021.9.1.data/data/include")
        set(IPP_LIB "${ipp_static_SOURCE_DIR}/ipp_static-2021.9.1.data/data/lib")
        set(IPP_MACOS TRUE)
    endif()
elseif(UNIX)
    # Installed via intel-oneapi-ipp-devel package
    set(IPP_ROOT "/opt/intel/oneapi/ipp/latest")
    set(IPP_INC "${IPP_ROOT}/include")
    set(IPP_LIB "${IPP_ROOT}/lib")
    set(IPP_LIBS ipps ippcore ippi ippcv ippvm)
endif()

if (DEFINED IPP_ROOT)
    if (IS_DIRECTORY "${IPP_ROOT}")
        message(STATUS "INTEL IPP FOUND at ${IPP_ROOT}")
        target_include_directories(SharedCode INTERFACE "${IPP_INC}")

        if (IPP_MACOS)
            # For universal builds, use -Xarch_x86_64 to apply IPP flags only to x86_64 slice
            target_compile_options(SharedCode INTERFACE "SHELL:-Xarch_x86_64 -DPAMPLEJUCE_IPP=1")

            # Link IPP libraries only for x86_64 slice of universal binary
            # Use -Wl to pass library flags directly to linker, bypassing driver issues
            # ippcore must come last as other libs depend on it
            target_link_options(SharedCode INTERFACE
                "SHELL:-Xarch_x86_64 -Wl,-L${IPP_LIB},-lipps,-lippi,-lippcv,-lippvm,-lippcore"
            )
        else()
            target_link_directories(SharedCode INTERFACE "${IPP_LIB}")
            target_link_libraries(SharedCode INTERFACE ${IPP_LIBS})
            target_compile_definitions(SharedCode INTERFACE PAMPLEJUCE_IPP=1)
        endif()
    else ()
        message(STATUS "INTEL IPP NOT LOADED: ${IPP_ROOT} was not found")
    endif ()
endif ()
