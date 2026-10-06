cmake_minimum_required(VERSION 3.20)
project(TrinityOpenXRLoader LANGUAGES C CXX)

# The pinned revision must agree with the headers in code/thirdparty/openxr.
# Explicit local sources support offline builds.
set(TRINITY_OPENXR_SOURCE_DIR "" CACHE PATH "Local Khronos OpenXR-SDK source")
set(TRINITY_OPENXR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/runtime" CACHE PATH "Loader output directory")
set(TRINITY_OPENXR_INSTALL_DIR "." CACHE STRING "Loader install directory")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../release-dependencies.json" TRINITY_DEPENDENCIES)
string(JSON TRINITY_OPENXR_DEFAULT_REVISION GET "${TRINITY_DEPENDENCIES}" openxr_revision)
set(TRINITY_OPENXR_REVISION "${TRINITY_OPENXR_DEFAULT_REVISION}" CACHE STRING "Immutable OpenXR-SDK commit")
string(LENGTH "${TRINITY_OPENXR_REVISION}" REVISION_LENGTH)
if(NOT REVISION_LENGTH EQUAL 40 OR NOT TRINITY_OPENXR_REVISION MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "TRINITY_OPENXR_REVISION must be a full immutable commit")
endif()
if(NOT TRINITY_OPENXR_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(trinity_openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG "${TRINITY_OPENXR_REVISION}")
    FetchContent_GetProperties(trinity_openxr_sdk)
    if(NOT trinity_openxr_sdk_POPULATED)
        FetchContent_Populate(trinity_openxr_sdk)
    endif()
    set(TRINITY_OPENXR_SOURCE_DIR "${trinity_openxr_sdk_SOURCE_DIR}")
endif()

if(TRINITY_RELEASE_BUILD)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${TRINITY_OPENXR_SOURCE_DIR}"
        OUTPUT_VARIABLE ACTUAL_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${TRINITY_OPENXR_SOURCE_DIR}"
        OUTPUT_VARIABLE SOURCE_CHANGES OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT ACTUAL_REVISION STREQUAL TRINITY_OPENXR_REVISION OR SOURCE_CHANGES)
        message(FATAL_ERROR "Release OpenXR source must be clean and match TRINITY_OPENXR_REVISION")
    endif()
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
# The engine drives OpenXR through Vulkan only; XCB support would need xcb/glx.h whenever any xcb.pc is installed.
set(BUILD_WITH_XCB_HEADERS OFF CACHE BOOL "" FORCE)
# The engine loads this exact basename in every configuration. The SDK's
# Windows default adds "d" in Debug, which makes that build undiscoverable.
set(OPENXR_DEBUG_POSTFIX "" CACHE STRING "Engine runtime loader basename" FORCE)
# added here so --target openxr_loader resolves under every generator; EXCLUDE_FROM_ALL keeps the SDK out of the install prefix
add_subdirectory("${TRINITY_OPENXR_SOURCE_DIR}" sdk EXCLUDE_FROM_ALL)
if(MINGW)
    # Avoid private compiler/JSON DLL dependencies in the game distribution.
    target_link_options(openxr_loader PRIVATE -static)
endif()
if(WIN32)
    set_target_properties(openxr_loader PROPERTIES PREFIX "")
endif()
# The SDK Visual Studio post-build commands copy from its original output
# directory, even with tests disabled. Keep that location and let install
# stage the loader; redirecting it makes those commands fail after linking.
if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
    set_target_properties(openxr_loader PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${TRINITY_OPENXR_OUTPUT_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${TRINITY_OPENXR_OUTPUT_DIR}")
endif()
if(TRINITY_BUNDLE_RUNTIME_DEPENDENCIES)
    install(TARGETS openxr_loader
        RUNTIME_DEPENDENCY_SET trinity_client_dependencies
        RUNTIME DESTINATION "${TRINITY_OPENXR_INSTALL_DIR}" COMPONENT TrinityOpenXRLoader
        LIBRARY DESTINATION "${TRINITY_OPENXR_INSTALL_DIR}" COMPONENT TrinityOpenXRLoader)
else()
    install(TARGETS openxr_loader
        RUNTIME DESTINATION "${TRINITY_OPENXR_INSTALL_DIR}" COMPONENT TrinityOpenXRLoader
        LIBRARY DESTINATION "${TRINITY_OPENXR_INSTALL_DIR}" COMPONENT TrinityOpenXRLoader)
endif()
install(FILES "${TRINITY_OPENXR_SOURCE_DIR}/LICENSE"
    DESTINATION licenses/openxr COMPONENT TrinityOpenXRLoader)
install(FILES "${TRINITY_OPENXR_SOURCE_DIR}/src/external/jsoncpp/LICENSE"
    DESTINATION licenses/openxr RENAME JSONCPP-LICENSE COMPONENT TrinityOpenXRLoader)
