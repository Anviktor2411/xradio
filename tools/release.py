#!/usr/bin/env python3
"""Cut a release: set the version, prove the build is sound, make the zip.

Run it and answer one question:

    $ python3 tools/release.py
    current version : v0.5.2
    new version [v0.5.3]: v0.6.0

It then sets the version in one place (plugin/src/brand.h -- the window and
the tests both read it from there), runs whatever checks this machine can
run, and writes dist/xradio-v0.6.0-source.zip ready to upload, plus a
release-notes skeleton with the version and compare link filled in.

Nothing is changed until the checks pass, so a failed test run leaves the
tree exactly as it was.

    --version v0.6.0   skip the question (for scripting)
    --no-tests         set the version and package without running anything
    --dry-run          say what would happen, change nothing
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BRAND = ROOT / "plugin" / "src" / "brand.h"
VERSION_RE = re.compile(r'version\(\)\s*\{\s*return\s*"([^"]+)"')

# Everything that is either generated, fetched, or not ours.
SKIP_DIRS = {"build", "build-tests", "lib", "SDK", "dist", ".git", "__pycache__",
             "Claude outputs"}


def say(line=""):
    print(line, flush=True)


def current_version():
    m = VERSION_RE.search(BRAND.read_text())
    if not m:
        sys.exit(f"could not find the version in {BRAND}")
    return m.group(1)


def next_patch(v):
    parts = v.split(".")
    if len(parts) == 3 and parts[2].isdigit():
        return f"{parts[0]}.{parts[1]}.{int(parts[2]) + 1}"
    return v


def normalise(v):
    """Accepts 0.5.3 or v0.5.3; stores the bare number, prints the v."""
    v = v.strip().lstrip("vV")
    if not re.fullmatch(r"\d+\.\d+\.\d+", v):
        return None
    return v


def set_version(new, dry):
    text = BRAND.read_text()
    old = current_version()
    if new == old:
        return old
    updated = text.replace(f'return "{old}";', f'return "{new}";', 1)
    # the example in the comment above draw(), so it does not go stale
    updated = updated.replace(f"v{old}` at (x, y)", f"v{new}` at (x, y)", 1)
    if updated == text:
        sys.exit("the version in brand.h did not change -- has the file moved on?")
    if not dry:
        BRAND.write_text(updated)
    return old


def have(cmd):
    return shutil.which(cmd) is not None


def run(name, argv, env=None):
    """One check. Returns True if it passed, and prints its tail if it did not."""
    say(f"  {name} ...")
    e = dict(os.environ)
    e.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    if env:
        e.update(env)
    try:
        p = subprocess.run(argv, cwd=ROOT, env=e, capture_output=True, text=True,
                           timeout=1800)
    except (OSError, subprocess.SubprocessError) as exc:
        say(f"    could not run it: {exc}")
        return False
    if p.returncode == 0:
        tail = [l for l in p.stdout.strip().splitlines() if l.strip()]
        if tail:
            say(f"    {tail[-1]}")
        return True
    say(f"    FAILED (exit {p.returncode})")
    for line in (p.stdout + p.stderr).strip().splitlines()[-25:]:
        say(f"    | {line}")
    return False


def checks():
    """Everything this machine is equipped to run, in the order CI runs it."""
    ok = True
    if have("bash"):
        ok &= run("portability", ["bash", "tools/check_portability.sh"])
        if have("x86_64-w64-mingw32-g++"):
            ok &= run("the plugin compiles for Windows",
                      ["bash", "tools/check_windows_build.sh"])
        else:
            say("  the plugin compiles for Windows ... skipped (no mingw-w64 here)")
        ok &= run("the CSL bridge compiles against real XPMP2",
                  ["bash", "tools/check_xpmp2_build.sh"])
    if have("python3"):
        ok &= run("server protocol and routing", ["python3", "tools/test_server.py"])

    if not (have("cmake") and (have("g++") or have("clang++"))):
        say("  the C++ tests ... skipped (no cmake/compiler here)")
        return ok

    build = ROOT / "build-tests"
    ok &= run("configuring the tests", [
        "cmake", "-B", str(build), "-DCMAKE_BUILD_TYPE=Debug",
        "-DXRADIO_BUILD_TESTS=ON", "-DXRADIO_USE_XPMP2=OFF",
        f"-DXPLANE_SDK={ROOT / 'tools' / 'fake_sdk'}"])
    ok &= run("building them", ["cmake", "--build", str(build), "-j", "4"])
    if not ok:
        return False

    for name, exe, args in [
        ("position smoothing", "smoothing_test", []),
        ("voice pipeline", "voice_test", []),
        ("window placement", "placement_test", []),
        ("settings window", "settings_test", []),
        ("hosting end to end", "hosting_test", ["49810"]),
    ]:
        ok &= run(name, [str(build / exe)] + args)

    if have("python3"):
        ok &= run("the two servers agree",
                  ["python3", "tools/test_server_parity.py",
                   "--cpp", str(build / "xradio_server")])
    return ok


def package(version, dry):
    dist = ROOT / "dist"
    out = dist / f"xradio-v{version}-source.zip"
    files = []
    for path in sorted(ROOT.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        if rel.suffix in (".zip", ".xpl", ".o", ".pyc"):
            continue
        files.append(rel)
    if dry:
        say(f"  would write {out.name} with {len(files)} files")
        return out, len(files)
    dist.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in files:
            z.write(ROOT / rel, str(rel))
    return out, len(files)


def notes(version, previous, dry):
    out = ROOT / "dist" / f"release-notes-v{version}.md"
    body = f"""## XRadio v{version}

