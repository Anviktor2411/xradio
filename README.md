# ISO Package Manager

A simple GUI app for finding, managing, and downloading `.iso` files.

**Theme Shop:** https://anviktor2411.github.io/iso-package-manager/ - browse, edit and share themes for the app.

## Features

- **Local** tab
  - Scan folders for `.iso` files
  - Filter by distro and search by name
  - Best-effort filename metadata parsing (distro / version / arch) shown in the list
  - Find duplicates by SHA-256
  - Copy path, extract, and mount + open / eject (Windows, Linux, macOS)
- **Internet** tab
  - Browse curated sources and list available ISO downloads
  - **Windows** category: Microsoft Windows 11, 10, 8.1, 8, 7, Vista and XP install media, plus the LTSC editions (Windows 11 / 10 Enterprise LTSC and their IoT Enterprise LTSC variants), from the Internet Archive catalogues
  - Download queue with a job list showing progress, transferred size, speed and ETA
  - Pause / resume, retry a failed job, open the target folder, remove or clear finished
    jobs - all of it works on several selected jobs at once
  - **Unfinished downloads are remembered.** Close the app mid-download, reopen it and
    press **Resume**: it continues from where it stopped instead of starting over
  - Optional auto-verify (SHA-256) when a checksum is known
  - Open official source pages
  - Multi-select ISOs in the list and download multiple files in one operation
  - Web search for direct `.iso` links (best-effort)
    - Deterministic-first option (try known mirrors/archives first)
    - Archive level control
    - Provider profiles (save/apply search provider settings)
    - Search history + favorites (right-click in the Web Search box)
  - Change how many web results are shown at a time (page size)
  - **Load more**
    - First reveals more already-fetched rows
    - Then (when all fetched items are already shown) fetches older/archived versions from official archives
- **Settings** tab
  - Theme, language, startup tab, scan folders and search provider settings, grouped into
    cards with a short explanation under each field
  - Everything is saved to your user profile and applied immediately
- **Logs** tab
  - In-app log viewer for errors and status updates
