#!/bin/bash
# The V3D compiler's statistics for every Vulkan engine pipeline, without a
# Pi: the engine is created against Mesa's V3D drm-shim (emulating the Pi 5's
# V3D 7.1), which compiles every pipeline, and V3D_DEBUG=shaderdb reports each.
#   scripts/shader-stats.sh [mesa|mesa-main] [OUT.txt]
# Needs scripts/build-mesa-shim.sh NAME and the engine/bench build. Extra
# environment (DVVK_*) passes through, e.g. to compare engine variants.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:-mesa}
OUT=${2:-$ROOT/build/shader-stats-$NAME.txt}
LOG=${OUT%.txt}.log
pass=$(env | grep -oE '^DVVK_[A-Z_0-9]+=' | sed 's/=$//' | sed 's/^/-e /' | tr '\n' ' ' || true)
DOCKER_ENV="$pass" "$ROOT/scripts/in-docker.sh" bash -c "
  M=\${WORK:-/work}/build/$NAME-v3d
  export LD_PRELOAD=\$M/lib/libv3d_noop_drm_shim.so VK_ICD_FILENAMES=\$M/share/vulkan/icd.d/broadcom_icd.aarch64.json
  export V3D_DEBUG=shaderdb,qpu,cs DVVK_PIPELINE_PROFILE=1 MESA_SHADER_CACHE_DISABLE=true XDG_CACHE_HOME=\$(mktemp -d)
  \$PREFIX/bin/vk-compile" >"$LOG" 2>&1 || { tail -5 "$LOG"; exit 1; }
python3 "$ROOT/bundle/shader-stats.py" "$LOG" >"$OUT"
gzip -f "$LOG"   # the QPU dump is large
echo "$OUT ($(zgrep -c "dvvk pipeline" "$LOG.gz") pipelines, $NAME)"
