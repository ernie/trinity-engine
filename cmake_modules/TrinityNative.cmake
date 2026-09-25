# Trusted native cgame/UI fallback, compiled from the Trinity source manifest.
# Include after the engine executable target exists, setting
# TRINITY_NATIVE_OUTPUT_DIR to $<TARGET_FILE_DIR:engine-target> if necessary.
option(BUILD_TRINITY_NATIVE_FALLBACK "Build bundled dual-mode cgame/UI libraries" OFF)
if(NOT BUILD_TRINITY_NATIVE_FALLBACK OR EMSCRIPTEN)
    return()
endif()
option(TRINITY_RELEASE_BUILD "Require clean pinned dependency sources and matching ABI" OFF)
set(TRINITY_SOURCE_DIR "" CACHE PATH "Explicit local Trinity checkout for native fallback")
set(TRINITY_SOURCE_REVISION "" CACHE STRING "Immutable 40-character Trinity commit for CI")
if(NOT TRINITY_SOURCE_DIR)
    string(LENGTH "${TRINITY_SOURCE_REVISION}" REVISION_LENGTH)
    if(NOT REVISION_LENGTH EQUAL 40 OR NOT TRINITY_SOURCE_REVISION MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "Set TRINITY_SOURCE_DIR or a full pinned TRINITY_SOURCE_REVISION; native code must not track a moving branch.")
    endif()
    include(FetchContent)
    FetchContent_Declare(trinity_native_source
        GIT_REPOSITORY https://github.com/ernie/trinity.git
        GIT_TAG "${TRINITY_SOURCE_REVISION}")
    FetchContent_GetProperties(trinity_native_source)
    if(NOT trinity_native_source_POPULATED)
        FetchContent_Populate(trinity_native_source)
    endif()
    set(TRINITY_SOURCE_DIR "${trinity_native_source_SOURCE_DIR}")
endif()
if(TRINITY_RELEASE_BUILD)
    string(LENGTH "${TRINITY_SOURCE_REVISION}" REVISION_LENGTH)
    if(NOT REVISION_LENGTH EQUAL 40 OR NOT TRINITY_SOURCE_REVISION MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "Release native fallback requires a full TRINITY_SOURCE_REVISION even for local sources")
    endif()
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${TRINITY_SOURCE_DIR}"
        OUTPUT_VARIABLE ACTUAL_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${TRINITY_SOURCE_DIR}"
        OUTPUT_VARIABLE SOURCE_CHANGES OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT ACTUAL_REVISION STREQUAL TRINITY_SOURCE_REVISION OR SOURCE_CHANGES)
        message(FATAL_ERROR "Release native source must be clean and exactly match TRINITY_SOURCE_REVISION")
    endif()
endif()
if(NOT DEFINED TRINITY_NATIVE_OUTPUT_DIR)
    set(TRINITY_NATIVE_OUTPUT_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Executable directory receiving the native modules in baseq3 and missionpack")
endif()
if(NOT DEFINED TRINITY_NATIVE_INSTALL_DIR)
    set(TRINITY_NATIVE_INSTALL_DIR ".")
endif()

set(TRINITY_SRCS_MK "${TRINITY_SOURCE_DIR}/build/srcs.mk")
if(NOT EXISTS "${TRINITY_SRCS_MK}")
    message(FATAL_ERROR
        "BUILD_TRINITY_NATIVE_FALLBACK=ON but ${TRINITY_SRCS_MK} does not exist - "
        "TRINITY_SOURCE_DIR does not look like a trinity checkout.")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TRINITY_SRCS_MK}")

set(TRINITY_CODE_DIR  "${TRINITY_SOURCE_DIR}/code")
set(TRINITY_GAME_DIR  "${TRINITY_CODE_DIR}/game")
set(TRINITY_CGAME_DIR "${TRINITY_CODE_DIR}/cgame")
set(TRINITY_Q3UI_DIR  "${TRINITY_CODE_DIR}/q3_ui")
set(TRINITY_UI_DIR    "${TRINITY_CODE_DIR}/ui")

