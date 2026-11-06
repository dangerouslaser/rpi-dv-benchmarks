#!/usr/bin/env python3
"""V3D compiler statistics per pipeline, from a dvbench log run with
V3D_DEBUG=shaderdb DVVK_PIPELINE_PROFILE=1 (and shader caches disabled, so
every shader is compiled): Mesa prints one SHADER-DB line per compiled
shader, then the Vulkan engine names the pipeline it just made.

    shader-stats.py LOG

Columns: QPU instructions, threads per QPU (4 best, 1 worst: fewer threads
means less latency hiding for every memory access), register spills:fills
(each one a memory round trip), max temporaries. With V3D_DEBUG=qpu,cs also
in the log: TMU lookups (memory loads/stores, texture fetches: the costly
part on V3D) and thread switches, counted statically in the QPU code. GL
shaders (CB1, through libplacebo) have no pipeline line and are listed
separately.
"""
import re
import sys

TMU_ADDR = re.compile(r"\b(tmua|tmuau|tmut|tmus|tmusf|tmuscm|tmusl|tmuslod|tmuhs|tmuhscm|tmuhsf|tmuhslod)\b")
STAT = re.compile(r"SHADER-DB-?\S* - (\w+) shader: (\d+) inst, (\d+) threads, (\d+) loops, (\d+) uniforms, "
                  r"(\d+) max-temps, (\d+):(\d+) spills:fills")
PIPE = re.compile(r"dvvk pipeline: shader=(\S+) .*spec=(\[[^\]]*\])")


def main(path):
    pending, vk, gl = [], [], []
    tmu = thrsw = 0
    have_qpu = False
    for line in open(path, errors="replace"):
        if " QPU:" in line:
            tmu = thrsw = 0
            have_qpu = True
            continue
        if "SHADER-DB" not in line:
            tmu += len(TMU_ADDR.findall(line))
            thrsw += line.count("thrsw")
        m = STAT.search(line)
        if m:
            stage, inst, threads, loops, uniforms, temps, spills, fills = m.groups()
            pending.append((stage, int(inst), int(threads), int(temps), int(spills), int(fills), tmu, thrsw))
            tmu = thrsw = 0
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
        print(f"{'shader':28} {'spec':22} {'inst':>6} {'threads':>7} {'temps':>5} {'spills:fills':>12}"
              + (f" {'tmu':>5} {'thrsw':>5}" if have_qpu else "") + "  flag")
        for r in sorted(rows, key=lambda r: -r[2] if named else -r[1]):
            if named:
                name, spec, inst, threads, temps, spills, fills, tmu_n, thrsw_n = r
            else:
                stage, inst, threads, temps, spills, fills, tmu_n, thrsw_n = r
                name, spec = stage, ""
            flag = []
            if threads < 4:
                flag.append(f"{threads} thread{'s' if threads > 1 else ''}")
            if spills or fills:
                flag.append("SPILLS")
            extra = f" {tmu_n:>5} {thrsw_n:>5}" if have_qpu else ""
            print(f"{name[:28]:28} {spec[:22]:22} {inst:>6} {threads:>7} {temps:>5} {f'{spills}:{fills}':>12}{extra}  {', '.join(flag)}")
        print()

    table("Vulkan engine pipelines", vk, True)
    table(f"GL shaders (CB1 / libplacebo / the bench's SAND pass), {len(gl)} compiled", gl, False)
    if not vk and not gl:
        print("no SHADER-DB lines: run with V3D_DEBUG=shaderdb and shader caches disabled")


if __name__ == "__main__":
    main(sys.argv[1])
