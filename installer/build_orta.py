"""Build Panzer Dragoon Orta for PC from the player's own game files.

Run by Setup.exe (with the Python and compiler it downloaded), or by hand:

    python build_orta.py --source <iso file or folder> --out <install folder>
                         --cc <llvm-mingw bin folder>

Steps: copy or unpack the game files, check default.xbe is the supported
version, translate the game's code to C (the toolkit's pipeline), compile
everything with clang, and lay out a ready-to-play folder.

Lines starting "@@" are for Setup.exe:
    @@PROGRESS <0-100> <text>   moves the progress bar
    @@ERROR <text>              a failure to show the player
    @@DONE <path to exe>        finished
"""

import argparse
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # the release's source root
TOOLKIT = os.path.join(ROOT, "xboxrecomp-main")
ORTA = os.path.join(ROOT, "orta")

# The game's default.xbe this build was made for (North American release).
SUPPORTED_XBE = {
    "b9e64bc1554efca9fcbbda4aa4a0c11cf5684241": "Panzer Dragoon Orta (USA)",
}

SECTIONS = "MMATRIX,D3D,D3DX,DSOUND,WMADEC,PSGSFD00,PSGSFD_I,PSGSFD_B,PSGSFD_P,XGRPH,XPP"

RUNTIME_SOURCES = {
    "kernel": "kernel_path kernel_pool kernel_rtl kernel_memory kernel_file kernel_thread "
              "kernel_sync kernel_hal kernel_xbox kernel_ob kernel_io kernel_crypto "
              "kernel_thunks kernel_bridge xbox_memory_layout guest_vmem icall_feedback "
              "recomp_trace nv2a_pb_scan nv2a_pb_exec nv2a_vsh_interp nv2a_combiner "
              "nv2a_gpu recomp_prof",
    "d3d": "d3d8_device d3d8_gamma d3d8_resources d3d8_shaders d3d8_states d3d8_combiners d3d8_vsh",
    "audio": "dsound_device wma_decoder",
    "apu": "apu_core apu_vp apu_dsp apu_mmio_hook apu_xaudio2",
    "nv2a": "nv2a_core nv2a_pgraph_d3d11 nv2a_pb_replay nv2a_mmio_hook nv2a_pb_test",
    "usb": "ohci usb_gamepad",
    "input": "xinput_device",
    "video": "video_player video_pump fb_present",
}

LIBS = ("d3d11 dxgi dxguid d3dcompiler_47 xinput1_4 winmm xaudio2_8 ole32 oleaut32 "
        "mfplat mfreadwrite mfuuid mf bcrypt dbghelp user32 gdi32 shell32 "
        "advapi32 uuid comctl32 kernel32").split()

# MSVC built this code until now. These keep clang from treating what MSVC
# merely warned about as errors, and from optimising on assumptions the
# translated x86 code does not keep (wrapping arithmetic, type-punned memory).
C_FLAGS = ["-std=gnu11", "-fms-extensions", "-O2", "-w",
           "-fwrapv", "-fno-strict-aliasing", "-fno-delete-null-pointer-checks",
           "-Wno-error=implicit-function-declaration", "-Wno-error=int-conversion",
           "-Wno-error=incompatible-pointer-types", "-Wno-error=incompatible-function-pointer-types",
           "-Wno-error=return-type",
           "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX", "-D_CRT_SECURE_NO_WARNINGS", "-D_USE_MATH_DEFINES"]
