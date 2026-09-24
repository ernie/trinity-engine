#!/bin/sh
# Regenerates spirv/shader_data.c from the GLSL sources in this directory.
# Invoked by the Makefile when glslangValidator is available.
#
# Usage: compile.sh <glslangValidator> <bin2hex> <shaders-dir>

set -e

GLSLANG="$1"
BIN2HEX="$2"
DIR="$3"

if [ -z "$GLSLANG" ] || [ -z "$BIN2HEX" ] || [ -z "$DIR" ]; then
	echo "usage: $0 <glslangValidator> <bin2hex> <shaders-dir>" >&2
	exit 1
fi

SPV="$DIR/spirv/data.spv"
OUT="$DIR/spirv/shader_data.c"

mkdir -p "$DIR/spirv"
rm -f "$OUT" "$SPV"

# c <stage> <array-name> <source-file> <variants> [defines...]
# Variants is a "+"-separated list, or "-" for none. Each one appends a
# second module named "<array-name>_<variant>" built with its own define.
# Mono modules stay free of the MultiView capability, so only transform
# vertices, scene sampling fragments, and flare probes carry "mv".
c() {
	stage="$1"
	name="$2"
	src="$3"
	rest="$4"
	shift 4
	"$GLSLANG" -S "$stage" -V -o "$SPV" "$DIR/$src" "$@"
	"$BIN2HEX" "$SPV" "+$OUT" "$name"
	while [ -n "$rest" ] && [ "$rest" != "-" ]; do
		variant="${rest%%+*}"
		case "$rest" in
			*+*) rest="${rest#*+}" ;;
			*) rest="" ;;
		esac
		case "$variant" in
			mv) define="-DMULTIVIEW" ;;
			array) define="-DARRAY_SOURCE" ;;
			*)
				echo "$0: unknown shader variant '$variant'" >&2
				exit 1
				;;
		esac
		"$GLSLANG" -S "$stage" -V "$define" -o "$SPV" "$DIR/$src" "$@"
		"$BIN2HEX" "$SPV" "+$OUT" "${name}_${variant}"
	done
	rm -f "$SPV"
}

# The Windows build consumes the same permutation inventory.
while read -r stage name source variants defines; do
	[ -n "$stage" ] || continue
	c "$stage" "$name" "$source" "$variants" $defines
done < "$DIR/shaders.list"

echo "shader_data.c regenerated"