# Parse srcs.mk: three variables (QA_SRC, CG_SRC, UI_SRC) holding flat
# basename lists with backslash continuations plus $(DIR)/x_syscalls.asm
# entries, where CG_SRC and UI_SRC each appear in both arms of a single
# ifeq ($(CONFIG),missionpack)/else/endif conditional. Exactly that shape;
# anything else is a configure error.
function(trinity_parse_manifest MANIFEST)
    file(READ "${MANIFEST}" CONTENT)
    string(REPLACE "\r\n" "\n" CONTENT "${CONTENT}")
    # Make splices backslash-newline into a single line.
    string(REPLACE "\\\n" " " CONTENT "${CONTENT}")
    string(REPLACE ";" "\\;" CONTENT "${CONTENT}")
    string(REPLACE "\n" ";" MANIFEST_LINES "${CONTENT}")

    set(ARM shared) # shared | missionpack | baseq3
    foreach(VAR QA_SRC CG_SRC UI_SRC)
        foreach(SUFFIX shared missionpack baseq3)
            set(TOKENS_${VAR}_${SUFFIX} "")
        endforeach()
    endforeach()

    foreach(LINE IN LISTS MANIFEST_LINES)
        string(STRIP "${LINE}" LINE)

        if(LINE STREQUAL "" OR LINE MATCHES "^#")
            continue()
        elseif(LINE MATCHES "^ifeq")
            if(NOT LINE MATCHES "^ifeq[ \t]*\\(\\$\\(CONFIG\\),missionpack\\)$")
                message(FATAL_ERROR
                    "${MANIFEST}: unrecognized conditional '${LINE}' - this "
                    "parser only understands ifeq (\$(CONFIG),missionpack).")
            endif()
            if(NOT ARM STREQUAL "shared")
                message(FATAL_ERROR "${MANIFEST}: nested conditional is unsupported")
            endif()
            set(ARM missionpack)
        elseif(LINE STREQUAL "else")
            if(NOT ARM STREQUAL "missionpack")
                message(FATAL_ERROR "${MANIFEST}: unexpected else")
            endif()
            set(ARM baseq3)
        elseif(LINE STREQUAL "endif")
            if(NOT ARM STREQUAL "baseq3")
                message(FATAL_ERROR "${MANIFEST}: unexpected endif")
            endif()
            set(ARM shared)
        elseif(LINE MATCHES "^(QA_SRC|CG_SRC|UI_SRC)[ \t]*=[ \t]*(.*)$")
            set(VAR "${CMAKE_MATCH_1}")
            if((VAR STREQUAL "QA_SRC" AND NOT ARM STREQUAL "shared") OR
               (NOT VAR STREQUAL "QA_SRC" AND ARM STREQUAL "shared"))
                message(FATAL_ERROR "${MANIFEST}: ${VAR} assigned in wrong conditional arm")
            endif()
            if(DEFINED SEEN_${VAR}_${ARM})
                message(FATAL_ERROR "${MANIFEST}: duplicate ${VAR} assignment in ${ARM}")
            endif()
            set(SEEN_${VAR}_${ARM} TRUE)
            string(REGEX MATCHALL "[^ \t]+" TOKENS "${CMAKE_MATCH_2}")
            list(APPEND TOKENS_${VAR}_${ARM} ${TOKENS})
        else()
            message(FATAL_ERROR
                "${MANIFEST}: line not understood: '${LINE}' - the manifest "
                "shape changed; update cmake_modules/TrinityNative.cmake.")
        endif()
    endforeach()

    if(NOT ARM STREQUAL "shared")
        message(FATAL_ERROR "${MANIFEST}: unterminated conditional")
    endif()
    foreach(EXPECTED QA_SRC_shared CG_SRC_missionpack CG_SRC_baseq3
                     UI_SRC_missionpack UI_SRC_baseq3)
        if(NOT DEFINED SEEN_${EXPECTED})
            message(FATAL_ERROR "${MANIFEST}: missing ${EXPECTED} assignment")
        endif()
    endforeach()

    set(TRINITY_QA_SRC             "${TOKENS_QA_SRC_shared}"      PARENT_SCOPE)
    set(TRINITY_CG_SRC_BASEQ3      "${TOKENS_CG_SRC_baseq3}"      PARENT_SCOPE)
    set(TRINITY_CG_SRC_MISSIONPACK "${TOKENS_CG_SRC_missionpack}" PARENT_SCOPE)
    set(TRINITY_UI_SRC_BASEQ3      "${TOKENS_UI_SRC_baseq3}"      PARENT_SCOPE)
    set(TRINITY_UI_SRC_MISSIONPACK "${TOKENS_UI_SRC_missionpack}" PARENT_SCOPE)