CXX_FLAGS = ["-std=c++17", "-O2", "-w", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX"]


def say(msg):
    print(msg, flush=True)


def progress(pct, text):
    say("@@PROGRESS %d %s" % (pct, text))


def fail(text):
    say("@@ERROR " + text)
    sys.exit(1)


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# ── 1. Game files ────────────────────────────────────────────────────────
def gather_game_files(source, dest):
    os.makedirs(dest, exist_ok=True)
    if os.path.isfile(source):
        sys.path.insert(0, TOOLKIT)
        from tools.xiso.xdvdfs import Xiso, XisoError
        try:
            with Xiso(source) as iso:
                entries = list(iso.walk())
                total = sum(e.size for _, e in entries) or 1
                done = 0
                for d, e in entries:
                    out = os.path.join(dest, *(d.split("/") if d else []), e.name)
                    iso.extract(e, out)
                    done += e.size
                    progress(2 + int(13 * done / total), "Unpacking the disc image")
        except XisoError as ex:
            fail("That file is not an Xbox disc image (%s)." % ex)
    else:
        files = []
        for base, dirs, names in os.walk(source):
            dirs[:] = [d for d in dirs if d.upper() not in ("UDATA", "TDATA")]
            for n in names:
                files.append(os.path.join(base, n))
        total = sum(os.path.getsize(f) for f in files) or 1
        done = 0
        for f in files:
            rel = os.path.relpath(f, source)
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copyfile(f, out)
            done += os.path.getsize(f)
            progress(2 + int(13 * done / total), "Copying the game files")
    xbe = os.path.join(dest, "default.xbe")
    if not os.path.isfile(xbe):
        fail("default.xbe was not found. Choose the disc image, or the folder that "
             "holds the game's files (the one with default.xbe in it).")
    return xbe


# ── 2. Translate ─────────────────────────────────────────────────────────
def run_tool(args, label, pct):
    progress(pct, label)
    env = dict(os.environ)
    env["XBOXRECOMP_ALIAS_SEEDS"] = os.path.join(TOOLKIT, "game_files", "alias_seeds.json")
    env["PYTHONPATH"] = TOOLKIT + os.pathsep + env.get("PYTHONPATH", "")
    p = subprocess.run([sys.executable] + args, cwd=TOOLKIT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace")
    tail = p.stdout.strip().splitlines()[-6:]
    for line in tail:
        say("  " + line)
    if p.returncode != 0:
        for line in p.stdout.strip().splitlines()[-30:]:
            say("  " + line)
        fail("%s failed. The log above has the details." % label)


def translate():
    gen = os.path.join(ORTA, "src", "recomp", "gen")
    manual = os.path.join(ORTA, "src", "recomp_manual.c")
    run_tool(["-m", "tools.xbe_parser", "game_files/default.xbe", "--json",
              "game_files/default_analysis.json", "--quiet"], "Reading the game's program file", 16)
    run_tool(["-m", "tools.disasm", "game_files/default.xbe", "--text-only",
              "--extra-sections", SECTIONS, "--seed-functions",
              "game_files/seeds_manual.json", "--force"],
             "Disassembling the game (1-2 minutes)", 18)
    run_tool(["-m", "tools.func_id", "game_files/default.xbe"], "Labelling functions", 26)
    run_tool(["-m", "tools.abi_analysis", "game_files/default.xbe"], "Checking calling conventions", 30)
    args = ["-m", "tools.recomp", "game_files/default.xbe", "--all", "--split", "250",
            "--gen-dir", gen, "--exclude-manual", manual]
    if os.path.isfile(os.path.join(TOOLKIT, "game_files", "trace_functions.json")):
        args += ["--trace-functions", "game_files/trace_functions.json"]
    run_tool(args, "Translating the game to C (3-4 minutes)", 34)
    return gen


# ── 3. Compile ───────────────────────────────────────────────────────────
def compile_all(cc_dir, gen, objdir, jobs):
    clang = os.path.join(cc_dir, "x86_64-w64-mingw32-clang.exe")
    clangxx = os.path.join(cc_dir, "x86_64-w64-mingw32-clang++.exe")
    if not os.path.isfile(clang):
        fail("The compiler was not found in %s." % cc_dir)
    src = os.path.join(TOOLKIT, "src")
    inc = ["-I" + src, "-I" + os.path.join(TOOLKIT, "include"),
           "-I" + os.path.join(TOOLKIT, "include", "xbox")]
    inc += ["-I" + os.path.join(src, d) for d in
            ("kernel", "apu", "nv2a", "d3d", "audio", "input", "platform", "usb", "video")]
    game_inc = ["-I" + os.path.join(ORTA, "src"), "-I" + os.path.join(ORTA, "src", "recomp"),
                "-I" + gen, "-I" + os.path.join(ORTA, "third_party", "imgui")]
    extra = os.path.join(ORTA, "src", "recomp", "recomp_extra.h")

    jobs_list = []                       # (compiler, flags, source, object)
    for d, names in RUNTIME_SOURCES.items():
        for n in names.split():
            jobs_list.append((clang, C_FLAGS + inc, os.path.join(src, d, n + ".c")))
    game_c = [os.path.join(ORTA, "src", n) for n in ("main.c", "recomp_manual.c", "launcher.c")]
    game_c += sorted(os.path.join(gen, f) for f in os.listdir(gen) if f.endswith(".c"))
    for f in game_c:
        jobs_list.append((clang, C_FLAGS + inc + game_inc + ["-include", extra], f))
    imgui = os.path.join(ORTA, "third_party", "imgui")
    for f in ["imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
              "imgui_impl_dx11.cpp"]:
        jobs_list.append((clangxx, CXX_FLAGS + game_inc, os.path.join(imgui, f)))
    jobs_list.append((clangxx, CXX_FLAGS + game_inc, os.path.join(ORTA, "src", "overlay.cpp")))

    os.makedirs(objdir, exist_ok=True)
    # Biggest first, so the long ones are not left for the end on one core.
    jobs_list.sort(key=lambda j: -os.path.getsize(j[2]))
    objs, errors = [], []
    total = len(jobs_list)

    def one(job):
        comp, flags, source = job
        rel = os.path.relpath(source, ROOT).replace(os.sep, "_").replace(".", "_")
        obj = os.path.join(objdir, rel + ".o")
        # A retry after a failed run reuses what already compiled.
        if os.path.isfile(obj) and os.path.getmtime(obj) > os.path.getmtime(source):
            return source, obj, 0, ""
        p = subprocess.run([comp] + flags + ["-c", source, "-o", obj],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           errors="replace")
        return source, obj, p.returncode, p.stdout

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for source, obj, rc, out in ex.map(one, jobs_list):
            done += 1
            if rc != 0:
                errors.append((source, out))
            objs.append((source, obj))
            progress(45 + int(45 * done / total),
                     "Compiling (%d of %d files)" % (done, total))
    if errors:
        for source, out in errors[:5]:
            say("  %s:" % os.path.relpath(source, ROOT))
            for line in out.strip().splitlines()[:15]:
                say("    " + line)
        fail("%d file(s) did not compile. The log above has the details." % len(errors))
    return clangxx, objs


def link(clangxx, objs, exe):
    progress(91, "Linking")
    # The runtime goes in a static library, as the CMake build has it: only
    # the parts the game uses are linked, so a runtime file with references
    # meant for another game or backend (nv2a_pb_replay.c) does no harm.
    src = os.path.join(TOOLKIT, "src")
    runtime = [o for s, o in objs if os.path.commonpath([s, src]) == src]
    game = [o for s, o in objs if os.path.commonpath([s, src]) != src]
    lib = os.path.join(os.path.dirname(exe), "libxboxrecomp.a")
    if os.path.exists(lib):
        os.remove(lib)
    ar = os.path.join(os.path.dirname(clangxx), "llvm-ar.exe")
    arsp = lib + ".rsp"
    with open(arsp, "w") as f:
        for o in runtime:
            f.write('"%s"\n' % o.replace("\\", "/"))
    p = subprocess.run([ar, "rcs", lib, "@" + arsp], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace")
    if p.returncode != 0:
        for line in p.stdout.strip().splitlines()[:20]:
            say("  " + line)
        fail("Making the runtime library failed. The log above has the details.")
    rsp = exe + ".rsp"
    with open(rsp, "w") as f:
        for o in game:
            f.write('"%s"\n' % o.replace("\\", "/"))
        f.write('"%s"\n' % lib.replace("\\", "/"))
    # The runtime's "#pragma comment(lib, ...)" lines name MSVC's libraries
    # (xaudio2, xinput, d3dcompiler). mingw has those names for other DLL
    # versions, or not at all, so point each name at the DLL the MSVC build
    # uses, ahead of the compiler's own folder.
    alias = os.path.join(os.path.dirname(exe), "libalias")
    os.makedirs(alias, exist_ok=True)
    libdir = os.path.join(os.path.dirname(os.path.dirname(clangxx)), "x86_64-w64-mingw32", "lib")
    for name, real in (("xaudio2", "xaudio2_8"), ("xinput", "xinput1_4"),
                       ("d3dcompiler", "d3dcompiler_47")):
        shutil.copyfile(os.path.join(libdir, "lib%s.a" % real),
                        os.path.join(alias, "lib%s.a" % name))
    args = [clangxx, "-mwindows", "-static", "-L" + alias, "-o", exe, "@" + rsp]
    args += ["-l" + l for l in LIBS]
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace")
    if p.returncode != 0:
        for line in p.stdout.strip().splitlines()[:40]:
            say("  " + line)
        fail("Linking failed. The log above has the details.")
    os.remove(rsp)


# ── 4. Lay out the game folder ───────────────────────────────────────────
def assemble(exe_built, out):
    progress(94, "Putting the game in place")
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, "Panzer Dragoon Orta.exe")
    shutil.copyfile(exe_built, exe)
    for f in ("engine.ini", "settings.ini", "README.txt"):
        src = os.path.join(ORTA, "package", f)
        if f == "settings.ini" and os.path.isfile(os.path.join(out, f)):
            continue                      # reinstalling: keep the player's settings
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(out, f))
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", required=True, help="disc image (.iso) or game folder")
    ap.add_argument("--out", required=True, help="where to put the playable game")
    ap.add_argument("--cc", required=True, help="llvm-mingw bin folder")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2)))
    ap.add_argument("--any-version", action="store_true",
                    help="try even if default.xbe is not the supported version")
    a = ap.parse_args()
    t0 = time.time()

    progress(1, "Reading the game files")
    # The game's files go straight into the install folder (one copy, not
    # two); the pipeline only needs default.xbe and the seeds beside it.
    installed_files = os.path.join(a.out, "game_files")
    gather_game_files(a.source, installed_files)
    game_files = os.path.join(TOOLKIT, "game_files")
    shutil.rmtree(game_files, ignore_errors=True)
    os.makedirs(game_files)
    xbe = os.path.join(game_files, "default.xbe")
    shutil.copyfile(os.path.join(installed_files, "default.xbe"), xbe)
    digest = sha1_of(xbe)
    say("default.xbe sha1 %s" % digest)
    if SUPPORTED_XBE and digest not in SUPPORTED_XBE and not a.any_version:
        fail("This copy of the game is a version this build does not support yet "
             "(only the North American release is). default.xbe sha1: %s" % digest)
    for f in ("seeds_manual.json", "alias_seeds.json", "trace_functions.json"):
        shutil.copyfile(os.path.join(ORTA, "seeds", f), os.path.join(game_files, f))

    # A retry after a failed run skips translating when nothing it reads changed.
    h = hashlib.sha1(digest.encode())
    for f in ("seeds_manual.json", "alias_seeds.json", "trace_functions.json"):
        with open(os.path.join(ORTA, "seeds", f), "rb") as fh:
            h.update(fh.read())
    with open(os.path.join(ORTA, "src", "recomp_manual.c"), "rb") as fh:
        h.update(fh.read())
    stamp = os.path.join(ROOT, "_obj", "translation.stamp")
    gen_dir = os.path.join(ORTA, "src", "recomp", "gen")
    if (os.path.isfile(stamp) and open(stamp).read() == h.hexdigest() and
            os.path.isfile(os.path.join(gen_dir, "recomp_dispatch.c"))):
        say("Translation is up to date from the last run")
        gen = gen_dir
    else:
        if os.path.isfile(stamp):
            os.remove(stamp)
        gen = translate()
        os.makedirs(os.path.dirname(stamp), exist_ok=True)
        with open(stamp, "w") as fh:
            fh.write(h.hexdigest())
    clangxx, objs = compile_all(a.cc, gen, os.path.join(ROOT, "_obj"), a.jobs)
    built = os.path.join(ROOT, "_obj", "orta.exe")
    link(clangxx, objs, built)
    exe = assemble(built, a.out)
    # The installed folder has its own copy of everything; free the space.
    for d in (game_files, os.path.join(ROOT, "_obj"), gen):
        shutil.rmtree(d, ignore_errors=True)
    progress(100, "Done in %d minutes" % max(1, round((time.time() - t0) / 60)))
    say("@@DONE " + exe)


if __name__ == "__main__":
    main()
