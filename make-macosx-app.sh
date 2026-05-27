#!/bin/bash

# Let's make the user give us a target to work with.
# architecture is assumed universal if not specified, and is optional.
# if arch is defined, it we will store the .app bundle in the target arch build directory
if [ $# == 0 ] || [ $# -gt 2 ]; then
	echo "Usage:   $0 target <arch>"
	echo "Example: $0 release x86"
	echo "Valid targets are:"
	echo " release"
	echo " debug"
	echo
	echo "Optional architectures are:"
	echo " x86"
	echo " x86_64"
	echo " ppc"
	echo " aarch64"
	echo
	exit 1
fi

# validate target name
if [ "$1" == "release" ]; then
	TARGET_NAME="release"
elif [ "$1" == "debug" ]; then
	TARGET_NAME="debug"
else
	echo "Invalid target: $1"
	echo "Valid targets are:"
	echo " release"
	echo " debug"
	exit 1
fi

CURRENT_ARCH=""

# validate the architecture if it was specified
if [ "$2" != "" ]; then
	if [ "$2" == "x86" ]; then
		CURRENT_ARCH="x86"
	elif [ "$2" == "x86_64" ]; then
		CURRENT_ARCH="x86_64"
	elif [ "$2" == "ppc" ]; then
		CURRENT_ARCH="ppc"
	elif [ "$2" == "aarch64" ]; then
		CURRENT_ARCH="aarch64"
	else
		echo "Invalid architecture: $2"
		echo "Valid architectures are:"
		echo " x86"
		echo " x86_64"
		echo " ppc"
		echo " aarch64"
		echo
		exit 1
	fi
fi

# symlinkArch() creates a symlink with the architecture suffix.
# meant for universal binaries, but also handles the way this script generates
# application bundles for a single architecture as well.
function symlinkArch()
{
    EXT="dylib"
    SEP="${3}"
    SRCFILE="${1}"
    DSTFILE="${2}${SEP}"
    DSTPATH="${4}"

    if [ ! -e "${DSTPATH}/${SRCFILE}.${EXT}" ]; then
        echo "**** ERROR: missing ${SRCFILE}.${EXT} from ${MACOS}"
        exit 1
    fi

    if [ ! -d "${DSTPATH}" ]; then
        echo "**** ERROR: path not found ${DSTPATH}"
        exit 1
    fi

    pushd "${DSTPATH}" > /dev/null

    IS32=`file "${SRCFILE}.${EXT}" | grep "i386"`
    IS64=`file "${SRCFILE}.${EXT}" | grep "x86_64"`
    ISPPC=`file "${SRCFILE}.${EXT}" | grep "ppc"`
    ISARM=`file "${SRCFILE}.${EXT}" | grep "aarch64"`

    if [ "${IS32}" != "" ]; then
        if [ ! -L "${DSTFILE}x86.${EXT}" ]; then
            ln -s "${SRCFILE}.${EXT}" "${DSTFILE}x86.${EXT}"
        fi
    elif [ -L "${DSTFILE}x86.${EXT}" ]; then
        rm "${DSTFILE}x86.${EXT}"
    fi

    if [ "${IS64}" != "" ]; then
        if [ ! -L "${DSTFILE}x86_64.${EXT}" ]; then
            ln -s "${SRCFILE}.${EXT}" "${DSTFILE}x86_64.${EXT}"
        fi
    elif [ -L "${DSTFILE}x86_64.${EXT}" ]; then
        rm "${DSTFILE}x86_64.${EXT}"
    fi

    if [ "${ISPPC}" != "" ]; then
        if [ ! -L "${DSTFILE}ppc.${EXT}" ]; then
            ln -s "${SRCFILE}.${EXT}" "${DSTFILE}ppc.${EXT}"
        fi
    elif [ -L "${DSTFILE}ppc.${EXT}" ]; then
        rm "${DSTFILE}ppc.${EXT}"
    fi

    if [ "${ISARM}" != "" ]; then
        if [ ! -L "${DSTFILE}aarch64.${EXT}" ]; then
            ln -s "${SRCFILE}.${EXT}" "${DSTFILE}aarch64.${EXT}"
        fi
    elif [ -L "${DSTFILE}aarch64.${EXT}" ]; then
        rm "${DSTFILE}aarch64.${EXT}"
    fi

    popd > /dev/null
}

SEARCH_ARCHS="																	\
	x86																			\
	x86_64																		\
	ppc																			\
	aarch64																		\
"

HAS_LIPO=`command -v lipo`
HAS_CP=`command -v cp`

# if lipo is not available, we cannot make a universal binary, print a warning
if [ ! -x "${HAS_LIPO}" ] && [ "${CURRENT_ARCH}" == "" ]; then
	CURRENT_ARCH=`uname -m`
	if [ "${CURRENT_ARCH}" == "i386" ]; then CURRENT_ARCH="x86"; fi
	echo "$0 cannot make a universal binary, falling back to architecture ${CURRENT_ARCH}"
fi

# if the optional arch parameter is used, replace SEARCH_ARCHS to only work with one
if [ "${CURRENT_ARCH}" != "" ]; then
	SEARCH_ARCHS="${CURRENT_ARCH}"
fi

AVAILABLE_ARCHS=""

TRINITY_VERSION="1.32e"
TRINITY_CLIENT_ARCHS=""
TRINITY_SERVER_ARCHS=""


DEDICATED_NAME="trinity.ded"

ICNSDIR="code/unix"
ICNS="quake3_flat.icns"
PKGINFO="APPLTRNT"

OBJROOT="build"
#BUILT_PRODUCTS_DIR="${OBJROOT}/${TARGET_NAME}-darwin-${CURRENT_ARCH}"
PRODUCT_NAME="trinity"
BUNDLE_NAME="Trinity"
WRAPPER_EXTENSION="app"
WRAPPER_NAME="${BUNDLE_NAME}.${WRAPPER_EXTENSION}"
CONTENTS_FOLDER_PATH="${WRAPPER_NAME}/Contents"
UNLOCALIZED_RESOURCES_FOLDER_PATH="${CONTENTS_FOLDER_PATH}/Resources"
EXECUTABLE_FOLDER_PATH="${CONTENTS_FOLDER_PATH}/MacOS"
EXECUTABLE_NAME="${PRODUCT_NAME}"

# loop through the architectures to build string lists for each universal binary
for ARCH in $SEARCH_ARCHS; do
	CURRENT_ARCH=${ARCH}
	BUILT_PRODUCTS_DIR="${OBJROOT}/${TARGET_NAME}-darwin-${CURRENT_ARCH}"
	TRINITY_CLIENT="${EXECUTABLE_NAME}.${CURRENT_ARCH}"
	TRINITY_SERVER="${DEDICATED_NAME}.${CURRENT_ARCH}"

	if [ ! -d ${BUILT_PRODUCTS_DIR} ]; then
		CURRENT_ARCH=""
		BUILT_PRODUCTS_DIR=""
		continue
	fi

	# executables
	if [ -e ${BUILT_PRODUCTS_DIR}/${TRINITY_CLIENT} ]; then
		TRINITY_CLIENT_ARCHS="${BUILT_PRODUCTS_DIR}/${TRINITY_CLIENT} ${TRINITY_CLIENT_ARCHS}"
		VALID_ARCHS="${ARCH} ${VALID_ARCHS}"
	else
		continue
	fi
	if [ -e ${BUILT_PRODUCTS_DIR}/${TRINITY_SERVER} ]; then
		TRINITY_SERVER_ARCHS="${BUILT_PRODUCTS_DIR}/${TRINITY_SERVER} ${TRINITY_SERVER_ARCHS}"
	fi

	#echo "valid arch: ${ARCH}"
done

# final preparations and checks before attempting to make the application bundle
cd `dirname $0`

if [ ! -f Makefile ]; then
	echo "$0 must be run from the trinity build directory"
	exit 1
fi

if [ "${TRINITY_CLIENT_ARCHS}" == "" ]; then
	echo "$0: no trinity binary architectures were found for target '${TARGET_NAME}'"
	exit 1
fi

# set the final application bundle output directory
if [ "${2}" == "" ]; then
	BUILT_PRODUCTS_DIR="${OBJROOT}/${TARGET_NAME}-darwin-universal2"
	if [ ! -d ${BUILT_PRODUCTS_DIR} ]; then
		mkdir -p ${BUILT_PRODUCTS_DIR} || exit 1;
	fi
else
	BUILT_PRODUCTS_DIR="${OBJROOT}/${TARGET_NAME}-darwin-${CURRENT_ARCH}"
fi

BUNDLEBINDIR="${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}"


# here we go
echo "Creating bundle '${BUILT_PRODUCTS_DIR}/${WRAPPER_NAME}'"
echo "with architectures:"
for ARCH in ${VALID_ARCHS}; do
	echo " ${ARCH}"
done
echo ""

# make the application bundle directories
if [ ! -d "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}" ]; then
	mkdir -p "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}" || exit 1;
fi
if [ ! -d "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}" ]; then
	mkdir -p "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}" || exit 1;
fi

# copy and generate some application bundle resources
cp code/libsdl/macosx/*.dylib "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}"
cp code/libopenal/macosx/*.dylib "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}"
# Fetch and bundle MoltenVK so the Vulkan renderer loads on stock macOS
# without users needing the Vulkan SDK. fetch-moltenvk.sh is idempotent;
# users can also run it manually and commit the resulting dylib.
if [ ! -f code/libvulkan/macosx/libvulkan.1.dylib ]; then
    ./misc/macos/fetch-moltenvk.sh || exit 1
fi
cp code/libvulkan/macosx/*.dylib "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}"
cp ${ICNSDIR}/${ICNS} "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/$ICNS" || exit 1;
echo -n ${PKGINFO} > "${BUILT_PRODUCTS_DIR}/${CONTENTS_FOLDER_PATH}/PkgInfo" || exit 1;

# create Info.Plist
PLIST="<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>${EXECUTABLE_NAME}</string>
    <key>CFBundleIconFile</key>
    <string>quake3_flat</string>
    <key>CFBundleIdentifier</key>
    <string>org.trinity.${PRODUCT_NAME}</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>${BUNDLE_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>${TRINITY_VERSION}</string>
    <key>CFBundleSignature</key>
    <string>????</string>
    <key>CFBundleVersion</key>
    <string>${TRINITY_VERSION}</string>
    <key>CGDisableCoalescedUpdates</key>
    <true/>
    <key>LSMinimumSystemVersion</key>
    <string>${MACOSX_DEPLOYMENT_TARGET}</string>"

PLIST="${PLIST}
    <key>NSHumanReadableCopyright</key>
    <string>QUAKE III ARENA Copyright © 1999-2000 id Software, Inc. All rights reserved.</string>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
    <key>NSHighResolutionCapable</key>
    <false/>
    <key>NSRequiresAquaSystemAppearance</key>
    <false/>
</dict>
</plist>
"
echo -e "${PLIST}" > "${BUILT_PRODUCTS_DIR}/${CONTENTS_FOLDER_PATH}/Info.plist"

# action takes care of generating universal binaries if lipo is available
# otherwise, it falls back to using a simple copy, expecting the first item in
# the second parameter list to be the desired architecture
function action()
{
	COMMAND=""

	if [ -x "${HAS_LIPO}" ]; then
		COMMAND="${HAS_LIPO} -create -o"
		$HAS_LIPO -create -o "${1}" ${2} # make sure $2 is treated as a list of files
	elif [ -x ${HAS_CP} ]; then
		COMMAND="${HAS_CP}"
		SRC="${2// */}" # in case there is a list here, use only the first item
		$HAS_CP "${SRC}" "${1}"
	else
		"$0 cannot create an application bundle."
		exit 1
	fi

	#echo "${COMMAND}" "${1}" "${2}"
}

