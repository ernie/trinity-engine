CMAKE_MINIMUM_REQUIRED(VERSION 3.12)

PROJECT(trinity)

# Release builds require immutable dependency inputs.
option(TRINITY_RELEASE_BUILD "Build pinned Windows/Linux release runtime dependencies" OFF)
if(TRINITY_RELEASE_BUILD)
    if(CMAKE_VERSION VERSION_LESS 3.20)
        message(FATAL_ERROR "Release dependency builds require CMake 3.20 or newer")
    endif()
    if(EMSCRIPTEN OR APPLE)
        message(FATAL_ERROR "Bundled VR release dependencies are validated only on Windows/Linux")
    endif()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/misc/release-dependencies.json" RELEASE_DEPS)
    string(JSON TRINITY_SOURCE_REVISION GET "${RELEASE_DEPS}" trinity_revision)
    string(JSON TRINITY_OPENXR_REVISION GET "${RELEASE_DEPS}" openxr_revision)
    if(NOT TRINITY_SOURCE_REVISION)
        message(FATAL_ERROR "Release blocked: publish and pin the reviewed Trinity revision in misc/release-dependencies.json")
    endif()
    set(BUILD_TRINITY_NATIVE_FALLBACK ON CACHE BOOL "" FORCE)
    set(BUILD_OPENXR_LOADER ON CACHE BOOL "" FORCE)
    set(TRINITY_SOURCE_REVISION "${TRINITY_SOURCE_REVISION}" CACHE STRING "Pinned release mod" FORCE)
    set(TRINITY_OPENXR_REVISION "${TRINITY_OPENXR_REVISION}" CACHE STRING "Pinned release loader" FORCE)
endif()

SET(CNAME "trinity")
SET(DNAME "trinity.ded")

SET(RENDERER_PREFIX ${CNAME})
SET(RENDERER_DEFAULT vulkan) # valid options: opengl, vulkan, opengl2

OPTION(USE_SDL "" ON)
OPTION(USE_CURL "" ON)
OPTION(USE_LOCAL_HEADERS "" ON)
OPTION(USE_VULKAN "" ON)
OPTION(USE_SYSTEM_JPEG "" OFF)
OPTION(USE_RENDERER_DLOPEN "" ON)
OPTION(USE_VOIP "" ON)
OPTION(USE_OPENAL "" ON)
OPTION(USE_OPENAL_DLOPEN "" ON)
OPTION(TRINITY_FRAME "Steam Frame package: launches in VR without the desktop mirror" OFF)
IF(TRINITY_FRAME)
	add_compile_definitions(TRINITY_FRAME)
ENDIF()

