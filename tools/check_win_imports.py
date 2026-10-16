#!/usr/bin/env python3
"""Fails if a Windows binary needs a DLL that a pilot's PC does not have.

Windows refuses to load a DLL when any DLL in its import table cannot be
found, and for a plugin all X-Plane's Log.txt then says is

    .../XRadio/win_x64/XRadio.xpl : Error Code = 126 : The specified module could not be found.

A MinGW build that does not link its runtime statically imports
libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll (and libssp-0.dll
for Opus's stack protector). The machine that built it has them on PATH, so
the plugin works there and nowhere else. This reads the import table -- the
list `objdump -p` prints as "DLL Name:" -- and lets a DLL through only if it
is

  * part of Windows: the list below, and the api-ms-win-* API sets;
  * one X-Plane provides: XPLM_64.dll, XPWidgets_64.dll, and fmod.dll and
    fmodstudio.dll, which it loads from Resources/dlls/64 before any plugin;
  * the Visual C++ runtime (MSVCP140.dll, VCRUNTIME140.dll ...) that an MSVC
    build with /MD needs. It is not part of Windows, but the Visual C++
    Redistributable is already on any PC running plugins built that way --
    XRadio's own CI build among them;
  * shipped in the same folder as the binary: X-Plane finds those.

The MinGW runtime fails even when it is shipped next to the binary: another
plugin may already have loaded its own libstdc++-6.dll into X-Plane, and
Windows hands every later plugin asking for that name the one already loaded.

    python3 tools/check_win_imports.py build/XRadio/win_x64/XRadio.xpl build/xradio_server.exe

Stdlib only and no objdump needed, so it runs wherever Python does: in the
release script, in the Windows build, and in TowerTrainer's setup wizard,
which carries a copy of this file.
"""

import os
import re
import struct
import sys

# Each of these ships with every Windows 10 and 11 install. Add a name only if
# that is true of it: being on this list is what lets a DLL through.
SYSTEM_DLLS = {
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "cfgmgr32.dll", "comctl32.dll",
    "comdlg32.dll", "credui.dll", "crypt32.dll", "d2d1.dll", "d3d9.dll",
    "d3d11.dll", "d3d12.dll", "d3dcompiler_47.dll", "dbghelp.dll", "dcomp.dll",
    "dinput8.dll", "dnsapi.dll", "dwmapi.dll", "dwrite.dll", "dxgi.dll",
    "dxva2.dll", "gdi32.dll", "glu32.dll", "hid.dll", "imm32.dll", "iphlpapi.dll",
    "kernel32.dll", "ksuser.dll", "mpr.dll", "msacm32.dll", "msimg32.dll",
    "msvcrt.dll", "mswsock.dll", "ncrypt.dll", "netapi32.dll", "normaliz.dll",
    "ntdll.dll", "ole32.dll", "oleacc.dll", "oleaut32.dll", "opengl32.dll",
    "powrprof.dll", "propsys.dll", "psapi.dll", "rpcrt4.dll", "secur32.dll",
    "setupapi.dll", "shcore.dll", "shell32.dll", "shlwapi.dll", "ucrtbase.dll",
    "urlmon.dll", "user32.dll", "userenv.dll", "uxtheme.dll", "version.dll",
    "windowscodecs.dll", "winhttp.dll", "wininet.dll", "winmm.dll", "wintrust.dll",
    "wldap32.dll", "ws2_32.dll", "wsock32.dll", "wtsapi32.dll", "xinput1_4.dll",
}
XPLANE_DLLS = {"xplm_64.dll", "xpwidgets_64.dll", "fmod.dll", "fmodstudio.dll"}
# Release builds only: msvcp140d.dll and the other debug runtimes are on no
# pilot's PC, and they do not match.
VC_RUNTIME = re.compile(
    r"^(msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|vcruntime140(_1)?|concrt140|vcomp140)\.dll$",
    re.IGNORECASE)
# GCC's runtime as MinGW-w64, LLVM-MinGW and Cygwin/MSYS2 ship it.
MINGW_RUNTIME = re.compile(
    r"^(lib(stdc\+\+|gcc_s|winpthread|ssp|gomp|atomic|quadmath|gfortran|mcfgthread)([-_].*)?"
    r"|libc\+\+|libunwind|cygwin1|msys-2\.0)\.dll$",
    re.IGNORECASE)