#
# the meat of universal binary creation
# destination file names do not have architecture suffix.
# action will handle merging universal binaries if supported.
# symlink appropriate architecture names for universal (fat) binary support.
#

# executables
action "${BUNDLEBINDIR}/${EXECUTABLE_NAME}"				"${TRINITY_CLIENT_ARCHS}"
action "${BUNDLEBINDIR}/${DEDICATED_NAME}"				"${TRINITY_SERVER_ARCHS}"

# Renderer DLLs are per-arch by filename — cl_main.c interpolates
# REND_ARCH_STRING into the dlopen path, so trinity_vulkan_aarch64.dylib
# and trinity_vulkan_x86_64.dylib must stay distinct (never lipo'd). Copy
# each per-arch DLL as-is and rewrite install_name to @rpath/<basename>
# so dyld resolves them via the trinity binary's LC_RPATH=@executable_path.
for ARCH in ${VALID_ARCHS}; do
    SRC_DIR="${OBJROOT}/${TARGET_NAME}-darwin-${ARCH}"
    for dll in "${SRC_DIR}"/${PRODUCT_NAME}_*_${ARCH}.dylib; do
        [ -f "${dll}" ] || continue
        cp "${dll}" "${BUNDLEBINDIR}/"
        install_name_tool -id "@rpath/$(basename "${dll}")" \
            "${BUNDLEBINDIR}/$(basename "${dll}")"
    done
