#!/usr/bin/env python3
"""Generate a Windows batch build for ps3-dev from its .vcxproj files.

mohasi builds with MSBuild + the SN Systems PS3 platform toolset, which only exists
inside a Visual Studio install. The dev VM has the Cell SDK (ppu-lv2-gcc) but no VS,
so MSBuild cannot resolve Platform=PS3. The compiler is the same either way
(PlatformToolset=GCC -> ppu-lv2-gcc), so this reads the projects and emits the same
compile/link commands directly - no VS, no make.

Emits build-ps3.bat: static libs first, then the app, then the .elf.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
REPO = Path(__file__).resolve().parent / "ps3-dev"

# expanded on the VM
WIN_REPO = r"C:\ps3-dev"
SDK = r"C:\usr\local\cell"
CC = rf"{SDK}\host-win32\ppu\bin\ppu-lv2-gcc.exe"
AR = rf"{SDK}\host-win32\ppu\bin\ppu-lv2-ar.exe"
OBJCOPY = rf"{SDK}\host-win32\ppu\bin\ppu-lv2-objcopy.exe"
CGC = rf"{SDK}\host-win32\Cg\bin\sce-cgc.exe"

# link order matters for GNU ld; this is mohasi's order from yo-player.vcxproj
LIBS = ["simple-lib-core", "simple-lib-https", "simple-lib-av", "simple-lib-app"]
APP = "yo-player"


def text_of(proj, tag):
    """First non-empty value of <tag> under a Release|PS3 ItemDefinitionGroup."""
    for idg in proj.findall(".//m:ItemDefinitionGroup", NS):
        cond = idg.get("Condition", "")
        if cond and "Release|PS3" not in cond:
            continue
        for el in idg.iter():
            if el.tag == f"{{{NS['m']}}}{tag}" and el.text:
                return el.text
    return ""


def expand(value, proj_dir_win):
    """Resolve the MSBuild macros these projects actually use."""
    value = value.replace("$(SolutionDir)", WIN_REPO + "\\")
    value = value.replace("$(SCE_PS3_ROOT)", SDK)
    value = value.replace("$(ProjectDir)", proj_dir_win + "\\")
    value = value.replace("$(Configuration)", "Release")
    # drop the "inherit" placeholders
    value = re.sub(r"%\([A-Za-z]+\)", "", value)
    return value


def split_list(value):
    return [p.strip() for p in value.split(";") if p.strip()]


def load(project_rel, name):
    """-> dict with sources, includes, defines, extra flags, config type."""
    path = REPO / project_rel / f"{name}.vcxproj"
    tree = ET.parse(path)
    root = tree.getroot()
    proj_dir_win = rf"{WIN_REPO}\{project_rel}".replace("/", "\\")

    sources = [c.get("Include") for c in root.findall(".//m:ClCompile[@Include]", NS)]

    # Also compile any .c on disk that the project file doesn't list. Upstream's list matches its tree
    # exactly, so this changes nothing for an untouched checkout - it just means a source file added by this
    # fork builds without having to edit mohasi's .vcxproj (which we never commit to).
    listed = {s.replace("\\", "/").lower() for s in sources}
    for found in sorted((REPO / project_rel / "src").rglob("*.c")):
        rel = found.relative_to(REPO / project_rel).as_posix()
        if rel.lower() not in listed:
            print(f"  note: {name} picks up {rel} (not in the .vcxproj)")
            sources.append(rel.replace("/", "\\"))
    includes = []
    for raw in split_list(text_of(root, "AdditionalIncludeDirectories")):
        inc = expand(raw, proj_dir_win).strip().replace("/", "\\")
        if not inc:
            continue                      # the %(...) inherit placeholder
        if not re.match(r"^[A-Za-z]:\\|^\\\\", inc):
            inc = rf"{proj_dir_win}\{inc}"   # vcxproj paths are project-relative
        includes.append(inc)
    defines = [d for d in split_list(text_of(root, "PreprocessorDefinitions"))
               if not d.startswith("%(")]
    # ClCompile/AdditionalOptions (libs use -ffunction-sections etc.)
    extra = ""
    for idg in root.findall(".//m:ItemDefinitionGroup", NS):
        cl = idg.find("m:ClCompile", NS)
        if cl is not None:
            ao = cl.find("m:AdditionalOptions", NS)
            if ao is not None and ao.text:
                extra = expand(ao.text, proj_dir_win)
    cfgtype = ""
    for pg in root.findall(".//m:PropertyGroup", NS):
        ct = pg.find("m:ConfigurationType", NS)
        if ct is not None and ct.text:
            cfgtype = ct.text
    # Cg shaders: <CustomBuild Include="x.cg"> -> sce-cgc -> objcopy -I binary -> .ppu.o
    # The objcopy input must be a BARE filename in the obj dir: the _binary_<name>_start
    # symbol gfx.c expects is derived from exactly that string.
    shaders = []
    for cb in root.findall(".//m:CustomBuild[@Include]", NS):
        inc = cb.get("Include")
        if not inc.lower().endswith(".cg"):
            continue
        cmd = "".join(cb.itertext())
        profile = "sce_vp_rsx" if "sce_vp_rsx" in cmd else "sce_fp_rsx"
        ext = "vpo" if profile == "sce_vp_rsx" else "fpo"
        shaders.append((inc, profile, ext))

    link = root.find(".//m:ItemDefinitionGroup/m:Link", NS)
    link_deps = link_opts = ""
    if link is not None:
        d = link.find("m:AdditionalDependencies", NS)
        o = link.find("m:AdditionalOptions", NS)
        if d is not None and d.text:
            link_deps = re.sub(r"%\([A-Za-z]+\)", "", d.text).replace(";", " ")
        if o is not None and o.text:
            link_opts = expand(o.text, proj_dir_win)
    return dict(name=name, dir=proj_dir_win, sources=sources, includes=includes,
                defines=defines, extra=extra, cfgtype=cfgtype, shaders=shaders,
                link_deps=link_deps, link_opts=link_opts)


def obj_name(src):
    return re.sub(r"[\\/]", "_", src).rsplit(".", 1)[0] + ".o"


def emit_project(out, p, is_app):
    inc = " ".join(f'-I"{i}"' for i in p["includes"])
    dfn = " ".join(f"-D{d}" for d in p["defines"])
    objdir = rf'{p["dir"]}\obj'
    bindir = rf'{p["dir"]}\bin\Release'
    out.append(f'echo === {p["name"]} ({len(p["sources"])} sources) ===')
    out.append(f'if not exist "{objdir}" mkdir "{objdir}"')
    out.append(f'if not exist "{bindir}" mkdir "{bindir}"')
    # -Werror is mohasi's; we leave it off so a stray warning cannot stop the build.
    # (-O2 throughout, as upstream. The app was briefly built -Os to fit the EBOOT inside a repacked PKG's
    # fixed data section; packaging now goes through the SDK's own make_package_npdrm, so there is no size
    # ceiling to optimise against any more.)
    common = f'-O2 -Wall {p["extra"]} {dfn} -DBUILD_NAME=\\"{p["name"]}\\" -DBUILD_STAMP=\\"%STAMP%\\"'
    # Every object is removed first, and every compile is followed by an EXPLICIT errorlevel test.
    #
    # Both matter, and the reason is a build that lied: `gcc ... || goto :failed` did not stop this batch
    # when a compile failed. The link then globbed *.o and quietly picked up the PREVIOUS build's object for
    # the file that had just failed, so the batch printed ALL-OK and produced an .elf that silently did not
    # contain the change - a package was shipped and tested on that basis. `if errorlevel 1 goto :failed` on
    # its own line is the form cmd honours; wiping the objdir means a missed compile can only ever show up
    # as a loud link error, never as stale code.
    out.append(f'del /q "{objdir}"\\*.o 2>nul')
    for src in p["sources"]:
        o = rf'{objdir}\{obj_name(src)}'
        s = rf'{p["dir"]}\{src}'.replace("/", "\\")
        out.append(f'"{CC}" -c {common} {inc} -o "{o}" "{s}"')
        out.append('if errorlevel 1 goto :failed')
    for cg, profile, ext in p["shaders"]:
        base = cg.rsplit(".", 1)[0]
        out.append(f'echo   shader {cg} ({profile})')
        out.append(f'pushd "{objdir}"')
        out.append(f'"{CGC}" -quiet -profile {profile} -o "{base}.{ext}" "{p["dir"]}\\{cg}"')
        out.append('if errorlevel 1 (popd & goto :failed)')
        out.append(f'"{OBJCOPY}" -I binary -O elf64-powerpc-celloslv2 -B powerpc '
                   f'"{base}.{ext}" "{base}.ppu.o"')
        out.append('if errorlevel 1 (popd & goto :failed)')
        out.append('popd')

    if is_app:
        elf = rf'{bindir}\{p["name"]}.ppu.elf'
        libdirs = f'-L"{SDK}\\target\\ppu\\lib"'
        out.append(f'echo === linking {p["name"]}.ppu.elf ===')
        # (no --gc-sections: the SN linker rejects it as deprecated and dead-strips via the
        # --strip-unused-data already in the project's link options, so it is a no-op plus a warning)
        out.append(f'"{CC}" -o "{elf}" "{objdir}"\\*.o {p["link_opts"]} {libdirs} '
                   f'{p["link_deps"]} -lm')
        out.append('if errorlevel 1 goto :failed')
        out.append(f'echo BUILT: {elf}')
    else:
        a = rf'{bindir}\lib{p["name"]}.a'
        out.append(f'if exist "{a}" del "{a}"')
        out.append(f'"{AR}" rcs "{a}" "{objdir}"\\*.o')
        out.append('if errorlevel 1 goto :failed')
        out.append(f'echo BUILT: {a}')
    out.append("")


def main():
    out = ["@echo off", "setlocal enabledelayedexpansion",
           'set "PATH=%PATH%;C:\\Program Files\\Git\\cmd"',
           'for /f %%i in (\'git -C C:\\ps3-dev rev-list --count HEAD 2^>nul\') do set "CNT=%%i"',
           'for /f %%i in (\'git -C C:\\ps3-dev rev-parse --short HEAD 2^>nul\') do set "SHA=%%i"',
           'if "%CNT%"=="" set "CNT=0"', 'if "%SHA%"=="" set "SHA=unknown"',
           'set "STAMP=%CNT%-%SHA%"', 'echo build stamp: %STAMP%', ""]
    for lib in LIBS:
        emit_project(out, load(f"libs/{lib}", lib), is_app=False)
    emit_project(out, load(f"apps/{APP}", APP), is_app=True)
    out += ["echo.", "echo ALL-OK", "exit /b 0", "", ":failed",
            "echo.", "echo BUILD-FAILED", "exit /b 1"]
    dst = Path(__file__).resolve().parent / "build-ps3.bat"
    dst.write_text("\r\n".join(out), encoding="ascii")
    print(f"wrote {dst} ({len(out)} lines)")


if __name__ == "__main__":
    sys.exit(main())