endfunction()

# Resolve manifest tokens to source files for one module, applying the
# native delta (*_syscalls.asm -> sibling *_syscalls.c).
#   OUT_VAR     - output list variable
#   MODULE_NAME - for error messages, e.g. "baseq3 cgame"
#   UIDIR       - what $(UIDIR) means for this module's config
#   SEARCH_DIRS - ;-list of directories tried in order for plain basenames
#   ARGN        - manifest tokens
function(trinity_resolve_sources OUT_VAR MODULE_NAME UIDIR SEARCH_DIRS)
    set(RESOLVED "")
    foreach(TOKEN IN LISTS ARGN)
        if(TOKEN MATCHES "^\\$\\((QADIR|CGDIR|UIDIR)\\)/([A-Za-z0-9_]+_syscalls)\\.asm$")
            if(CMAKE_MATCH_1 STREQUAL "QADIR")
                set(DIR "${TRINITY_GAME_DIR}")
            elseif(CMAKE_MATCH_1 STREQUAL "CGDIR")
                set(DIR "${TRINITY_CGAME_DIR}")
            else()
                set(DIR "${UIDIR}")
            endif()
            # Native modules use C syscall wrappers rather than QVM assembly stubs.
            set(FILE "${DIR}/${CMAKE_MATCH_2}.c")
            if(NOT EXISTS "${FILE}")
                message(FATAL_ERROR
                    "trinity manifest (${MODULE_NAME}): '${TOKEN}' has no "
                    "native sibling ${FILE}")
            endif()
            list(APPEND RESOLVED "${FILE}")
            continue()
        endif()
        if(TOKEN MATCHES "\\$")
            message(FATAL_ERROR
                "trinity manifest (${MODULE_NAME}): token '${TOKEN}' uses make "
                "syntax this parser does not understand.")
        endif()
        set(FOUND "")
        foreach(DIR IN LISTS SEARCH_DIRS)
            if(EXISTS "${DIR}/${TOKEN}.c")
                set(FOUND "${DIR}/${TOKEN}.c")
                break()
            endif()
        endforeach()
        if(FOUND STREQUAL "")
            message(FATAL_ERROR
                "trinity manifest (${MODULE_NAME}): '${TOKEN}' is listed in "
                "srcs.mk but ${TOKEN}.c was not found in any of: ${SEARCH_DIRS}")
        endif()
        list(APPEND RESOLVED "${FOUND}")
    endforeach()
    set(${OUT_VAR} "${RESOLVED}" PARENT_SCOPE)
endfunction()

trinity_parse_manifest("${TRINITY_SRCS_MK}")

foreach(LIST_NAME
        TRINITY_CG_SRC_BASEQ3 TRINITY_CG_SRC_MISSIONPACK
        TRINITY_UI_SRC_BASEQ3 TRINITY_UI_SRC_MISSIONPACK)
    if("${${LIST_NAME}}" STREQUAL "")
        message(FATAL_ERROR
            "${TRINITY_SRCS_MK}: parsing produced an empty ${LIST_NAME} - the "
            "manifest shape changed; update cmake_modules/TrinityNative.cmake.")
    endif()
endforeach()

# Directory search order mirrors Makefile.build's pattern rules:
#   game modules:  $(QADIR)
#   cgame modules: $(QADIR), $(CGDIR), $(UIDIR)
#   ui modules:    $(QADIR), $(UIDIR)
# with $(UIDIR) = q3_ui for baseq3 and ui for missionpack.
trinity_resolve_sources(TRINITY_CGAME_SOURCES_BASEQ3 "baseq3 cgame"
    "${TRINITY_Q3UI_DIR}" "${TRINITY_GAME_DIR};${TRINITY_CGAME_DIR};${TRINITY_Q3UI_DIR}"
    ${TRINITY_CG_SRC_BASEQ3})
trinity_resolve_sources(TRINITY_CGAME_SOURCES_MISSIONPACK "missionpack cgame"
    "${TRINITY_UI_DIR}" "${TRINITY_GAME_DIR};${TRINITY_CGAME_DIR};${TRINITY_UI_DIR}"
    ${TRINITY_CG_SRC_MISSIONPACK})
