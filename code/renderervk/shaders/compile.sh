#!/bin/sh
# Regenerates spirv/shader_data.c from the GLSL sources in this directory.
# Invoked by the Makefile when glslangValidator is available. Mirrors
# compile.bat: glslang defaults (no --target-env) and the same shader and
# permutation set / array names.
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

# c <stage> <array-name> <source-file> [defines...]
c() {
	stage="$1"
	name="$2"
	src="$3"
	shift 3
	"$GLSLANG" -S "$stage" -V -o "$SPV" "$DIR/$src" "$@"
	"$BIN2HEX" "$SPV" "+$OUT" "$name"
	rm -f "$SPV"
}

# --- individual shaders -----------------------------------------------------
c vert color_vert_spv  color.vert
c vert dot_vert_spv    dot.vert
c vert fog_vert_spv    fog.vert
c vert gamma_vert_spv  gamma.vert

c frag blend_frag_spv  blend.frag
c frag bloom_frag_spv  bloom.frag
c frag blur_frag_spv   blur.frag
c frag color_frag_spv  color.frag
c frag dot_frag_spv    dot.frag
c frag fog_frag_spv    fog.frag
c frag gamma_frag_spv  gamma.frag

# --- lighting templates -----------------------------------------------------
c vert vert_light          light_vert.tmpl
c vert vert_light_fog      light_vert.tmpl -DUSE_FOG
c frag frag_light          light_frag.tmpl
c frag frag_light_fog      light_frag.tmpl -DUSE_FOG
c frag frag_light_line     light_frag.tmpl -DUSE_LINE
c frag frag_light_line_fog light_frag.tmpl -DUSE_LINE -DUSE_FOG

# --- generic vertex: single texture ----------------------------------------
c vert vert_tx0                gen_vert.tmpl
c vert vert_tx0_overbright     gen_vert.tmpl -DUSE_OVERBRIGHT
c vert vert_tx0_fog            gen_vert.tmpl -DUSE_FOG
c vert vert_tx0_env            gen_vert.tmpl -DUSE_ENV
c vert vert_tx0_env_fog        gen_vert.tmpl -DUSE_FOG -DUSE_ENV
c vert vert_tx0_ident1         gen_vert.tmpl -DUSE_CLX_IDENT
c vert vert_tx0_ident1_fog     gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_FOG
c vert vert_tx0_ident1_env     gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_ENV
c vert vert_tx0_ident1_env_fog gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_FOG -DUSE_ENV
c vert vert_tx0_fixed          gen_vert.tmpl -DUSE_FIXED_COLOR
c vert vert_tx0_fixed_fog      gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_FOG
c vert vert_tx0_fixed_env      gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_ENV
c vert vert_tx0_fixed_env_fog  gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_FOG -DUSE_ENV

# --- generic vertex: double texture ----------------------------------------
c vert vert_tx1                gen_vert.tmpl -DUSE_TX1
c vert vert_tx1_fog            gen_vert.tmpl -DUSE_TX1 -DUSE_FOG
c vert vert_tx1_env            gen_vert.tmpl -DUSE_TX1 -DUSE_ENV
c vert vert_tx1_env_fog        gen_vert.tmpl -DUSE_TX1 -DUSE_FOG -DUSE_ENV
c vert vert_tx1_ident1         gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_TX1
c vert vert_tx1_ident1_fog     gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_FOG
c vert vert_tx1_ident1_env     gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_ENV
c vert vert_tx1_ident1_env_fog gen_vert.tmpl -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_FOG -DUSE_ENV
c vert vert_tx1_fixed          gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_TX1
c vert vert_tx1_fixed_fog      gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_FOG
c vert vert_tx1_fixed_env      gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_ENV
c vert vert_tx1_fixed_env_fog  gen_vert.tmpl -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_FOG -DUSE_ENV
c vert vert_tx1_cl             gen_vert.tmpl -DUSE_CL1 -DUSE_TX1
c vert vert_tx1_cl_fog         gen_vert.tmpl -DUSE_CL1 -DUSE_TX1 -DUSE_FOG
c vert vert_tx1_cl_env         gen_vert.tmpl -DUSE_CL1 -DUSE_TX1 -DUSE_ENV
c vert vert_tx1_cl_env_fog     gen_vert.tmpl -DUSE_CL1 -DUSE_TX1 -DUSE_ENV -DUSE_FOG