done

# Optionally bundle Trinity mod paks inside the .app. Callers (CI, local
# release scripts) point TRINITY_ASSETS_DIR at a directory containing the
# pre-downloaded pak files; we copy them into Contents/Resources/, the
# macOS-conventional location for non-code bundle data. The engine's
# fs_apppath search (Sys_DefaultAppPath) resolves to Resources/ on macOS,
# so these are picked up automatically. Users' overrides in fs_homepath
# always win, so this is a safe fallback.
#
# Paks must NOT live under Contents/MacOS/ — Sequoia+ codesign treats
# files there as nested code subcomponents and refuses to sign the
# bundle if any are unsigned (and .pk3 is a zip, not a Mach-O, so it
# can't be signed as code). Resources/ files are hashed as resources.
if [ -n "${TRINITY_ASSETS_DIR}" ]; then
	if [ ! -d "${TRINITY_ASSETS_DIR}" ]; then
		echo "**** ERROR: TRINITY_ASSETS_DIR set to '${TRINITY_ASSETS_DIR}' but directory does not exist"
		exit 1
	fi
	mkdir -p "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/baseq3"
	mkdir -p "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/missionpack"
	for pak in pak8t.pk3 zzz-trinity-announcer.pk3; do
		if [ -f "${TRINITY_ASSETS_DIR}/${pak}" ]; then
			cp "${TRINITY_ASSETS_DIR}/${pak}" "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/baseq3/${pak}"
			echo "Bundled Resources/baseq3/${pak}"
		else
			echo "**** WARNING: ${pak} not found in TRINITY_ASSETS_DIR"
		fi
	done
	if [ -f "${TRINITY_ASSETS_DIR}/pak3t.pk3" ]; then
		cp "${TRINITY_ASSETS_DIR}/pak3t.pk3" "${BUILT_PRODUCTS_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}/missionpack/pak3t.pk3"
		echo "Bundled Resources/missionpack/pak3t.pk3"
	else
		echo "**** WARNING: pak3t.pk3 not found in TRINITY_ASSETS_DIR"
	fi
fi

# ad-hoc sign if codesign is available (required on Apple Silicon for the
# app to launch at all). Hardened Runtime is intentionally NOT used here:
# ad-hoc signatures cannot carry the restricted entitlements
# (cs.allow-unsigned-executable-memory, cs.disable-library-validation) that
# the runtime would need to honor, so enabling it here would break launch.
# The real notarization path in make-macosx-ub2.sh uses a Developer ID and
# the entitlements file, which is where Hardened Runtime belongs.
HAS_CODESIGN=`command -v codesign`
if [ -x "${HAS_CODESIGN}" ]; then
	BUNDLE="${BUILT_PRODUCTS_DIR}/${WRAPPER_NAME}"
	# sign nested content first (inside-out): dylibs and any executable files
	find "${BUNDLE}/Contents/MacOS" -type f \( -name '*.dylib' -o -perm +111 \) \
		-exec ${HAS_CODESIGN} --force --sign - {} \;
	# then the bundle itself
	${HAS_CODESIGN} --force --sign - "${BUNDLE}"
fi