# ===== Emscripten/WebAssembly support =====
IF(EMSCRIPTEN)
	SET(USE_VULKAN OFF)
	SET(USE_RENDERER_DLOPEN OFF)
	SET(USE_CURL OFF)
	SET(USE_OPENAL ON)
	SET(USE_OPENAL_DLOPEN OFF)
	SET(RENDERER_DEFAULT opengl2)
	SET(BASEGAME "baseq3")
	SET(BINEXT "")
	SET(RENDEXT "")
	SET(CMAKE_EXECUTABLE_SUFFIX ".js")
	# Disable LTO since Emscripten libraries aren't LTO enabled
	SET(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
	option(EMSCRIPTEN_PRELOAD_FILE "Preload game files into .data file" OFF)
ENDIF()

# Get version from CI tag or git
if(DEFINED ENV{GITHUB_REF_NAME} AND "$ENV{GITHUB_REF_NAME}" MATCHES "^v[0-9]")
	set(TRINITY_ENGINE_VERSION "$ENV{GITHUB_REF_NAME}")
else()
	execute_process(
		COMMAND git describe --tags --always --dirty
		WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
		OUTPUT_VARIABLE TRINITY_ENGINE_VERSION
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE GIT_RESULT
	)
	if(NOT GIT_RESULT EQUAL 0 OR NOT TRINITY_ENGINE_VERSION)
		set(TRINITY_ENGINE_VERSION "unknown")
	endif()
endif()
add_compile_definitions(TRINITY_ENGINE_VERSION="${TRINITY_ENGINE_VERSION}")

SET(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake_modules)

AUX_SOURCE_DIRECTORY(code/qcommon QCOMMON_SRCS)
# exclude platform-dependent QVM bytecode compilers and the VM policy file
list(FILTER QCOMMON_SRCS EXCLUDE REGEX ".*vm_[alpvx].*.c")
list(APPEND QCOMMON_SRCS code/qcommon/vm_vr.c code/qcommon/vm_vr_select.c code/qcommon/vm_vr_state.c code/vrcommon/vr_shared_sync.c)
# add platform-dependent QVM bytecode compilers
IF(EMSCRIPTEN)
	# Emscripten uses interpreted VM only - no native bytecode compiler
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES AMD64|x86|i*86)
	IF(CMAKE_SYSTEM_PROCESSOR MATCHES AMD64)
		IF(WIN32)
			SET(BINEXT .x64)
		ELSE()
			SET(BINEXT .x86_64)
		ENDIF()
		SET(RENDEXT _x86_64)
	ELSE()
		#SET(BINEXT .x86)
		SET(RENDEXT _x86)
	ENDIF()
	list(APPEND QCOMMON_SRCS code/qcommon/vm_x86.c)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES aarch64|arm64)
	SET(BINEXT .aarch64)
	SET(RENDEXT _aarch64)
	list(APPEND QCOMMON_SRCS code/qcommon/vm_aarch64.c)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES arm*)
	SET(BINEXT .arm)
	SET(RENDEXT _arm)
	list(APPEND QCOMMON_SRCS code/qcommon/vm_armv7l.c)
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le|ppc64")
	IF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64le")
		SET(BINEXT .ppc64le)
		SET(RENDEXT _ppc64le)
		list(APPEND QCOMMON_SRCS code/qcommon/vm_powerpc.c)
	ELSE()
		SET(BINEXT .ppc64)
		SET(RENDEXT _ppc64)
		list(APPEND QCOMMON_SRCS code/qcommon/vm_powerpc.c)
	ENDIF()
	ADD_COMPILE_OPTIONS(-mcpu=power8 -mvsx)
ELSE()
	TARGET_COMPILE_DEFINITIONS(qcommon PRIVATE -DNO_VM_COMPILED)
	TARGET_COMPILE_DEFINITIONS(qcommon_ded PRIVATE -DNO_VM_COMPILED)
ENDIF()

# botlib
AUX_SOURCE_DIRECTORY(code/botlib BOTLIB_SRCS)
ADD_LIBRARY(botlib OBJECT ${BOTLIB_SRCS})
TARGET_COMPILE_DEFINITIONS(botlib PRIVATE BOTLIB)

AUX_SOURCE_DIRECTORY(code/server SERVER_SRCS)
list(REMOVE_ITEM SERVER_SRCS code/server/sv_rankings.c)

# zstd
SET(ZSTD_SRCS code/libzstd/zstd.c)
INCLUDE_DIRECTORIES(code/libzstd)