- **Themes**
  - Switch between `Default`, `Modern Dark`, `Windows XP`, and `Graphical` from the **Theme** menu
  - **Theme -> Get more themes...** opens the community shop *inside the app*: search,
    preview the palette, install and switch to a theme without touching the website
  - **Theme packs** - drop any `*.ipmtheme.json` file into a `themes` folder and it shows up
    in the same menu, after a separator. No rebuild, no code: a set of packs ships with the
    download (Catppuccin Mocha, Dracula, Gruvbox Dark, Nord, Solarized Light, plus whatever
    has been accepted into the shop since) and a fresh `.pyz` already carries them *inside*
    the archive, so even the single downloaded file has themes to pick from
  - Packs are looked for in `IPM_THEMES_DIR`, then the `themes` folder next to the app,
    then `~/.ipm/themes`, then `themes/` inside the running `.pyz` / PyInstaller bundle -
    the first folder that has a given id wins, so your own copy always overrides the packed-in one
  - Command line: `ipm_cli.py themes [--preview]`, and for authors
    `python tools/ipmtheme.py list | validate | new | install | preview | bundle`
  - Visual editor: `python tools/theme_editor.py` - live preview of every tab, WCAG contrast
    checks, colour-vision simulation (protanopia, deuteranopia, tritanopia, greyscale),
    palette import from a list of hex codes, presets and drafts
  - Docs: [`docs/THEME_FORMAT.md`](docs/THEME_FORMAT.md) (every field),
    [`docs/THEME_EDITOR.md`](docs/THEME_EDITOR.md) (the editor),
    [`docs/SHARING.md`](docs/SHARING.md) (publishing a pack),
    [`docs/THEME_PACKS.md`](docs/THEME_PACKS.md) (overview of the theme system)
  - **Theme Shop website** - browse, edit and upload community themes in the browser,
    see [Theme Shop](#theme-shop-website)
- **Languages**
  - Switch UI language from the **Language** menu (English, German, Spanish, French, Russian, Portuguese, Chinese)

See also: [Search Guide](SEARCH_GUIDE.md)

## Theme Shop (website)

**https://anviktor2411.github.io/iso-package-manager/**

A community gallery and theme editor for ISO Package Manager, served by GitHub Pages
from the [`docs/`](docs/) folder. No account is needed to browse or download.

The same shop is reachable from inside the app under **Theme -> Get more themes...**,
which is the quickest way to install one. The website is where you *make* and *upload* themes.

- **Shop** - every approved theme with a screenshot or a live mock-up, search, sorting
  and filters (dark / light / base theme). Open a theme to see its full palette and
  download it as `.ipmtheme.json` or `.ipmtheme.zip` (pack + screenshot)
- **Editor** - change the name, all 17 colours, font and style options with a live preview
  of every app tab, a random palette button and the same checks the app runs. It also has
  WCAG contrast rows with per-row fixes, colour-vision simulation, harmony generators,
  paste-a-palette import, undo/redo, drafts saved in the browser and a share link that
  carries the whole theme. Start from scratch or open any shop theme and remix it
- **Upload** - drop your `.ipmtheme.json` / `.zip` and a screenshot (PNG, JPG or WebP,
  up to 3 MB) and press **Continue on GitHub**. Needs a free GitHub account

### Installing a theme from the shop

The easy way: **Theme -> Get more themes...** in the app, pick a theme, press
**Install and use**. Nothing to download or copy by hand.

By hand, if you prefer:

1. Download the `.ipmtheme.json` or `.ipmtheme.zip`
2. `python tools/ipmtheme.py install <file>` - or copy the `.json` into the `themes`
   folder next to the app (or `~/.ipm/themes`)
3. Restart the app and pick the theme from the **Theme** menu

### How uploads are published

The site is static, so uploads go through GitHub instead of a server:

1. **Continue on GitHub** opens a *Share a theme* issue with the pack already filled in
   (label `theme`)
2. A maintainer reviews it and adds the `approved` label
3. The [`import-theme.yml`](.github/workflows/import-theme.yml) workflow validates the pack
   with `tools/ipmtheme.py validate --strict`, re-encodes the screenshot to PNG, commits
   `themes/<id>.ipmtheme.json` + `themes/<id>.png`, rebuilds `docs/shop/index.json`,
   comments on the issue and closes it
4. The shop reads the index straight from the repository, so the theme appears within a
   few minutes. If the pack fails validation, the bot removes `approved` and explains what to fix

Maintainer setup (once): **Settings -> Pages** = branch `main`, folder `/docs`;
**Settings -> Actions -> Workflow permissions** = *Read and write*; labels `theme` and `approved`.

`docs/shop/index.json` is generated by `python scripts/build_theme_index.py` - it lives
outside `themes/` because the app loads every `*.json` in that folder as a theme pack.

## Requirements

All platforms:

- Python 3.10 or newer
- Tkinter (included with the official installers on Windows/macOS; on Linux it is a separate distro package)

Linux extras (used by **Mount + Open** / **Eject ISO**; the app degrades gracefully without them):

- `udisks2` — provides `udisksctl`, which mounts loop devices without root
- `xdg-utils` — provides `xdg-open`, used to reveal a folder in your file manager
- `p7zip-full` — provides the `7z` binary used by **Extract**

```bash
sudo apt install python3-tk udisks2 xdg-utils p7zip-full     # Debian/Ubuntu/Mint
sudo dnf install python3-tkinter udisks2 xdg-utils p7zip     # Fedora
sudo pacman -S tk udisks2 xdg-utils p7zip                    # Arch/Manjaro
sudo zypper install python3-tk udisks2 xdg-utils p7zip       # openSUSE
```

Optional dependencies (from `requirements.txt`):

- `pywebview` (in-app browser window for “Open Source Page”)
- `certifi` (improves SSL certificate validation for some HTTPS downloads/searches on certain systems)

## Setup (recommended: virtual environment)

On many Linux distros, system Python is **externally managed** (PEP 668), so installing with `pip` globally is blocked. Use a virtual environment.

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -U pip
python -m pip install -r requirements.txt
```

If you already have a `.venv` but it behaves strangely, delete it and recreate it:

```bash
rm -rf .venv
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
```

## Run

```bash
python3 ipm_launcher.py
```

The launcher asks whether to start the **UI** or the **Terminal** interface. Passing
any argument skips the question:

```bash
python3 ipm_launcher.py --gui                       # UI, no question
python3 ipm_launcher.py --cli search ubuntu -n 5    # Terminal, one shot
python3 ipm_launcher.py --version
```

You can still start the Tk window directly with `python3 main.py`.

If you are using a venv:

```bash
. .venv/bin/activate
python ipm_launcher.py
```

## Mounting an ISO

**Mount + Open** attaches the selected image and reveals it in your file manager;
**Eject ISO** detaches it again. Both report the backend they used in the status bar.

| Platform | Backend | Notes |
| --- | --- | --- |
| Windows | PowerShell `Mount-DiskImage` / `Dismount-DiskImage` | Drive letter is resolved automatically. |
| Linux | `udisksctl loop-setup` + `loop-mount` / `loop-delete` | No root needed; requires `udisks2` and a running desktop polkit agent. |
| Linux (fallback) | `mount -o loop,ro` + `umount` + `udisksctl loop-delete` | Used when `udisksctl` is missing; needs root or an `/etc/fstab` entry. |
| macOS | `hdiutil attach` / `hdiutil detach` | Mount point is read from `/Volumes/...`. |

On Linux and macOS the app detaches any image *it* attached when it exits, so no
loop device or volume is left behind. Images you mounted yourself are never touched.

If no mount backend is available, **Mount + Open** falls back to opening the `.iso`
with the default desktop application, and **Eject ISO** tells you to unmount it with
your OS tools instead of failing silently.

**Extract** uses the 7-Zip CLI (`7z`, `7zz`, `7za`, `7zr` on Linux/macOS; `7z.exe`,
`7za.exe`, `7zr.exe` on Windows) and works identically on all three platforms.

## Build a standalone executable

Windows:

```bat
build_exe.bat                 REM -> "dist\ISO Package Manager.exe"
```

### Linux

Two different Linux executables can be produced:

| Output | What it is | Can be built on |
| --- | --- | --- |
| `dist/iso-package-manager-<version>.pyz` | single-file Python zipapp with a `#!/usr/bin/env python3` shebang; needs `python3` + `tkinter` on the target | any OS, including Windows |
| `dist/iso-package-manager` | native ELF onefile binary, no Python on the target at all | Linux only (PyInstaller cannot cross-compile) |

**Portable `.pyz` — buildable from any machine, including Windows:**

```bash
python build_pyz.py                                # -> dist/iso-package-manager-0.10.1.pyz
chmod +x dist/iso-package-manager-0.10.1.pyz
./dist/iso-package-manager-0.10.1.pyz                 # asks: UI or Terminal
./dist/iso-package-manager-0.10.1.pyz --cli search ubuntu -n 5
./dist/iso-package-manager-0.10.1.pyz --gui
```

On Windows the same archive runs as `python dist\iso-package-manager-0.10.1.pyz`.

**Native ELF — run this on Linux:**

```bash
chmod +x build_linux.sh
./build_linux.sh              # -> dist/iso-package-manager
./build_linux.sh pyz          # -> the portable .pyz only (no compiler needed)
./build_linux.sh all          # pyz + native binary (+ .deb when dpkg-deb exists)
./build_linux.sh install      # build, then install to ~/.local/bin + a .desktop entry
./build_linux.sh deb          # build, then package dist/iso-package-manager_*.deb
./build_linux.sh clean        # remove build/ and dist/
```

Every target exits non-zero when something goes wrong, so `./build_linux.sh build || echo failed`
is safe in a script. `dist/iso-package-manager` is smoke-tested with `--version` before the
script reports success.

**No Linux machine at hand?** Either of these produces the native binary for you:

```bash
./build_linux_docker.sh all   # needs Docker; runs the whole build in ubuntu:22.04
```

or push to GitHub: `.github/workflows/build-linux.yml` builds the `.pyz`, the native
binary and a `.deb` on Ubuntu 22.04 (glibc 2.35) and 24.04 and uploads them as
downloadable artifacts on every push, pull request or manual run. Take the 22.04
("glibc-2.35") artifact for maximum compatibility — it also runs on Debian 12+, Mint,
Pop!_OS and current Fedora; the 24.04 binary is x86-64-v3 tuned and wants a recent CPU.

macOS can run `build_linux.sh` too (PyInstaller emits a Mach-O binary in `dist/`), but
that path has not been verified on a Mac in this project.

## Notes

- **Mount + Open** / **Eject ISO** work on Windows, Linux and macOS — see [Mounting an ISO](#mounting-an-iso).
- Current version: **V0.10.1**. It is set in one place (`ipm_launcher.py`) and changed
  with `python scripts/bump_version.py <new version>` — see
  [docs/RELEASING.md](docs/RELEASING.md).
- If `pywebview` is not installed, source pages open in your default browser.

### Archive / "older versions" settings

The app can fetch older versions from official distro archives when you use **Web Search** and click **Load more**.

Environment variables (optional):

- `IPM_MAX_ARCHIVE_ITEMS`
  - Caps how many archive ISO links can be collected per search.
  - Set to `0` or `-1` for unlimited (can be slow).
- `IPM_MAX_KALI_VERSIONS`
  - Caps how many Kali versions are scanned when searching without an explicit version.
- `IPM_MAX_MINT_VERSIONS`
  - Caps how many Linux Mint versions are scanned when searching without an explicit version.
- `IPM_MAX_ARCH_DATED_DIRS`
  - Caps how many dated directories are scanned for Arch/Artix when searching without an explicit date.

## Security / GitHub hygiene

- This repo includes a `.gitignore` to avoid committing virtual environments, caches, and downloaded ISO files.
- See `SECURITY.md` for vulnerability reporting guidance.
- If you enable Dependabot on GitHub, `.github/dependabot.yml` will check `requirements.txt` for dependency updates.

### Internet tab tips

- **Windows sources**
  - Pick **Category: Windows** and choose a release (`Windows 11`, `Windows 11 Enterprise LTSC`, `Windows 11 IoT Enterprise LTSC`, `Windows 10`, `Windows 10 Enterprise LTSC`, `Windows 10 IoT Enterprise LTSC`, `Windows 8.1`, `Windows 8`, `Windows 7`, `Windows Vista`, `Windows XP`) or `Windows (all versions)`.
  - The LTSC editions have their own sources, and typing `windows 11 ltsc`, `win 11 ltsc` or just `ltsc` in **Web Search** reaches them too (`ltsc`/`ltsb` on its own lists every LTSC edition). LTSC rows are tagged `[Win 11 LTSC]`, `[Win 11 IoT LTSC]`, `[Win 10 LTSC]` or `[Win 10 IoT LTSC]`.
  - **Refresh** lists genuine Microsoft install media found in the Internet Archive, with the file size next to each name.
  - Items are filtered: repacks/"lite" editions, cracked/activated builds and files under 300 MB are dropped, and results are matched to the requested release so e.g. XP discs do not show up under Windows 8. LTSC media is matched through the archive item title (Microsoft's LTSC file names never say "LTSC") and is kept out of the retail `Windows 11` / `Windows 10` lists, which also drops language-pack ISOs such as `CLIENT_LOF_PACKAGES_OEM.iso`.
  - **Open Source Page** goes to Microsoft's official download page for 10/11, the Windows 8 hub page for 8/8.1, and the Internet Archive search for 7/Vista/XP and the LTSC editions (Microsoft does not publish LTSC media on its consumer download pages).
  - **Load more** widens the catalogue search for more (older) builds.
  - The archive copies are user-contributed. Verify the SHA-256 of a downloaded ISO against Microsoft's official hash list before installing, and note that installing Windows still requires a valid licence key.

- **Windows Server sources**
  - Pick **Category: Windows Server** and choose a release (`Windows Server 2025`, `Windows Server 2022`, `Windows Server 2019`, `Windows Server 2016`, `Windows Server 2012 R2`, `Windows Server 2012`, `Windows Server 2008 R2`) or `Windows Server (all versions)` for the whole line, newest first.
  - Server releases are also reachable from **Category: Windows** (and **All**); typing `windows server`, `win server` or a specific build such as `windows server 2012 r2` in **Web Search** lists them too. Rows are tagged `[Srv 2025]`, `[Srv 2022]`, `[Srv 2019]`, `[Srv 2016]`, `[Srv 2012 R2]`, `[Srv 2012]` or `[Srv 2008 R2]`.
  - Current releases (2016 and newer) resolve from Microsoft's Evaluation Center media and the Internet Archive; retired releases (2012 R2 and older) come from archive copies, which is why 2008 R2 returns evaluation (GRMSX*) and volume-licence kits rather than retail discs.
  - Server results are filtered the same way as client media, plus two server-specific guards: desktop client discs are kept out of Server lists (a Windows 7 disc that shares an archive item with a 2008 R2 disc no longer shows up under `Windows Server 2008 R2`), and non-OS server products are excluded (`SQL Server`, `MSSQL`, `Exchange Server`, `SharePoint`, `Lync Server`, `System Center`, `BizTalk`, `MultiPoint Server`). Hyper-V Server is still listed, since it is a Windows Server install image.
  - Server 2012 and 2012 R2 share one base title, so R2 media is reported under `Windows Server 2012 R2` and never under `Windows Server 2012`.
  - **Open Source Page** goes to Microsoft's Evaluation Center for current Server releases and to the Internet Archive search for retired ones.
  - Server media is for learning and lab use. Install only in environments you are licensed and authorised to run, and verify hashes before use.

- **Multi-download**
  - Select multiple rows in the ISO list (Ctrl/Shift) and click **Download**.
  - If multiple items are selected, the app will ask for a **target folder** and download all selected ISOs into it.
  - **Copy URL** copies all selected URLs (one per line).

- **Downloads (queue)**
  - Downloads are queued and shown in the **Downloads** list.
  - Select a job and use **Pause** / **Resume**.
  - **Auto-verify (SHA-256)** checks downloaded files when a checksum is available.

- **Web Search (best-effort)**
  - The **Web Search** box tries to find direct `.iso` URLs and may crawl a few top result pages.
  - You can switch **Provider** between:
    - `DuckDuckGo`
    - `SearxNG` (more “Google-like” results; requires a SearxNG instance URL)
    - `Google API` (requires a Google API key + Custom Search Engine ID)
  - If Web Search returns 0 direct ISO links, try curated **Source** listings first.
  - For `Google API`:
    - Create an API key and enable the **Custom Search API**
    - Create a **Programmable Search Engine** (CSE) and copy its `cx` ID
    - Paste both into the app (do not commit keys to GitHub)
  - This can return 0 results if search engines rate-limit/block scraping or if pages hide links behind scripts.
  - Try more specific queries like:
    - `site:releases.ubuntu.com ubuntu iso`
    - `site:cdimage.debian.org debian live iso`
    - `site:download.opensuse.org tumbleweed iso`

