include(FindPackageHandleStandardArgs)

function(guitarfx_webview2_detect_arch out_var out_use_dll_import_var)
    set(_arch "")
    set(_use_dll_import OFF)

    foreach(_candidate
            "${MSVC_CXX_ARCHITECTURE_ID}"
            "${MSVC_C_ARCHITECTURE_ID}"
            "${CMAKE_VS_PLATFORM_NAME}"
            "${CMAKE_GENERATOR_PLATFORM}"
            "${JUCE_TARGET_ARCHITECTURE}"
            "${CMAKE_SYSTEM_PROCESSOR}")
        if(NOT "${_candidate}" STREQUAL "")
            string(TOLOWER "${_candidate}" _candidate_lower)

            if(_candidate_lower STREQUAL "arm64" OR _candidate_lower STREQUAL "aarch64")
                set(_arch "arm64")
                break()
            endif()

            if(_candidate_lower STREQUAL "arm64ec")
                set(_arch "arm64")
                set(_use_dll_import ON)
                break()
            endif()

            if(_candidate_lower STREQUAL "x64" OR _candidate_lower STREQUAL "amd64" OR _candidate_lower STREQUAL "x86_64")
                set(_arch "x64")
                break()
            endif()

            if(_candidate_lower STREQUAL "win32" OR _candidate_lower STREQUAL "x86" OR _candidate_lower STREQUAL "i386")
                set(_arch "x86")
                break()
            endif()
        endif()
    endforeach()

    if(_arch STREQUAL "")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_arch "x64")
        elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
            set(_arch "x86")
        endif()
    endif()

    set(${out_var} "${_arch}" PARENT_SCOPE)
    set(${out_use_dll_import_var} "${_use_dll_import}" PARENT_SCOPE)
endfunction()

set(_webview2_package_hint "")

if(DEFINED WebView2_root_dir AND NOT WebView2_root_dir STREQUAL "")
    set(_webview2_package_hint "${WebView2_root_dir}")
elseif(JUCE_WEBVIEW2_PACKAGE_LOCATION)
    set(_webview2_package_hint "${JUCE_WEBVIEW2_PACKAGE_LOCATION}")
else()
    set(_webview2_package_hint "$ENV{USERPROFILE}/AppData/Local/PackageManagement/NuGet/Packages")
endif()

set(_webview2_search_dir "")

if(EXISTS "${_webview2_package_hint}/build/native/include/WebView2.h")
    set(_webview2_search_dir "${_webview2_package_hint}")
else()
    file(GLOB _webview2_subdirs "${_webview2_package_hint}/*Microsoft.Web.WebView2*")

    if(_webview2_subdirs)
        list(GET _webview2_subdirs 0 _webview2_search_dir)
        list(LENGTH _webview2_subdirs _num_webview2_packages)

        if(_num_webview2_packages GREATER 1)
            message(WARNING "Multiple WebView2 packages found. Proceeding with ${_webview2_search_dir}.")
        endif()
    endif()
endif()

if(NOT _webview2_search_dir STREQUAL "")
    find_path(WebView2_root_dir build/native/include/WebView2.h HINTS "${_webview2_search_dir}")

    if(WebView2_root_dir)
        set(WebView2_include_dir "${WebView2_root_dir}/build/native/include")
        guitarfx_webview2_detect_arch(WebView2_arch WebView2_use_dll_import)

        if(NOT WebView2_arch STREQUAL "")
            if(WebView2_use_dll_import)
                set(WebView2_library "${WebView2_root_dir}/build/native/${WebView2_arch}/WebView2Loader.dll.lib")
                set(WebView2_loader_dll "${WebView2_root_dir}/build/native/${WebView2_arch}/WebView2Loader.dll")
            else()
                set(WebView2_library "${WebView2_root_dir}/build/native/${WebView2_arch}/WebView2LoaderStatic.lib")
            endif()
        endif()
    endif()
elseif(NOT WebView2_FIND_QUIETLY)
    message(WARNING
            "WebView2 wasn't found in the configured package locations.\n"
            "Set JUCE_WEBVIEW2_PACKAGE_LOCATION to the NuGet packages root or WebView2 package directory.")
endif()

find_package_handle_standard_args(WebView2 DEFAULT_MSG WebView2_include_dir WebView2_library)

if(WebView2_FOUND)
    set(WebView2_INCLUDE_DIRS "${WebView2_include_dir}")
    set(WebView2_LIBRARIES "${WebView2_library}")

    mark_as_advanced(WebView2_library WebView2_include_dir WebView2_root_dir WebView2_loader_dll)

    if(NOT TARGET juce_webview2)
        add_library(juce_webview2 INTERFACE)
        add_library(juce::juce_webview2 ALIAS juce_webview2)
        target_include_directories(juce_webview2 INTERFACE "${WebView2_INCLUDE_DIRS}")
        target_link_libraries(juce_webview2 INTERFACE "${WebView2_LIBRARIES}")
    endif()

    if(WebView2_use_dll_import)
        message(STATUS "WebView2 loader architecture: ${WebView2_arch} (dynamic import)")
    else()
        message(STATUS "WebView2 static loader architecture: ${WebView2_arch}")
    endif()
endif()