#!/usr/bin/env python3
"""
Builds the *runtime kit* the editor's Export uses to link a project's C++ scripts and plugins into the player.

    packaging/make_kit.py <build_dir> <kit_dir> [--ninja ninja]

The kit holds everything needed to relink FlyPlayer with extra object files, without CMake and without the
engine sources: the static libraries and PlayerMain.o from <build_dir>, the exact link command (as a template),
the compile flags FlyPlayer was built with, and the headers user code is compiled against.

    <kit_dir>/
      kit.txt        key=value: compiler, its version, build type, platform
      link.txt       the link command, one argument per line; @KIT@, @OUT@ and @USER_OBJECTS@ are placeholders
      compile.txt    compile flags for user code, one per line
      lib/           the .a / .o files the link command names
      include/       engine headers  (CPluginAPI.h, raylib headers, ...)  -I
      sdk/include/   script SDK headers (fly.hpp, FlyScriptABI.h)         -I
      sdk/src/       fly_module.cpp, compiled with the user's scripts

Build the engine for the kit with a configuration meant for shipping: Release, and -DFLYENGINE_ENABLE_FLYCLOUD=OFF so
the result has no libcurl/OpenSSL dependency.
"""
import argparse, json, os, shlex, shutil, subprocess, sys

def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout

def commands(ninja, build_dir, target):
    out = run([ninja, "-C", build_dir, "-t", "commands", target])
    return [l for l in out.splitlines() if l.strip()]

def split_compound(line):
    """A ninja command line can chain several commands with &&; return them as token lists."""
    parts, cur = [], []
    for tok in shlex.split(line):
        if tok == "&&":
            parts.append(cur); cur = []
        else:
            cur.append(tok)
    parts.append(cur)
    return [p for p in parts if p and p != [":"]]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build_dir"); ap.add_argument("kit_dir"); ap.add_argument("--ninja", default="ninja")
    ap.add_argument("--target", default="FlyPlayer")
    a = ap.parse_args()
    build = os.path.abspath(a.build_dir); kit = os.path.abspath(a.kit_dir)
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    # --- link command
    link = None
    for line in commands(a.ninja, build, a.target):
        for part in split_compound(line):
            if "-o" in part and a.target in part[part.index("-o") + 1] and any(t.endswith(".o") for t in part):
                link = part
    if not link:
        sys.exit("could not find the link command of " + a.target)
    compiler = link[0]

    shutil.rmtree(kit, ignore_errors=True)
    os.makedirs(os.path.join(kit, "lib"))
    used = {}
    out_tokens = []
    skip_next = False
    for i, tok in enumerate(link):
        if skip_next:
            skip_next = False; continue
        if tok == "-o":
            out_tokens.append("-o"); out_tokens.append("@OUT@"); skip_next = True; continue
        if tok.startswith("-Wl,--dependency-file") or tok in ("-rdynamic", "-Wl,--export-dynamic", "-g") \
                or tok.startswith("-Wl,-rpath"):
            continue   # the exported game needs no exported symbols, no debug info and no build-tree rpath
        path = tok if os.path.isabs(tok) else os.path.join(build, tok)
        if (tok.endswith(".a") or tok.endswith(".o")) and os.path.isfile(path) and not path.startswith("/usr/"):
            name = os.path.basename(path)
            if name in used and used[name] != path:
                name = "%d_%s" % (len(used), name)
            used[name] = path
            shutil.copy2(path, os.path.join(kit, "lib", name))
            out_tokens.append("@KIT@/lib/" + name)
            if tok.endswith("PlayerMain.cpp.o"):
                out_tokens.append("@USER_OBJECTS@")
            continue
        out_tokens.append(tok)
    with open(os.path.join(kit, "link.txt"), "w") as f:
        f.write("\n".join(out_tokens) + "\n")

    # --- compile flags of PlayerMain.cpp (user code is compiled the same way, minus include paths)
    flags = []
    for line in commands(a.ninja, build, "CMakeFiles/%s.dir/src/PlayerMain.cpp.o" % a.target):
        for part in split_compound(line):
            if not part or part[0] != compiler:
                continue
            skip = False
            for j, tok in enumerate(part[1:], 1):
                if skip: skip = False; continue
                if tok in ("-o", "-MF", "-MT", "-include", "-c", "-MQ", "-isystem", "-I"):
                    skip = tok != "-c"; continue
                if tok.startswith(("-I", "-MD", "-MMD", "-W", "-Winvalid-pch")) or tok.endswith(".cpp") or tok == "-g":
                    continue
                if tok.startswith(("-D", "-std=", "-O", "-f", "-m", "-pthread")):
                    flags.append(tok)
    with open(os.path.join(kit, "compile.txt"), "w") as f:
        f.write("\n".join(flags) + "\n")

    # --- headers and SDK
    shutil.copytree(os.path.join(repo, "include"), os.path.join(kit, "include"))
    shutil.copytree(os.path.join(repo, "ScriptingSDK", "include"), os.path.join(kit, "sdk", "include"))
    shutil.copytree(os.path.join(repo, "ScriptingSDK", "src"), os.path.join(kit, "sdk", "src"))

    build_type = ""
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.isfile(cache):
        for l in open(cache):
            if l.startswith("CMAKE_BUILD_TYPE:"):
                build_type = l.strip().split("=", 1)[1]
    version = run([compiler, "--version"]).splitlines()[0]
    with open(os.path.join(kit, "kit.txt"), "w") as f:
        f.write("compiler=%s\ncompiler_version=%s\nbuild_type=%s\nplatform=%s\n" % (compiler, version, build_type, sys.platform))
    print("kit written to", kit)
    print("  %d libraries/objects, %d compile flags, compiler %s" % (len(used), len(flags), version))
    if build_type not in ("Release", "RelWithDebInfo", "MinSizeRel"):
        print("  note: this is a %s build; export from a Release build for a small, fast game" % (build_type or "non-Release"))

main()
