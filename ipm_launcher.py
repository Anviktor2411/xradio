#!/usr/bin/env python3
"""ISO Package Manager - single entry point (launcher).

Open the built executable (or run ``python ipm_launcher.py`` with no arguments)
and it asks which interface to start:

    1)  UI        - the Tk window (main.py)
    2)  Terminal  - the text interface (ipm_cli.py, search / url / info / fetch / list)

Scripted or shortcut use is never blocked by the prompt - any argument selects a
mode directly::

    "ISO Package Manager.exe" --cli search ubuntu -n 5      # terminal, one shot
    "ISO Package Manager.exe" --cli                         # interactive shell
    "ISO Package Manager.exe" search "plan 9"               # args imply terminal
    "ISO Package Manager.exe" --gui                         # UI, no question
    "ISO Package Manager.exe"                               # asks: UI or Terminal

The GUI spawns a helper process for its in-app browser window using the internal
``--webview`` flag; the launcher forwards that (and its companion flags) straight
to the Tk application so the helper never shows the menu again.
"""

from __future__ import annotations

import os
import shlex
import sys

APP_NAME = "ISO Package Manager"
APP_VERSION = "0.10.1"

MENU = """
  +--------------------------------------------------+
  |  {name} V{version}                       |
  +--------------------------------------------------+
  |  1)  UI        - graphical window (Tk)           |
  |  2)  Terminal  - text interface (search/fetch)   |
  |  Q)  Quit                                        |
  +--------------------------------------------------+
""".format(name=APP_NAME, version=APP_VERSION)

_SHELL_BANNER = (
    "{name} terminal edition.\n"
    "  search <os> [-n 10] [-s \"Archive.org (any ISO)\"]   find ISO images\n"
    "  url <os>                                          print just the best URL\n"
    "  info <os> [--head]                                details for one result\n"
    "  fetch <os> [-d DIR] [-y]                          download the best match\n"
    "  list [archive|windows|linux]                      known sources\n"
    "  help                                              full option list\n"
    "  exit / quit                                       leave the terminal\n"
    "Note: 'help' lists everything; unknown OS families work with -s \"Archive.org (any ISO)\"."
).format(name=APP_NAME)


def _use_utf8_console() -> None:
    """Best effort: make the console UTF-8 and VT-aware so output is readable."""
    if os.name != "nt":
        return
    try:
        os.system("")  # enables ANSI escape handling in modern conhost
    except Exception:
        pass
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Mode selection
# ---------------------------------------------------------------------------

def ask_mode() -> str:
    """Return 'gui', 'cli' or 'quit' for an interactive double-click start."""
    if not (sys.stdin and sys.stdin.isatty()):
        # No console to type into (e.g. started from a shortcut/service) - the
        # only useful default is the window.
        return "gui"

    print(MENU)
    for attempt in range(3):
        try:
            answer = input("  Choose 1/2 [1]: ").strip().lower()
        except KeyboardInterrupt:
            print()
            return "quit"
        except EOFError:
            print()
            return "gui"

        if answer in ("", "1", "u", "ui", "g", "gui", "window"):
            return "gui"
        if answer in ("2", "t", "term", "terminal", "c", "cli", "shell"):
            return "cli"
        if answer in ("q", "quit", "exit", "0"):
            return "quit"
        if attempt < 2:
            print("  please type 1 (UI), 2 (Terminal) or Q (quit).")
    print("  no valid choice - starting the UI.")
    return "gui"


# ---------------------------------------------------------------------------
# Mode runners
# ---------------------------------------------------------------------------

def run_gui() -> int:
    """Start the Tk application (also handles the internal --webview mode)."""
    import main as gui_app

    return int(gui_app.main())


def run_cli(argv: list[str]) -> int:
    """Run one CLI command, or open an interactive terminal when argv is empty."""
    import ipm_cli

    if argv:
        try:
            return int(ipm_cli.main(argv))
        except SystemExit as exc:  # argparse exits for bad options
            return int(exc.code) if isinstance(exc.code, int) else 1

    if not (sys.stdin and sys.stdin.isatty()):
        ipm_cli.build_parser().print_help()
        return 0
    return cli_shell(ipm_cli)


def cli_shell(ipm_cli) -> int:
    """Small read-eval-print loop so a double-clicked EXE stays usable."""
    print(_SHELL_BANNER)
    while True:
        try:
            line = input("ipm> ")
        except (EOFError, KeyboardInterrupt):
            print()
            return 0

        line = line.strip()
        if not line:
            continue
        low = line.lower()
        if low in ("exit", "quit", "q", ":q"):
            return 0
        if low in ("help", "?", "-h", "--help"):
            print(ipm_cli.build_parser().format_help())
            continue
        if low in ("cls", "clear"):
            os.system("cls" if os.name == "nt" else "clear")
            print(_SHELL_BANNER)
            continue

        try:
            argv = shlex.split(line, posix=False)
        except ValueError as exc:
            print("  cannot parse that line:", exc)
            continue
        argv = [tok[1:-1] if len(tok) > 1 and tok[0] == '"' and tok[-1] == '"' else tok for tok in argv]

        try:
            code = int(ipm_cli.main(argv))
        except SystemExit as exc:
            code = int(exc.code) if isinstance(exc.code, int) else 1
        except KeyboardInterrupt:
            print("\n  interrupted.")
            continue
        except Exception as exc:  # keep the shell alive on engine errors
            print(f"  error: {exc.__class__.__name__}: {exc}")
            continue

        if code not in (0, 2):
            print(f"  (exit {code})")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    _use_utf8_console()
    args = list(sys.argv[1:] if argv is None else argv)

    # Internal GUI helper process (in-app browser) - never prompt here.
    if "--webview" in args:
        return run_gui()

    if args:
        head = args[0].lower()
        if head in ("--gui", "--ui", "/gui"):
            return run_gui()
        if head in ("--cli", "--terminal", "--term", "--shell", "/cli"):
            return run_cli(args[1:])
        if head in ("-h", "--help", "-?", "/?"):
            print(__doc__)
            print(MENU)
            print("  --gui            start the UI without asking")
            print("  --cli [args...]  start the terminal without asking")
            print("  --version        show the version")
            return 0
        if head in ("-v", "--version"):
            print(f"{APP_NAME} V{APP_VERSION}")
            return 0
        # Any other argument is a CLI command (search/url/info/fetch/list).
        return run_cli(args)

    mode = ask_mode()
    if mode == "quit":
        print("  bye.")
        return 0
    if mode == "cli":
        return run_cli([])
    return run_gui()


if __name__ == "__main__":
    raise SystemExit(main())