def imported_dlls(path):
    """The DLL names in a PE file's import table, in the order they appear.

    That is the table the loader resolves when the binary is loaded, and the
    one that fails with error 126. Delay-loaded DLLs are not in it: Windows
    looks for those only when one of their functions is first called.
    Raises ValueError for anything that is not a readable PE file.
    """
    with open(path, "rb") as f:
        data = f.read()
    try:
        if data[:2] != b"MZ":
            raise ValueError("no MZ header")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("no PE header")
        nsections, = struct.unpack_from("<H", data, pe + 6)
        optsize, = struct.unpack_from("<H", data, pe + 20)
        opt = pe + 24
        magic, = struct.unpack_from("<H", data, opt)
        if magic == 0x20B:      # PE32+, 64-bit
            ndirs, = struct.unpack_from("<I", data, opt + 108)
            dirs = opt + 112
        elif magic == 0x10B:    # PE32
            ndirs, = struct.unpack_from("<I", data, opt + 92)
            dirs = opt + 96
        else:
            raise ValueError(f"unknown optional header 0x{magic:x}")
        if ndirs < 2:
            return []
        imports_rva, = struct.unpack_from("<I", data, dirs + 8)
        if imports_rva == 0:
            return []

        sections = []
        for i in range(nsections):
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, opt + optsize + 40 * i + 8)
            sections.append((va, max(vsize, rawsize), rawsize, rawptr))

        def offset(rva):
            for va, size, rawsize, rawptr in sections:
                if va <= rva < va + size and rva - va < rawsize:
                    return rawptr + rva - va
            raise ValueError(f"address 0x{rva:x} is in no section")

        names = []
        desc = offset(imports_rva)
        # 20 bytes per DLL. The loader stops at the first entry without a name
        # or an address table; a real table ends with an all-zero one, and a
        # broken one ends at the cap.
        for _ in range(4096):
            _lookup, _stamp, _chain, name_rva, thunks = struct.unpack_from("<IIIII", data, desc)
            if name_rva == 0 or thunks == 0:
                break
            start = offset(name_rva)
            names.append(data[start:data.index(b"\0", start)].decode("ascii", "replace"))
            desc += 20
        return names
    except struct.error as e:
        raise ValueError(f"truncated ({e})") from None


def provided(dll):
    """True for a DLL that Windows, X-Plane or the Visual C++ runtime supplies."""
    name = dll.lower()
    return (name in SYSTEM_DLLS or name in XPLANE_DLLS or VC_RUNTIME.match(name) is not None
            or name.startswith(("api-ms-win-", "ext-ms-win-")))


def check(path):
    """One binary. Returns {"ok", "imports", "mingw", "missing", "problem"}:
    mingw and missing name the DLLs at fault, and problem says in one sentence
    what is wrong with the binary (empty when nothing is)."""
    try:
        imports = imported_dlls(path)
    except (OSError, ValueError) as e:
        return {"ok": False, "imports": [], "mingw": [], "missing": [],
                "problem": f"cannot be read as a Windows binary: {e}"}
    try:
        shipped = {n.lower() for n in os.listdir(os.path.dirname(os.path.abspath(path)))}
    except OSError:
        shipped = set()
    mingw = [d for d in imports if MINGW_RUNTIME.match(d)]
    missing = [d for d in imports
               if d not in mingw and not provided(d) and d.lower() not in shipped]
    problems = []
    if mingw:
        problems.append(f"needs the MinGW runtime ({', '.join(mingw)}), which only a PC "
                        f"with the compiler installed has -- it must be linked with -static")
    if missing:
        problems.append(f"needs {', '.join(missing)}, which neither Windows nor X-Plane "
                        f"provides and which is not shipped next to it")
    return {"ok": not problems, "imports": imports, "mingw": mingw, "missing": missing,
            "problem": "; it also ".join(problems)}


def main(paths):
    if not paths:
        print(__doc__.strip())
        return 2
    bad = 0
    for path in paths:
        r = check(path)
        print(f"  {'ok  ' if r['ok'] else 'FAIL'}  {path}")
        if not r["ok"]:
            bad += 1
            print(f"        {r['problem']}")
        if r["imports"]:
            print(f"        imports {', '.join(r['imports'])}")
    print()
    if bad:
        print(f"{bad} of {len(paths)} Windows binaries would not load on a PC without the "
              f"compiler: Windows refuses them (X-Plane's Log.txt: Error Code = 126)")
        return 1
    print(f"{len(paths)} Windows {'binary needs' if len(paths) == 1 else 'binaries need'} "
          f"no DLL a pilot's PC lacks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