One zip for Windows, macOS (Intel + Apple Silicon) and Linux. Unzip into
`X-Plane 12/Resources/plugins/` so you get `plugins/XRadio/`.

**Everyone flying together must be on this version** -- the wire protocol is
checked at login and a mismatched build is refused with a reason.

### New

- (what changed)

### Fixed

- (what was broken)

**Full Changelog**: https://github.com/Anviktor2411/xradio/compare/v{previous}...v{version}
"""
    if not dry:
        out.parent.mkdir(exist_ok=True)
        out.write_text(body)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version")
    ap.add_argument("--no-tests", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--yes", action="store_true", help="do not ask to confirm")
    args = ap.parse_args()

    say()
    say("XRadio release")
    say("--------------")
    old = current_version()
    say(f"current version : v{old}")

    if args.version:
        new = normalise(args.version)
        if not new:
            sys.exit(f"'{args.version}' is not a version like v0.5.3")
    else:
        suggestion = next_patch(old)
        while True:
            try:
                answer = input(f"new version [v{suggestion}]: ").strip()
            except EOFError:
                answer = ""
            new = normalise(answer or suggestion)
            if new:
                break
            say("  please write it like v0.5.3")

    if new == old:
        say(f"\nv{new} is already the current version.")
    say(f"new version     : v{new}")
    if args.dry_run:
        say("(dry run -- nothing will be written)")
    say()

    if not args.yes and not args.version and sys.stdin.isatty():
        if input("go ahead? [Y/n]: ").strip().lower() in ("n", "no"):
            say("nothing done.")
            return 1

    if not args.no_tests:
        say("Checking the build is sound (this takes a few minutes):")
        if not checks():
            say()
            say("Something failed, so the version was NOT changed and no zip was")
            say("written. Fix it and run this again.")
            return 1
        say()

    set_version(new, args.dry_run)
    verb = "would be set" if args.dry_run else "set"
    if new == old:
        say(f"version left at v{new}")
    else:
        say(f"version {verb} in {BRAND.relative_to(ROOT)}")

    zip_path, count = package(new, args.dry_run)
    if not args.dry_run:
        say(f"packaged {count} files into {zip_path.relative_to(ROOT)}")

    notes_path = notes(new, old, args.dry_run)
    if not args.dry_run:
        say(f"release notes skeleton in {notes_path.relative_to(ROOT)}")

    say()
    say("Next, by hand:")
    say(f"  1. Upload the source to GitHub (the zip above), or commit it.")
    say(f"  2. Wait for the build to go green, then download the")
    say(f"     XRadio-all-platforms artifact from that run.")
    say(f"  3. Draft a release tagged v{new}, paste the notes, attach that")
    say(f"     artifact -- the source zip is not what pilots install.")
    say()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        say("\nstopped.")
        sys.exit(1)