trinity_resolve_sources(TRINITY_UI_SOURCES_BASEQ3 "baseq3 ui"
    "${TRINITY_Q3UI_DIR}" "${TRINITY_GAME_DIR};${TRINITY_Q3UI_DIR}"
    ${TRINITY_UI_SRC_BASEQ3})
trinity_resolve_sources(TRINITY_UI_SOURCES_MISSIONPACK "missionpack ui"
    "${TRINITY_UI_DIR}" "${TRINITY_GAME_DIR};${TRINITY_UI_DIR}"
    ${TRINITY_UI_SRC_MISSIONPACK})

# trinity's Makefile.build compiles with -DTRINITY_VERSION="git describe";
# ui_main.c/ui_menu.c/cg_servercmds.c consume it unconditionally.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${TRINITY_SOURCE_DIR}"
    OUTPUT_VARIABLE TRINITY_MOD_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE TRINITY_MOD_VERSION_RESULT)
if(NOT TRINITY_MOD_VERSION_RESULT EQUAL 0 OR NOT TRINITY_MOD_VERSION)
    set(TRINITY_MOD_VERSION "unknown")
endif()


file(READ "${TRINITY_GAME_DIR}/vr_shared.h" VR_SHARED_HEADER)
if(NOT VR_SHARED_HEADER MATCHES "#define VR_API_MAJOR 1" OR
   NOT VR_SHARED_HEADER MATCHES "#define VR_API_MINOR 0")
    message(FATAL_ERROR "Trinity source does not implement VR API 1.0")
endif()

# A minor version bump alone cannot catch a stale append-only state layout, so
# compare declarations with comments, includes and whitespace stripped.
if(TRINITY_RELEASE_BUILD)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../code/vrcommon/vr_shared.h" ENGINE_VR_HEADER)
    foreach(HEADER ENGINE_VR_HEADER VR_SHARED_HEADER)
        string(REGEX REPLACE "//[^\n]*" "" ${HEADER} "${${HEADER}}")
        string(REGEX REPLACE "#include[^\n]*" "" ${HEADER} "${${HEADER}}")
        string(REGEX REPLACE "[ \t\r\n]+" "" ${HEADER} "${${HEADER}}")
    endforeach()
    if(NOT ENGINE_VR_HEADER STREQUAL VR_SHARED_HEADER)
        message(FATAL_ERROR "Pinned Trinity native VR state declarations differ from the engine ABI")
    endif()
    set(TRINITY_MOD_VERSION "${TRINITY_SOURCE_REVISION}")
endif()

# Names match vm_vr.c and q_platform.h exactly; no library-prefix search.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" NATIVE_PROCESSOR)
if(MSVC AND CMAKE_C_COMPILER_ARCHITECTURE_ID)
    # Ninja + the ARM64 developer prompt has no generator platform.
    string(TOLOWER "${CMAKE_C_COMPILER_ARCHITECTURE_ID}" NATIVE_PROCESSOR)
endif()
if(CMAKE_GENERATOR_PLATFORM)
    string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" NATIVE_PROCESSOR)
    if(NATIVE_PROCESSOR STREQUAL "win32")
        set(NATIVE_PROCESSOR x86)
    endif()
endif()
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    # A universal library is copied under each loader name below.
    list(GET CMAKE_OSX_ARCHITECTURES 0 NATIVE_PROCESSOR)
endif()
if(NATIVE_PROCESSOR MATCHES "^(x86_64|amd64|x64)$")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(NATIVE_ARCH x86_64)
    elseif(WIN32)
        set(NATIVE_ARCH x86)
    else()
        set(NATIVE_ARCH i386)
    endif()