# --- generic vertex: triple texture ----------------------------------------
c vert vert_tx2                gen_vert.tmpl -DUSE_TX2
c vert vert_tx2_fog            gen_vert.tmpl -DUSE_TX2 -DUSE_FOG
c vert vert_tx2_env            gen_vert.tmpl -DUSE_TX2 -DUSE_ENV
c vert vert_tx2_env_fog        gen_vert.tmpl -DUSE_TX2 -DUSE_ENV -DUSE_FOG
c vert vert_tx2_cl             gen_vert.tmpl -DUSE_CL2 -DUSE_TX2
c vert vert_tx2_cl_fog         gen_vert.tmpl -DUSE_CL2 -DUSE_TX2 -DUSE_FOG
c vert vert_tx2_cl_env         gen_vert.tmpl -DUSE_CL2 -DUSE_TX2 -DUSE_ENV
c vert vert_tx2_cl_env_fog     gen_vert.tmpl -DUSE_CL2 -DUSE_TX2 -DUSE_ENV -DUSE_FOG

# --- generic fragment: single texture --------------------------------------
c frag frag_tx0                gen_frag.tmpl -DUSE_ATEST
c frag frag_tx0_overbright     gen_frag.tmpl -DUSE_ATEST -DUSE_OVERBRIGHT
c frag frag_tx0_fog            gen_frag.tmpl -DUSE_ATEST -DUSE_FOG
c frag frag_tx0_ident1         gen_frag.tmpl -DUSE_CLX_IDENT -DUSE_ATEST
c frag frag_tx0_ident1_fog     gen_frag.tmpl -DUSE_CLX_IDENT -DUSE_ATEST -DUSE_FOG
c frag frag_tx0_fixed          gen_frag.tmpl -DUSE_FIXED_COLOR -DUSE_ATEST
c frag frag_tx0_fixed_fog      gen_frag.tmpl -DUSE_FIXED_COLOR -DUSE_ATEST -DUSE_FOG
c frag frag_tx0_ent            gen_frag.tmpl -DUSE_ENT_COLOR -DUSE_ATEST
c frag frag_tx0_ent_fog        gen_frag.tmpl -DUSE_ENT_COLOR -DUSE_ATEST -DUSE_FOG
c frag frag_tx0_df             gen_frag.tmpl -DUSE_CLX_IDENT -DUSE_ATEST -DUSE_DF

# --- generic fragment: double texture --------------------------------------
c frag frag_tx1                gen_frag.tmpl -DUSE_TX1
c frag frag_tx1_fog            gen_frag.tmpl -DUSE_TX1 -DUSE_FOG
c frag frag_tx1_ident1         gen_frag.tmpl -DUSE_CLX_IDENT -DUSE_TX1
c frag frag_tx1_ident1_fog     gen_frag.tmpl -DUSE_CLX_IDENT -DUSE_TX1 -DUSE_FOG
c frag frag_tx1_fixed          gen_frag.tmpl -DUSE_FIXED_COLOR -DUSE_TX1
c frag frag_tx1_fixed_fog      gen_frag.tmpl -DUSE_FIXED_COLOR -DUSE_TX1 -DUSE_FOG
c frag frag_tx1_cl             gen_frag.tmpl -DUSE_CL1 -DUSE_TX1
c frag frag_tx1_cl_fog         gen_frag.tmpl -DUSE_CL1 -DUSE_TX1 -DUSE_FOG

# --- generic fragment: triple texture --------------------------------------
c frag frag_tx2                gen_frag.tmpl -DUSE_TX2
c frag frag_tx2_fog            gen_frag.tmpl -DUSE_TX2 -DUSE_FOG
c frag frag_tx2_cl             gen_frag.tmpl -DUSE_CL2 -DUSE_TX2
c frag frag_tx2_cl_fog         gen_frag.tmpl -DUSE_CL2 -DUSE_TX2 -DUSE_FOG

echo "shader_data.c regenerated"
