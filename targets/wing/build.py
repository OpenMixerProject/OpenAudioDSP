#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build WING with CCES 2.12.1, without invoking or modifying a console."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import uuid
from xml.etree import ElementTree


WING = Path(__file__).resolve().parent
ROOT = WING.parent.parent
BOARD_PROFILE = WING / "dsp_board_profile.json"
TARGETS = (1, 2, 3, 4)
COMMON = ["targets/wing/audio.cpp", "targets/wing/runtime.c"]
INCLUDES = ("targets/wing",)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    tool = parser.add_mutually_exclusive_group()
    tool.add_argument("--gateway", default=os.environ.get("CCES", str(Path.home() / ".local/bin/cces")))
    tool.add_argument("--cces-bin", type=Path, help="directory containing native CCES executables")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dxe-only", action="store_true", help="compile/link the four ADSP-21489 DXEs only")
    mode.add_argument("--sim", action="store_true", help="run audio-only vectors on the gateway's ADSP-21371 simulator")
    parser.add_argument("--out", type=Path, help="new output directory (default: build or build-dxe)")
    args = parser.parse_args()
    remote = args.cces_bin is None
    if args.sim and not remote:
        parser.error("--sim requires the CCES gateway")
    if args.sim and args.out:
        parser.error("--sim streams results; it does not create an output directory")
    out = (args.out or WING / ("build-dxe" if args.dxe_only else "build")).resolve()
    if not args.sim and out.exists():
        parser.error(f"{out} already exists; move it away or choose --out")
    proc = "ADSP-21371" if args.sim else "ADSP-21489"
    target_flags = ("-proc", proc, "-si-revision", "any")
    loader_flags = (*target_flags, "-bSPIHOST", "-fBINARY", "-HostWidth", "8")
    entry = ["targets/wing/audio_test.cpp"] if args.sim else ["targets/wing/sport.c", "targets/wing/runtime-spi.c"]
    elfloader = not args.sim and not args.dxe_only
    headers = [WING / name for name in ("profile.h", "audio.h", "runtime.h", "version.h")]
    paths = [ROOT / f for f in [*COMMON, *entry]] + headers + [Path(__file__).resolve(), BOARD_PROFILE]
    inputs = {p.relative_to(ROOT).as_posix(): p.read_bytes() for p in paths}
    profile_bytes = inputs[BOARD_PROFILE.relative_to(ROOT).as_posix()]
    profile = json.loads(profile_bytes)["profile"]
    profile_hash = hashlib.sha256(profile_bytes).hexdigest()
    stem = f"wing_{uuid.uuid4().hex[:12]}"
    commands, cleanup = [], []
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".wing-build-", dir=out.parent) as tmp:
        cwd = Path(tmp)

        def run(verb, *argv, **kwargs):
            line = [verb, *argv]
            if verb in ("cc21k", "elfloader", "elfloader-21489"):
                commands.append(shlex.join(line).replace(stem + "/", ""))
            if remote:
                command = [args.gateway, *line]
            else:
                executable = (args.cces_bin / (verb + (".exe" if os.name == "nt" else ""))).resolve()
                command = [str(executable), *argv]
            print(shlex.join(line), flush=True)
            return subprocess.run(command, cwd=cwd, check=True, **kwargs)

        src = f"{stem}/" if remote else ""
        try:
            for rel, data in inputs.items():
                if rel.endswith(".json"):
                    continue  # Attested packaging input, not a compiler input.
                if remote:
                    cleanup.append(src + rel)
                    run("put", src + rel, input=data)
                else:
                    p = cwd / rel
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_bytes(data)
            stage = cwd / "output"
            stage.mkdir()
            for dsp in TARGETS:
                sources = [*entry, *COMMON]
                objects = []
                for source in sources:
                    obj = f"{src}{dsp}_{Path(source).stem}.doj"
                    cleanup.append(obj)
                    objects.append(obj)
                    run("cc21k", *target_flags, "-O", "-double-size-32", "-double-size-any",
                        "-DNDEBUG", f"-DWING_DSP={dsp}",
                        *(arg for directory in INCLUDES for arg in ("-I", src + directory)),
                        "-c", src + source, "-o", obj)
                dxe = f"{src}dsp{dsp}.dxe"
                cleanup.append(dxe)
                link_args = []
                if not args.sim:
                    mapfile = f"{src}dsp{dsp}.map.xml"
                    cleanup.append(mapfile)
                    link_args = ["-map", mapfile]
                run("cc21k", *target_flags, "-double-size-32", "-double-size-any", *link_args, *objects, "-o", dxe)
                ldr = f"{src}dsp{dsp}.ldr"
                if args.sim:
                    sim = run("sim", dxe, "wing_test_done", "wing_test_result", "4", "120000",
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout
                    print(sim)
                    expected = {1: 1797, 2: 1029, 3: 517, 4: 517}[dsp]
                    result_file = re.search(r"=== ([\w./-]+\.simres\.txt) ===", sim)
                    if result_file and result_file[1].startswith(src):
                        cleanup.append(result_file[1])
                    match = re.search(r"=== [^\n]*\.simres\.txt ===\n(.*?)=== end ===", sim, re.S)
                    if (not match or "HALTED (Reason: Breakpoint)" not in sim or "Error:" in sim or
                            [int(n) for n in match[1].split()] != [expected, 0, dsp, 48000]):
                        raise SystemExit(f"DSP{dsp} audio simulator did not produce the expected results")
                    continue
                if elfloader:
                    cleanup.append(ldr)
                    if remote:
                        run("elfloader-21489", dxe, ldr)
                    else:
                        run("elfloader", *loader_flags, dxe, "-o", ldr)
                for name in (dxe, mapfile, ldr) if elfloader else (dxe, mapfile):
                    dest = stage / Path(name).name
                    if remote:
                        with dest.open("wb") as f:
                            run("get", name, stdout=f)
                    else:
                        (cwd / name).replace(dest)
                    if not dest.stat().st_size:
                        raise SystemExit(f"empty CCES output: {name}")
                project = ElementTree.parse(stage / Path(mapfile).name).find("LINKPROJECT")
                if project is None or project.get("architecture") != proc:
                    raise SystemExit(f"wrong processor in linker map for DSP{dsp}")
                for memory in project.findall("MEMORY"):
                    if (memory.attrib["name"].startswith(("mem_sdram", "mem_flash", "mem_sram")) and
                            int(memory.attrib["words_used"], 0)):
                        raise SystemExit(f"DSP{dsp} uses unconfigured external memory: {memory.attrib['name']}")
            if not args.sim:
                commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
                source_hashes = [f"{hashlib.sha256(data).hexdigest()}  {name}" for name, data in sorted(inputs.items())]
                source_hash = hashlib.sha256("\n".join(source_hashes).encode()).hexdigest()
                loader_info = (f"target={proc}\nboot=spi-slave\nformat=raw-binary\n"
                               "loader_kernel=489_spi.dxe\n"
                               f"loader_flags={shlex.join(loader_flags)}\n") if elfloader else ""
                (stage / "provenance.txt").write_text(
                    f"profile={profile}\ndsp_runtime_abi=wdsp-v1\ndsp_board_profile_sha256={profile_hash}\n"
                    f"source_commit={commit}\nsource_sha256={source_hash}\n"
                    f"toolchain=CCES 2.12.1 ({'gateway' if remote else 'native'})\nprocessor={proc}\n"
                    + loader_info + "\n"
                    + "\n".join(commands) + "\n\n" + "\n".join(source_hashes) + "\n", encoding="utf-8")
                if elfloader:
                    (stage / BOARD_PROFILE.name).write_bytes(profile_bytes)
                    (stage / "SHA256SUMS").write_text("".join(
                        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n"
                        for p in sorted(stage.glob("*.ldr"))), encoding="ascii")
                stage.rename(out)
        finally:
            if remote:
                for name in reversed(cleanup):
                    subprocess.run([args.gateway, "rm", name], cwd=cwd, check=False,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