elseif(NATIVE_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    if(WIN32)
        set(NATIVE_ARCH x86)
    else()
        set(NATIVE_ARCH i386)
    endif()
elseif(NATIVE_PROCESSOR MATCHES "^(aarch64|arm64)$")
    if(WIN32)
        set(NATIVE_ARCH arm64)
    else()
        set(NATIVE_ARCH aarch64)
    endif()
elseif(NATIVE_PROCESSOR MATCHES "^arm")
    if(WIN32)
        set(NATIVE_ARCH arm32)
    else()
        set(NATIVE_ARCH arm)
    endif()
elseif(NATIVE_PROCESSOR MATCHES "^(ppc64le|ppc64)$" AND NOT WIN32)
    set(NATIVE_ARCH "${NATIVE_PROCESSOR}")
else()
    message(FATAL_ERROR "Unsupported native fallback architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(NATIVE_ARCH_ALIASES "")
if(APPLE)
    foreach(OSX_ARCH IN LISTS CMAKE_OSX_ARCHITECTURES)
        if(OSX_ARCH STREQUAL "arm64")
            list(APPEND NATIVE_ARCH_ALIASES aarch64)
        elseif(OSX_ARCH STREQUAL "x86_64")
            list(APPEND NATIVE_ARCH_ALIASES x86_64)
        else()
            message(FATAL_ERROR "Unsupported native fallback macOS architecture: ${OSX_ARCH}")
        endif()
    endforeach()
    list(REMOVE_ITEM NATIVE_ARCH_ALIASES "${NATIVE_ARCH}")
endif()

foreach(VARIANT baseq3 missionpack)
    string(TOUPPER "${VARIANT}" UPPER_VARIANT)
    if(VARIANT STREQUAL "missionpack")
        set(VARIANT_UI_DIR "${TRINITY_UI_DIR}")
    else()
        set(VARIANT_UI_DIR "${TRINITY_Q3UI_DIR}")
    endif()
    foreach(MODULE cgame ui)
        string(TOUPPER "${MODULE}" UPPER_MODULE)
        set(TARGET trinity_native_${VARIANT}_${MODULE})
        add_library(${TARGET} SHARED ${TRINITY_${UPPER_MODULE}_SOURCES_${UPPER_VARIANT}})
        if(MODULE STREQUAL "ui" AND VARIANT STREQUAL "baseq3")
            target_compile_definitions(${TARGET} PRIVATE Q3UI TRINITY_VERSION="${TRINITY_MOD_VERSION}")
        else()
            target_compile_definitions(${TARGET} PRIVATE ${UPPER_MODULE} TRINITY_VERSION="${TRINITY_MOD_VERSION}")
        endif()
        if(VARIANT STREQUAL "missionpack")
            target_compile_definitions(${TARGET} PRIVATE MISSIONPACK)
        endif()
        target_include_directories(${TARGET} PRIVATE "${TRINITY_GAME_DIR}" "${TRINITY_CGAME_DIR}" "${VARIANT_UI_DIR}")
        if(NOT WIN32)
            target_link_libraries(${TARGET} PRIVATE m)
        endif()
        if(MINGW)
            target_link_options(${TARGET} PRIVATE -static-libgcc)
        endif()
        set_target_properties(${TARGET} PROPERTIES
            C_STANDARD 99 C_STANDARD_REQUIRED YES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
            PREFIX "" OUTPUT_NAME "${MODULE}${NATIVE_ARCH}"
            RUNTIME_OUTPUT_DIRECTORY "${TRINITY_NATIVE_OUTPUT_DIR}/${VARIANT}"
            LIBRARY_OUTPUT_DIRECTORY "${TRINITY_NATIVE_OUTPUT_DIR}/${VARIANT}")
        install(TARGETS ${TARGET}
            RUNTIME DESTINATION "${TRINITY_NATIVE_INSTALL_DIR}/${VARIANT}" COMPONENT TrinityNative
            LIBRARY DESTINATION "${TRINITY_NATIVE_INSTALL_DIR}/${VARIANT}" COMPONENT TrinityNative)
        foreach(ALIAS IN LISTS NATIVE_ARCH_ALIASES)
            set(ALIAS_NAME "${MODULE}${ALIAS}${CMAKE_SHARED_LIBRARY_SUFFIX}")
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${TARGET}>"
                    "$<TARGET_FILE_DIR:${TARGET}>/${ALIAS_NAME}")
            install(FILES "$<TARGET_FILE:${TARGET}>"
                DESTINATION "${TRINITY_NATIVE_INSTALL_DIR}/${VARIANT}" RENAME "${ALIAS_NAME}" COMPONENT TrinityNative)
        endforeach()
    endforeach()
endforeach()
add_custom_target(trinity-native DEPENDS trinity_native_baseq3_cgame trinity_native_baseq3_ui
    trinity_native_missionpack_cgame trinity_native_missionpack_ui)