# opus (for VOIP)
IF(USE_VOIP)
	SET(INTERNAL_OPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/code/thirdparty/opus-1.5.2)
	FILE(GLOB_RECURSE OPUS_SOURCES ${INTERNAL_OPUS_DIR}/*.c)
	ADD_LIBRARY(opus OBJECT ${OPUS_SOURCES})
	TARGET_COMPILE_DEFINITIONS(opus PRIVATE OPUS_BUILD HAVE_LRINTF FLOATING_POINT FLOAT_APPROX USE_ALLOCA)
	# Find all include directories in opus tree
	FILE(GLOB OPUS_INCLUDE_DIRS ${INTERNAL_OPUS_DIR}/include ${INTERNAL_OPUS_DIR}/celt ${INTERNAL_OPUS_DIR}/silk ${INTERNAL_OPUS_DIR}/silk/float ${INTERNAL_OPUS_DIR}/src)
	TARGET_INCLUDE_DIRECTORIES(opus PRIVATE ${OPUS_INCLUDE_DIRS})
	# Suppress warnings on third-party code
	IF(MSVC)
		TARGET_COMPILE_OPTIONS(opus PRIVATE /w)
	ELSE()
		TARGET_COMPILE_OPTIONS(opus PRIVATE -w)
	ENDIF()

	SET(VOIP_CLIENT_SRCS code/client/cl_voip.c)
	SET(VOIP_SERVER_SRCS code/server/sv_voip.c)
ENDIF()

# universal binary
ADD_LIBRARY(qcommon OBJECT ${QCOMMON_SRCS} ${SERVER_SRCS} ${ZSTD_SRCS})

# dedicated server
ADD_LIBRARY(qcommon_ded OBJECT ${QCOMMON_SRCS} ${SERVER_SRCS} ${ZSTD_SRCS})
TARGET_COMPILE_DEFINITIONS(qcommon_ded PUBLIC DEDICATED)

IF(USE_VOIP)
	TARGET_COMPILE_DEFINITIONS(qcommon PRIVATE USE_VOIP)
	TARGET_COMPILE_DEFINITIONS(qcommon_ded PRIVATE USE_VOIP)
ENDIF()

IF(EMSCRIPTEN)
	TARGET_COMPILE_DEFINITIONS(qcommon PRIVATE NO_VM_COMPILED)
	TARGET_COMPILE_DEFINITIONS(qcommon_ded PRIVATE NO_VM_COMPILED)
ELSEIF(NO_VM_COMPILED)
	TARGET_COMPILE_DEFINITIONS(qcommon PRIVATE NO_VM_COMPILED)
	TARGET_COMPILE_DEFINITIONS(qcommon_ded PRIVATE NO_VM_COMPILED)
ENDIF()

# client + cURL
AUX_SOURCE_DIRECTORY(code/client CLIENT_SRCS)
IF(MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64)$")
	# snd_mix.c calls the x64 assembly mixer on non-MSVC compilers. Match
	# Make's preprocessing flags (ELF here selects undecorated symbol names).
	ENABLE_LANGUAGE(ASM)
	list(APPEND CLIENT_SRCS code/asm/snd_mix_x86_64.s)
	SET_SOURCE_FILES_PROPERTIES(code/asm/snd_mix_x86_64.s PROPERTIES
		COMPILE_OPTIONS "-x;assembler-with-cpp;-DELF")
ENDIF()
list(APPEND CLIENT_SRCS code/vrcommon/vr_cvars.c code/vrcommon/xr_loader.c)
IF(NOT USE_CURL)
	list(REMOVE_ITEM CLIENT_SRCS code/client/cl_curl.c)
ENDIF()
# VOIP and OpenAL source files are conditionally included
IF(NOT USE_VOIP)
	list(REMOVE_ITEM CLIENT_SRCS code/client/cl_voip.c)
ENDIF()
IF(NOT USE_OPENAL)
	list(REMOVE_ITEM CLIENT_SRCS code/client/snd_openal.c code/client/qal.c)
ENDIF()
# Discord Rich Presence is compiled out on Emscripten; call sites are neutralized by cl_discord.h
IF(EMSCRIPTEN)
	list(REMOVE_ITEM CLIENT_SRCS code/client/cl_discord.c code/client/cl_discord_proto.c)
ENDIF()

#ADD_LIBRARY(client OBJECT ${CLIENT_SRCS})

# jpeg
IF(USE_SYSTEM_JPEG)
	ADD_LIBRARY(client OBJECT ${CLIENT_SRCS})
	find_package(JPEG REQUIRED)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE -DUSE_SYSTEM_JPEG)
	TARGET_LINK_LIBRARIES(client ${JPEG_LIBRARIES})
ELSE()
	AUX_SOURCE_DIRECTORY(code/libjpeg JPEG_SRCS)
	#ADD_LIBRARY(jpeg ${JPEG_SRCS})
	#TARGET_LINK_LIBRARIES(client jpeg)
	ADD_LIBRARY(client OBJECT ${CLIENT_SRCS} ${JPEG_SRCS})
ENDIF()

# VOIP and OpenAL definitions for client
IF(USE_VOIP)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_VOIP)
	TARGET_INCLUDE_DIRECTORIES(client PRIVATE ${OPUS_INCLUDE_DIRS})
ENDIF()
IF(USE_OPENAL)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_OPENAL)
	IF(USE_OPENAL_DLOPEN)
		TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_OPENAL_DLOPEN)
	ENDIF()
ENDIF()

# renderers
AUX_SOURCE_DIRECTORY(code/renderercommon RENDERER_COMMON_SRCS)
AUX_SOURCE_DIRECTORY(code/renderer RENDERER_GL_SRCS)
AUX_SOURCE_DIRECTORY(code/renderervk RENDERER_VK_SRCS)
IF(USE_RENDERER_DLOPEN)
	list(APPEND RENDERER_VK_SRCS code/vrcommon/xr_loader.c)
ENDIF()

# ===== Renderer2 (GL2/GLSL) for Emscripten =====
IF(EMSCRIPTEN)
	# Stringify GLSL shaders to C source files at configure time
	SET(GLSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/code/renderer2/glsl)
	SET(GLSL_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/glsl_generated)
	FILE(MAKE_DIRECTORY ${GLSL_OUT_DIR})

	FILE(GLOB GLSL_FILES ${GLSL_DIR}/*.glsl)
	SET(GLSL_GENERATED_SRCS "")
	FOREACH(GLSL_FILE ${GLSL_FILES})
		GET_FILENAME_COMPONENT(GLSL_NAME ${GLSL_FILE} NAME_WE)
		SET(GLSL_OUT ${GLSL_OUT_DIR}/${GLSL_NAME}.c)
		# Read the GLSL file and generate a C string constant
		FILE(READ ${GLSL_FILE} GLSL_CONTENT)
		# Escape backslashes, then quotes, then convert newlines
		STRING(REPLACE "\\" "\\\\" GLSL_CONTENT "${GLSL_CONTENT}")
		STRING(REPLACE "\"" "\\\"" GLSL_CONTENT "${GLSL_CONTENT}")
		STRING(REPLACE "\n" "\\n\"\n\"" GLSL_CONTENT "${GLSL_CONTENT}")
		FILE(WRITE ${GLSL_OUT} "const char *fallbackShader_${GLSL_NAME} =\n\"${GLSL_CONTENT}\";\n")
		LIST(APPEND GLSL_GENERATED_SRCS ${GLSL_OUT})
	ENDFOREACH()

	# Collect renderer2 sources (excluding stringify.c which is a host tool)
	SET(RENDERER_GL2_SRCS "")
	FILE(GLOB RENDERER_GL2_C_FILES code/renderer2/*.c)
	FOREACH(SRC ${RENDERER_GL2_C_FILES})
		GET_FILENAME_COMPONENT(SRC_NAME ${SRC} NAME)
		IF(NOT SRC_NAME STREQUAL "stringify.c")
			LIST(APPEND RENDERER_GL2_SRCS ${SRC})
		ENDIF()
	ENDFOREACH()

	ADD_LIBRARY(${RENDERER_PREFIX}_opengl2 OBJECT
		${RENDERER_GL2_SRCS}
		${RENDERER_COMMON_SRCS}
		${GLSL_GENERATED_SRCS})
	TARGET_INCLUDE_DIRECTORIES(${RENDERER_PREFIX}_opengl2 PRIVATE ${GLSL_OUT_DIR})
ENDIF()

IF(USE_RENDERER_DLOPEN)
	SET(AUX_SRCS
		code/qcommon/q_shared.c
		code/qcommon/puff.c
		code/qcommon/q_math.c
	)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_RENDERER_DLOPEN RENDERER_PREFIX="${RENDERER_PREFIX}" RENDERER_DEFAULT=${RENDERER_DEFAULT})

	ADD_LIBRARY(${RENDERER_PREFIX}_opengl${RENDEXT} SHARED ${RENDERER_GL_SRCS} ${RENDERER_COMMON_SRCS} ${AUX_SRCS})
	TARGET_COMPILE_DEFINITIONS(${RENDERER_PREFIX}_opengl${RENDEXT} PRIVATE USE_RENDERER_DLOPEN)
	SET_TARGET_PROPERTIES(${RENDERER_PREFIX}_opengl${RENDEXT} PROPERTIES PREFIX "")

	ADD_LIBRARY(${RENDERER_PREFIX}_vulkan${RENDEXT} SHARED ${RENDERER_VK_SRCS} ${RENDERER_COMMON_SRCS} ${AUX_SRCS})
	TARGET_COMPILE_DEFINITIONS(${RENDERER_PREFIX}_vulkan${RENDEXT} PRIVATE USE_RENDERER_DLOPEN)
	SET_TARGET_PROPERTIES(${RENDERER_PREFIX}_vulkan${RENDEXT} PROPERTIES PREFIX "")
	TARGET_LINK_LIBRARIES(${RENDERER_PREFIX}_vulkan${RENDEXT} ${CMAKE_DL_LIBS})
ELSE()
	IF(NOT EMSCRIPTEN)
		IF(USE_VULKAN)
			ADD_LIBRARY(${RENDERER_PREFIX}_vulkan OBJECT ${RENDERER_VK_SRCS} ${RENDERER_COMMON_SRCS})
		ELSE()
			ADD_LIBRARY(${RENDERER_PREFIX}_opengl OBJECT ${RENDERER_GL_SRCS} ${RENDERER_COMMON_SRCS})
		ENDIF()
	ENDIF()
ENDIF()

# MSVC x64 calls these helpers from qcommon and from dynamically loaded
# renderers. Each DLL needs its own MASM object; a static renderer shares the
# executable's qcommon object and must not add a duplicate definition.
IF(MSVC AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	SET(TRINITY_MSVC_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
	IF(CMAKE_C_COMPILER_ARCHITECTURE_ID)
		SET(TRINITY_MSVC_TARGET_ARCH "${CMAKE_C_COMPILER_ARCHITECTURE_ID}")
	ENDIF()
	IF(CMAKE_GENERATOR_PLATFORM)
		SET(TRINITY_MSVC_TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
	ENDIF()
	STRING(TOLOWER "${TRINITY_MSVC_TARGET_ARCH}" TRINITY_MSVC_TARGET_ARCH)
	IF(TRINITY_MSVC_TARGET_ARCH MATCHES "^(x64|amd64|x86_64)$")
		ENABLE_LANGUAGE(ASM_MASM)
		SET(TRINITY_COMMON_X64_ASM code/asm/common_x64.asm)
		TARGET_SOURCES(qcommon PRIVATE ${TRINITY_COMMON_X64_ASM})
		TARGET_SOURCES(qcommon_ded PRIVATE ${TRINITY_COMMON_X64_ASM})
		IF(USE_RENDERER_DLOPEN)
			TARGET_SOURCES(${RENDERER_PREFIX}_opengl${RENDEXT} PRIVATE ${TRINITY_COMMON_X64_ASM})
			TARGET_SOURCES(${RENDERER_PREFIX}_vulkan${RENDEXT} PRIVATE ${TRINITY_COMMON_X64_ASM})
		ENDIF()
	ENDIF()
ENDIF()

# common function, window system
IF(EMSCRIPTEN)
	SET(Q3_SRCS
		code/unix/unix_main.c
		code/unix/unix_shared.c)

	AUX_SOURCE_DIRECTORY(code/sdl Q3_UI_SRCS)
ELSEIF(UNIX)
	SET (Q3_SRCS
		code/unix/unix_main.c
		code/unix/unix_shared.c
		code/unix/linux_signals.c)

	IF(USE_SDL)
		AUX_SOURCE_DIRECTORY(code/sdl Q3_UI_SRCS)
	ELSE()
		SET(Q3_UI_SRCS
			code/unix/linux_glimp.c
			code/unix/linux_qgl.c
			code/unix/linux_snd.c
			code/unix/x11_dga.c
			code/unix/x11_randr.c
			code/unix/x11_vidmode.c)
		IF(USE_VULKAN)
			list(APPEND Q3_UI_SRCS code/unix/linux_qvk.c)
		ENDIF()
	ENDIF()
	list(APPEND Q3_UI_SRCS code/unix/unix_discord.c)

	IF(APPLE)
		SET(EXE_TYPE MACOSX_BUNDLE)
		list(APPEND Q3_SRCS code/unix/quake3_flat.icns)
		set_source_files_properties(code/unix/quake3_flat.icns PROPERTIES
			MACOSX_PACKAGE_LOCATION Resources)
		SET(MACOSX_BUNDLE_ICON_FILE quake3_flat.icns)
	ENDIF()
ELSEIF(WIN32)
	SET(EXE_TYPE WIN32)
	IF(MSVC)
		SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")
		ADD_COMPILE_DEFINITIONS(_CRT_SECURE_NO_WARNINGS) # v3.12+
	ENDIF()

	SET(Q3_SRCS
		code/win32/win_main.c
		code/win32/win_shared.c
		code/win32/win_syscon.c
		code/win32/win_resource.rc)
	# The resource script names assets relative to the repository root.
	# Supply that search path when building outside the source directory.
	SET_SOURCE_FILES_PROPERTIES(code/win32/win_resource.rc PROPERTIES
		INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")

	IF(USE_SDL)
		# Also supplies the Windows microphone-capture backend, so an SDL/VoIP
		# configuration must not pick up win_snd.c.
		AUX_SOURCE_DIRECTORY(code/sdl Q3_UI_SRCS)
		list(APPEND Q3_UI_SRCS code/win32/win_discord.c)
	ELSE()
		SET(Q3_UI_SRCS
			code/win32/win_gamma.c
			code/win32/win_glimp.c
			code/win32/win_input.c
			code/win32/win_minimize.c
			code/win32/win_qgl.c
			code/win32/win_snd.c
			code/win32/win_wndproc.c
			code/win32/win_discord.c)
		IF(USE_VULKAN)
			list(APPEND Q3_UI_SRCS code/win32/win_qvk.c)
		ENDIF()
	ENDIF()
ENDIF()

IF(NOT APPLE)
	list(REMOVE_ITEM Q3_UI_SRCS code/sdl/sdl_macos_hdr.m)
ENDIF()
ADD_LIBRARY(q3ui OBJECT ${Q3_UI_SRCS})
IF(USE_VOIP)
	TARGET_COMPILE_DEFINITIONS(q3ui PRIVATE USE_VOIP)
	TARGET_INCLUDE_DIRECTORIES(q3ui PRIVATE ${OPUS_INCLUDE_DIRS})
ENDIF()

IF(EMSCRIPTEN)
	# SDL is provided by Emscripten's ports system
	TARGET_COMPILE_OPTIONS(q3ui PRIVATE "SHELL:-s USE_SDL=2")
	TARGET_COMPILE_OPTIONS(client PRIVATE "SHELL:-s USE_SDL=2")
	TARGET_COMPILE_OPTIONS(${RENDERER_PREFIX}_opengl2 PRIVATE "SHELL:-s USE_SDL=2")
ELSE()
	IF(USE_SDL)
		find_package(SDL2 REQUIRED)
		TARGET_LINK_LIBRARIES(q3ui SDL2::Main)
	ELSEIF(UNIX)
		find_package (X11 REQUIRED)
		TARGET_LINK_LIBRARIES(q3ui ${X11_LIBRARIES})
	ENDIF()
ENDIF()

IF(USE_VULKAN)
	TARGET_COMPILE_DEFINITIONS(q3ui PRIVATE USE_VULKAN_API)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_VULKAN_API)
ELSE()
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_OPENGL_API)
ENDIF()

IF(USE_RENDERER_DLOPEN OR NOT USE_VULKAN)
	TARGET_COMPILE_DEFINITIONS(q3ui PRIVATE USE_OPENGL_API)
	TARGET_COMPILE_DEFINITIONS(client PRIVATE USE_OPENGL_API)
ENDIF()

# client executable

SET(VOIP_LINK_SRCS "")
IF(USE_VOIP)
	SET(VOIP_LINK_SRCS ${VOIP_SERVER_SRCS})
ENDIF()

ADD_EXECUTABLE(${CNAME}${BINEXT} ${EXE_TYPE} ${Q3_SRCS} ${VOIP_LINK_SRCS})
option(BUILD_OPENXR_LOADER "Build the optional bundled Khronos OpenXR loader" OFF)
if(WIN32 AND BUILD_TRINITY_NATIVE_FALLBACK AND CMAKE_VERSION VERSION_LESS 3.21)
	message(FATAL_ERROR "Windows native fallback installation requires CMake 3.21 for runtime dependency packaging")
endif()
if(BUILD_OPENXR_LOADER AND NOT EMSCRIPTEN)
	set(TRINITY_OPENXR_OUTPUT_DIR "$<TARGET_FILE_DIR:${CNAME}${BINEXT}>" CACHE PATH "Loader output directory" FORCE)
	if(APPLE)
		set(TRINITY_OPENXR_INSTALL_DIR "${CNAME}${BINEXT}.app/Contents/MacOS" CACHE STRING "Loader install directory" FORCE)
	endif()
	if(WIN32 AND BUILD_TRINITY_NATIVE_FALLBACK)
		set(TRINITY_BUNDLE_RUNTIME_DEPENDENCIES ON)
	endif()
	add_subdirectory(misc/openxr-loader openxr-loader)
	add_dependencies(${CNAME}${BINEXT} openxr_loader)
	# The SDK keeps its Visual Studio output directory for its own post-build
	# commands. Stage that result next to the engine for direct solution builds.
	if(CMAKE_GENERATOR MATCHES "^Visual Studio")
		add_custom_command(TARGET ${CNAME}${BINEXT} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				"$<TARGET_FILE:openxr_loader>" "$<TARGET_FILE_DIR:${CNAME}${BINEXT}>"
			VERBATIM)
	endif()
endif()
set(TRINITY_NATIVE_OUTPUT_DIR "$<TARGET_FILE_DIR:${CNAME}${BINEXT}>")
if(APPLE)
	set(TRINITY_NATIVE_INSTALL_DIR "${CNAME}${BINEXT}.app/Contents/MacOS")
endif()
include(TrinityNative)
if(TARGET trinity-native)
	add_dependencies(${CNAME}${BINEXT} trinity-native)
	# Keep the trusted module directory and dynamic renderers adjacent to the
	# installed executable. A native-enabled prefix is a runnable client.
	if(WIN32)
		install(TARGETS ${CNAME}${BINEXT}
			RUNTIME_DEPENDENCY_SET trinity_client_dependencies
			RUNTIME DESTINATION . BUNDLE DESTINATION .)
	else()
		install(TARGETS ${CNAME}${BINEXT} RUNTIME DESTINATION . BUNDLE DESTINATION .)
	endif()
	if(USE_RENDERER_DLOPEN)
		if(WIN32)
			install(TARGETS ${RENDERER_PREFIX}_opengl${RENDEXT}
				${RENDERER_PREFIX}_vulkan${RENDEXT}
				RUNTIME_DEPENDENCY_SET trinity_client_dependencies
				RUNTIME DESTINATION . LIBRARY DESTINATION .)
		else()
			install(TARGETS ${RENDERER_PREFIX}_opengl${RENDEXT}
				${RENDERER_PREFIX}_vulkan${RENDEXT}
				RUNTIME DESTINATION . LIBRARY DESTINATION .)
		endif()
	endif()
	if(WIN32)
		set(TRINITY_RUNTIME_DEPENDENCY_DIRECTORIES "")
		if(USE_SDL AND TARGET SDL2::Core)
			get_target_property(TRINITY_SDL2_LIBRARY SDL2::Core IMPORTED_LOCATION)
			get_filename_component(TRINITY_SDL2_LIBRARY_DIR "${TRINITY_SDL2_LIBRARY}" DIRECTORY)
			list(APPEND TRINITY_RUNTIME_DEPENDENCY_DIRECTORIES
				DIRECTORIES "${TRINITY_SDL2_LIBRARY_DIR}"
					"${TRINITY_SDL2_LIBRARY_DIR}/../bin"
					"${TRINITY_SDL2_LIBRARY_DIR}/../../bin")
		endif()
		install(RUNTIME_DEPENDENCY_SET trinity_client_dependencies
			${TRINITY_RUNTIME_DEPENDENCY_DIRECTORIES}
			PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
			POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]"
			RUNTIME DESTINATION .)
	endif()
endif()
TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} qcommon botlib client q3ui)
IF(USE_VOIP)
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} opus)
ENDIF()
#TARGET_LINK_OPTIONS(${CNAME} PRIVATE -fno-pie) # v3.13+

IF(EMSCRIPTEN)
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} ${RENDERER_PREFIX}_opengl2)
	TARGET_LINK_OPTIONS(${CNAME}${BINEXT} PRIVATE
		"SHELL:-s USE_SDL=2"
		"SHELL:-s TOTAL_MEMORY=256MB"
		"SHELL:-s STACK_SIZE=5MB"
		"SHELL:-s MIN_WEBGL_VERSION=2"
		"SHELL:-s MAX_WEBGL_VERSION=2"
		"SHELL:-s EXPORTED_RUNTIME_METHODS=['FS','addRunDependency','removeRunDependency','ccall','HEAPU8']"
		"SHELL:-s EXPORTED_FUNCTIONS=['_main','_exit','_Cbuf_AddText','_CL_TV_GetPlayerList','_CL_TV_FeedBytes','_Cvar_VariableValue','_Cvar_VariableString','_malloc','_free']"
		"SHELL:-s EXIT_RUNTIME=1"
		"SHELL:-s EXPORT_ES6"
		"SHELL:-s EXPORT_NAME=TrinityEngine"
		"SHELL:-s ALLOW_MEMORY_GROWTH"
		"SHELL:-s FORCE_FILESYSTEM"
	)
	IF(EMSCRIPTEN_PRELOAD_FILE)
		IF(NOT EXISTS "${CMAKE_SOURCE_DIR}/${BASEGAME}")
			MESSAGE(FATAL_ERROR "No files in '${BASEGAME}' directory for emscripten to preload.")
		ENDIF()
		TARGET_LINK_OPTIONS(${CNAME}${BINEXT} PRIVATE
			"SHELL:--preload-file ${BASEGAME}")
	ENDIF()

	# Deploy HTML template and config
	SET(CLIENT_NAME ${CNAME})
	CONFIGURE_FILE(
		${CMAKE_CURRENT_SOURCE_DIR}/code/web/client.html.in
		${CMAKE_CURRENT_BINARY_DIR}/${CNAME}.html
		@ONLY)
	CONFIGURE_FILE(
		${CMAKE_CURRENT_SOURCE_DIR}/code/web/loader.js.in
		${CMAKE_CURRENT_BINARY_DIR}/${CNAME}-loader.js
		@ONLY)
ELSEIF(NOT USE_RENDERER_DLOPEN)
	IF(USE_VULKAN)
		TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} ${RENDERER_PREFIX}_vulkan)
	ELSE()
		TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} ${RENDERER_PREFIX}_opengl)
	ENDIF()
ENDIF()

# dedicated server executable

IF(NOT EMSCRIPTEN)
	SET(DED_VOIP_SRCS "")
	IF(USE_VOIP)
		SET(DED_VOIP_SRCS ${VOIP_SERVER_SRCS})
	ENDIF()
	ADD_EXECUTABLE(${DNAME}${BINEXT} ${EXE_TYPE} ${Q3_SRCS} ${DED_VOIP_SRCS})
	TARGET_LINK_LIBRARIES(${DNAME}${BINEXT} qcommon_ded botlib)
	#TARGET_LINK_OPTIONS(${DNAME} PRIVATE -fno-pie) # v3.13+
ENDIF()

IF(EMSCRIPTEN)
	# No additional system libraries needed for Emscripten
ELSEIF(WIN32)
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} winmm comctl32 ws2_32 iphlpapi)
	TARGET_LINK_LIBRARIES(${DNAME}${BINEXT} winmm comctl32 ws2_32 iphlpapi)
	# Copy OpenAL Soft DLL next to client executable
	IF(USE_OPENAL AND USE_OPENAL_DLOPEN)
		IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
			SET(OPENAL_DLL ${CMAKE_CURRENT_SOURCE_DIR}/code/libopenal/windows/mingw/lib64/OpenAL64.dll)
		ELSE()
			SET(OPENAL_DLL ${CMAKE_CURRENT_SOURCE_DIR}/code/libopenal/windows/mingw/lib32/OpenAL32.dll)
		ENDIF()
		ADD_CUSTOM_COMMAND(TARGET ${CNAME}${BINEXT} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENAL_DLL}" "$<TARGET_FILE_DIR:${CNAME}${BINEXT}>")
		IF(TARGET trinity-native)
			install(FILES "${OPENAL_DLL}" DESTINATION .)
		ENDIF()
	ENDIF()
ELSE()
	TARGET_LINK_LIBRARIES(${CNAME}${BINEXT} m ${CMAKE_DL_LIBS})
	TARGET_LINK_LIBRARIES(${DNAME}${BINEXT} m ${CMAKE_DL_LIBS})
ENDIF()
