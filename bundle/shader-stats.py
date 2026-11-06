#!/usr/bin/env python3
"""V3D compiler statistics per pipeline, from a dvbench log run with
V3D_DEBUG=shaderdb DVVK_PIPELINE_PROFILE=1 (and shader caches disabled, so
every shader is compiled): Mesa prints one SHADER-DB line per compiled
shader, then the Vulkan engine names the pipeline it just made.

    shader-stats.py LOG

Columns: QPU instructions, threads per QPU (4 best, 1 worst: fewer threads
means less latency hiding for every memory access), register spills:fills
(each one a memory round trip), max temporaries. GL shaders (CB1, through
libplacebo) have no pipeline line and are listed separately.
"""
import re
import sys

STAT = re.compile(r"SHADER-DB-?\S* - (\w+) shader: (\d+) inst, (\d+) threads, (\d+) loops, (\d+) uniforms, "
                  r"(\d+) max-temps, (\d+):(\d+) spills:fills")
PIPE = re.compile(r"dvvk pipeline: shader=(\S+) .*spec=(\[[^\]]*\])")


def main(path):
    pending, vk, gl = [], [], []
    for line in open(path, errors="replace"):
        m = STAT.search(line)
        if m:
            stage, inst, threads, loops, uniforms, temps, spills, fills = m.groups()
            pending.append((stage, int(inst), int(threads), int(temps), int(spills), int(fills)))
            continue
        p = PIPE.search(line)
        if p and pending:
            # The compute shader just compiled for this pipeline.
            vk.append((p.group(1), p.group(2)) + pending[-1][1:])
            gl.extend(pending[:-1])
            pending = []
    gl.extend(pending)

    def table(title, rows, named):
        if not rows:
            return
        print(title)
        print(f"{'shader':28} {'spec':22} {'inst':>6} {'threads':>7} {'temps':>5} {'spills:fills':>12}  flag")
        for r in sorted(rows, key=lambda r: -r[-5] if named else -r[1]):
            if named:
                name, spec, inst, threads, temps, spills, fills = r
            else:
                stage, inst, threads, temps, spills, fills = r
                name, spec = stage, ""
            flag = []
            if threads < 4:
                flag.append(f"{threads} thread{'s' if threads > 1 else ''}")
            if spills or fills:
                flag.append("SPILLS")
            print(f"{name[:28]:28} {spec[:22]:22} {inst:>6} {threads:>7} {temps:>5} {f'{spills}:{fills}':>12}  {', '.join(flag)}")
        print()

    table("Vulkan engine pipelines", vk, True)
    table(f"GL shaders (CB1 / libplacebo / the bench's SAND pass), {len(gl)} compiled", gl, False)
    if not vk and not gl:
        print("no SHADER-DB lines: run with V3D_DEBUG=shaderdb and shader caches disabled")


if __name__ == "__main__":
    main(sys.argv[1])
