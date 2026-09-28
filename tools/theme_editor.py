#!/usr/bin/env python3
"""Theme Studio - a visual theme editor for ISO Package Manager.

It edits the same ``.ipmtheme.json`` pack format the CLI validates and the app
loads, in two levels of detail:

* **Simple** - the nine colours that actually shape a look (background, panel,
  text, muted, accent, danger, border, selection) plus name, author and base.
  The colours that are derived from those (``accent_text``, ``selection_fg``,
  ``tree_heading_fg``) are worked out for you.
* **Everything** - all 17 palette entries, every ``style.*`` switch, the font
  family and the full set of metadata fields.

Both modes paint the same live preview: a mock ISO Package Manager window, the
WCAG contrast table, and the exact JSON that will be written to disk.

::

    python tools/theme_editor.py                          # start a new theme
    python tools/theme_editor.py themes/nord.ipmtheme.json
    python tools/theme_editor.py --new "Midnight Blue" --base modern-dark
    python tools/theme_editor.py --check themes/*.ipmtheme.json   # no GUI
    python tools/theme_editor.py --self-test                      # verify itself

The GUI needs Tkinter (part of the standard library on Windows and macOS, and
``python3-tk`` on Debian/Ubuntu). ``--check`` and ``--self-test`` run without a
display, which is what makes it usable in CI.

Docs: ``docs/THEME_EDITOR.md``.  Format reference: ``docs/THEME_FORMAT.md``.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import io
import json
import re
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

HERE = Path(__file__).resolve().parent
for _candidate in (HERE.parent, HERE):
    if (_candidate / "ipm_themes.py").is_file():
        sys.path.insert(0, str(_candidate))
        break

import ipm_themes as T  # noqa: E402

EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2
EXIT_NO_TK = 3

EDITOR_TITLE = "Theme Studio - ISO Package Manager"
EDITOR_VERSION = "1.0"

try:  # Tk is optional so --check works on a headless machine
    import tkinter as tk
    from tkinter import colorchooser, filedialog, font as tkfont, messagebox, simpledialog, ttk

    TK_ERROR = ""
except Exception as exc:  # pragma: no cover - depends on the machine
    tk = None  # type: ignore[assignment]
    colorchooser = filedialog = tkfont = messagebox = simpledialog = ttk = None  # type: ignore[assignment]
    TK_ERROR = f"{exc.__class__.__name__}: {exc}"

TK_AVAILABLE = tk is not None


# ---------------------------------------------------------------------------
# Editor chrome (deliberately not themed: the editor must stay readable while
# you edit a theme that may be, say, white-on-yellow)
# ---------------------------------------------------------------------------

EDITOR_COLORS = {
    "bg": "#1b1f24",
    "panel": "#22272e",
    "sunken": "#16191d",
    "text": "#e6edf3",
    "muted": "#8b949e",
    "accent": "#2f81f7",
    "accent_text": "#ffffff",
    "border": "#30363d",
    "ok": "#3fb950",
    "warn": "#d29922",
    "err": "#f85149",
}

SIMPLE_KEYS: tuple[str, ...] = (
    "bg",
    "panel",
    "text",
    "muted",
    "accent",
    "accent_active",
    "danger",
    "border",
    "selection",
)

COLOR_GROUPS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("Window", ("bg", "panel", "border")),
    ("Text", ("text", "muted")),
    ("Buttons", ("accent", "accent_active", "accent_text", "danger", "danger_active")),
    (
        "Lists, tables and tabs",
        ("selection", "selection_fg", "tree_bg", "tree_heading_bg", "tree_heading_fg", "tab_bg", "tab_selected_bg"),
    ),
)

COLOR_LABELS: dict[str, str] = {
    "bg": "Window background",
    "panel": "Panels and cards",
    "text": "Text",
    "muted": "Hint text",
    "accent": "Accent button",
    "accent_active": "Accent (hover)",
    "danger": "Delete button",
    "danger_active": "Delete (hover)",
    "border": "Borders",
    "selection": "Selected row",
    "tree_bg": "Table background",
    "tree_heading_bg": "Table header",
    "tree_heading_fg": "Table header text",
    "tab_bg": "Tab (inactive)",
    "tab_selected_bg": "Tab (selected)",
    "accent_text": "Text on accent",
    "selection_fg": "Text on selection",
}

COLOR_HELP: dict[str, str] = {
    "bg": "Behind everything",
    "panel": "Toolbars, tables, side lists",
    "text": "Primary text on bg and panel",
    "muted": "Status bar, hints, descriptions",
    "accent": "Download / Scan / Save buttons",
    "accent_active": "Accent while hovered or pressed",
    "danger": "Stop / Delete buttons",
    "danger_active": "Delete while hovered or pressed",
    "border": "Separators, frame edges, focus ring",
    "selection": "Background of the selected row",
    "tree_bg": "Rows of the results table",
    "tree_heading_bg": "Results header bar",
    "tree_heading_fg": "Results header text",
    "tab_bg": "Unselected tabs",
    "tab_selected_bg": "The tab you are on",
    "accent_text": "Label on an accent button",
    "selection_fg": "Label on the selected row",
}

STYLE_LABELS: dict[str, str] = {
    "ttk_theme": "ttk engine",
    "button_relief": "Button relief",
    "tab_relief": "Tab relief",
    "tree_heading_relief": "Header relief",
    "disabled_bg": "Disabled button bg",
    "disabled_fg": "Disabled button text",
    "pressed_bg": "Pressed button bg",
    "focus_border": "Focus ring",
    "use_icons": "Use toolbar icons",
}

STYLE_HELP: dict[str, str] = {
    "ttk_theme": "auto | clam | alt | default | classic | vista | xpnative",
    "button_relief": "flat | raised | sunken | groove | ridge | solid",
    "tab_relief": "flat | raised | sunken | groove | ridge | solid",
    "tree_heading_relief": "flat | raised | sunken | groove | ridge | solid",
    "disabled_bg": "Colour, or a palette key such as panel",
    "disabled_fg": "Colour, or a palette key such as muted",
    "pressed_bg": "Empty means 'use accent_active'",
    "focus_border": "Colour, or a palette key such as accent",
    "use_icons": "The chunky 'Graphical' toolbar look",
}

STYLE_COLORS: tuple[str, ...] = ("disabled_bg", "disabled_fg", "pressed_bg", "focus_border")
RELIEFS: tuple[str, ...] = ("flat", "raised", "sunken", "groove", "ridge", "solid")

META_FIELDS: tuple[tuple[str, str, str], ...] = (
    ("name", "Name", "What users see in the Theme menu (max 48 characters)"),
    ("id", "Id", "Lowercase machine name: a-z, 0-9, '-', '_', '.'"),
    ("author", "Author", "Your name or handle"),
    ("version", "Version", "Bump this when you change colours, e.g. 1.0.1"),
    ("description", "Description", "One line about the theme"),
    ("license", "License", "MIT, CC0-1.0, ..."),
    ("homepage", "Homepage", "Optional https:// link"),
    ("font_family", "Font", "Empty means the base theme's font"),
)

GENERATORS: tuple[tuple[str, str, str], ...] = (
    ("readable", "Fix readability", "Move the text colours until every pair passes WCAG AA"),
    ("aaa", "High contrast", "Push every pair to WCAG AAA (7:1)"),
    ("invert", "Invert dark/light", "Flip the whole palette and fix the result"),
    ("grayscale", "Remove colour", "Keep the lightness, drop the hue"),
    ("random-dark", "Random dark", "A legible dark palette from a random hue"),
    ("random-light", "Random light", "A legible light palette from a random hue"),
    ("from-accent", "Build from accent", "Derive a whole palette from the current accent"),
)

_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
_RESERVED = tuple(T.BUILTIN_ORDER)

SAMPLE_ISOS: tuple[tuple[str, str, str], ...] = (
    ("Ubuntu 24.04 LTS", "6.0 GB", "Ready"),
    ("Debian 13 netinst", "0.6 GB", "Ready"),
    ("Fedora 42 Workstation", "2.2 GB", "Downloading"),
    ("Linux Mint 22", "2.9 GB", "Queued"),
)


# ---------------------------------------------------------------------------
# Pure helpers - no Tk, so they can be tested on a headless machine
# ---------------------------------------------------------------------------

def slugify(name: str) -> str:
    """``"Midnight Blue!"`` -> ``"midnight-blue"`` (the default pack id)."""
    slug = re.sub(r"[^a-z0-9]+", "-", (name or "").strip().lower()).strip("-")
    return slug or "my-theme"


def id_problem(pack_id: str, *, taken: Iterable[str] = ()) -> str:
    """Why this id would be rejected, or ``""`` when it is fine."""
    value = (pack_id or "").strip().lower()
    if not value:
        return 'id: required, e.g. "my-theme"'
    if value in _RESERVED:
        return f"id {value!r} is reserved by a built-in theme"
    if not _ID_RE.match(value):
        return 'id: use a-z, 0-9, "-", "_", "." (max 64 characters, must start with a letter or digit)'
    if value in {str(name).strip().lower() for name in taken}:
        return f"id {value!r} is already used by an installed theme"
    return ""


def name_problem(name: str) -> str:
    value = (name or "").strip()
    if not value:
        return "name: give the theme a name"
    if len(value) > 48:
        return f"name: {len(value)} characters - the limit is 48"
    return ""


def base_palette(base: str) -> dict[str, str]:
    """Every colour of a built-in base theme, including the derived ones."""
    theme = T.BUILTIN_THEMES.get((base or "").strip().lower()) or T.BUILTIN_THEMES[T.FALLBACK_THEME_ID]
    return {key: theme.colors[key] for key in T.COLOR_KEYS if key in theme.colors}


def base_style(base: str) -> dict[str, Any]:
    theme = T.BUILTIN_THEMES.get((base or "").strip().lower()) or T.BUILTIN_THEMES[T.FALLBACK_THEME_ID]
    return dict(theme.style)


def default_font(base: str) -> str:
    theme = T.BUILTIN_THEMES.get((base or "").strip().lower()) or T.BUILTIN_THEMES[T.FALLBACK_THEME_ID]
    return theme.font_family


def build_pack(
    *,
    name: str = "My Theme",
    pack_id: str = "",
    author: str = "",
    version: str = "1.0.0",
    description: str = "",
    license: str = "MIT",
    homepage: str = "",
    base: str = T.FALLBACK_THEME_ID,
    font_family: str = "",
    colors: Mapping[str, Any] | None = None,
    style: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """A complete, valid pack dict - the editor's single source of truth."""
    chosen = (base or T.FALLBACK_THEME_ID).strip().lower()
    if chosen not in T.BUILTIN_THEMES:
        chosen = T.FALLBACK_THEME_ID
    palette = {key: value for key, value in base_palette(chosen).items()}
    for key, value in (colors or {}).items():
        if key in T.COLOR_KEYS and isinstance(value, str) and T.is_color(value):
            palette[key] = value.strip()
    merged_style = base_style(chosen)
    for key, value in (style or {}).items():
        if key in T.STYLE_KEYS:
            merged_style[key] = value
    clean_name = (name or "").strip() or "My Theme"
    return {
        "$schema": T.SCHEMA_HINT,
        "schema_version": T.SCHEMA_VERSION,
        "id": ((pack_id or "").strip().lower() or slugify(clean_name)),
        "name": clean_name,
        "author": (author or "").strip(),
        "version": (version or "").strip() or "1.0.0",
        "description": (description or "").strip(),
        "license": (license or "").strip(),
        "homepage": (homepage or "").strip(),
        "base": chosen,
        "font_family": (font_family or "").strip(),
        "colors": palette,
        "style": merged_style,
    }


def style_overrides(values: Mapping[str, Any]) -> dict[str, Any]:
    """Only keep the style switches worth writing to a pack."""
    out: dict[str, Any] = {}
    for key in T.STYLE_KEYS:
        value = values.get(key)
        if key == "use_icons":
            if value:
                out[key] = True
            continue
        if value is None or value == "":
            if key in STYLE_COLORS and value is None:
                out[key] = None
            continue
        if key == "ttk_theme" and value == "auto":
            continue
        out[key] = value
    return out


def pack_from_theme(theme: T.Theme) -> dict[str, Any]:
    """A built-in theme or loaded pack -> a pack dict the editor can hold."""
    data = theme.to_dict()
    data["$schema"] = T.SCHEMA_HINT
    data["colors"] = {key: data["colors"][key] for key in T.COLOR_KEYS if key in data["colors"]}
    return normalize_pack(data)


def normalize_pack(data: Mapping[str, Any]) -> dict[str, Any]:
    """Repair a loaded pack so the editor always has every field to bind to."""
    if not isinstance(data, Mapping):
        raise T.ThemeError("a theme pack must be a JSON object")
    base = str(data.get("base") or T.FALLBACK_THEME_ID).strip().lower()
    if base not in T.BUILTIN_THEMES:
        base = T.FALLBACK_THEME_ID
    palette = base_palette(base)
    raw_colors = data.get("colors")
    if isinstance(raw_colors, Mapping):
        for key, value in raw_colors.items():
            if key in T.COLOR_KEYS and isinstance(value, str) and value.strip():
                palette[key] = value.strip()
    style = base_style(base)
    raw_style = data.get("style")
    if isinstance(raw_style, Mapping):
        for key, value in raw_style.items():
            if key in T.STYLE_KEYS:
                style[key] = value
    name = str(data.get("name") or "").strip() or "My Theme"
    pack_id = str(data.get("id") or "").strip().lower() or slugify(name)
    version = str(data.get("version") or "").strip() or "1.0.0"
    return {
        "$schema": str(data.get("$schema") or T.SCHEMA_HINT),
        "schema_version": data.get("schema_version") or T.SCHEMA_VERSION,
        "id": pack_id,
        "name": name,
        "author": str(data.get("author") or ""),
        "version": version,
        "description": str(data.get("description") or ""),
        "license": str(data.get("license") or ""),
        "homepage": str(data.get("homepage") or ""),
        "base": base,
        "font_family": str(data.get("font_family") or ""),
        "colors": palette,
        "style": style,
    }


def preview_colors(pack: Mapping[str, Any]) -> dict[str, str]:
    """The colours the preview should paint, even for an incomplete pack."""
    base = str(pack.get("base") or T.FALLBACK_THEME_ID).strip().lower()
    if base not in T.BUILTIN_THEMES:
        base = T.FALLBACK_THEME_ID
    colors = base_palette(base)
    raw = pack.get("colors")
    if isinstance(raw, Mapping):
        for key, value in raw.items():
            if key in T.COLOR_KEYS and isinstance(value, str) and value.strip():
                colors[key] = value.strip()
    return colors


def preview_state(pack: Mapping[str, Any]) -> dict[str, Any]:
    """Colour + style switches resolved exactly the way the app resolves them."""
    try:
        state = T.theme_from_pack(normalize_pack(pack)).style_kwargs()
    except T.ThemeError:
        state = dict(preview_colors(pack))
        state.update(base_style(str(pack.get("base") or T.FALLBACK_THEME_ID)))
        state.update({k: v for k, v in (pack.get("style") or {}).items() if k in T.STYLE_KEYS})
    for key in STYLE_COLORS:
        value = state.get(key)
        if isinstance(value, str) and value in state and T.is_color(state[value]):
            state[key] = state[value]
    state["font_family"] = str(pack.get("font_family") or "").strip() or default_font(str(pack.get("base") or ""))
    return state


def review(pack: Mapping[str, Any], *, taken_ids: Iterable[str] = ()) -> dict[str, Any]:
    """Everything the Checks panel shows: problems, warnings and contrast."""
    data = normalize_pack(pack)
    errors, warnings = T.validate_pack(data, "<editor>")
    extra: list[str] = []
    if name_problem(data["name"]):
        extra.append(name_problem(data["name"]))
    collision = id_problem(data["id"], taken=taken_ids)
    if collision and "reserved" not in collision:
        extra.append(collision)
    theme: T.Theme | None = None
    try:
        theme = T.theme_from_pack(data)
    except T.ThemeError as exc:
        extra.append(str(exc))
    rows = T.contrast_report(theme.colors) if theme is not None else []
    lows = [row for row in rows if row[2] is not None and not row[4]]
    return {
        "pack": data,
        "theme": theme,
        "errors": list(errors) + extra,
        "warnings": list(warnings),
        "rows": rows,
        "low": lows,
    }


def contrast_lines(rows: Sequence[tuple[str, str, float | None, float, bool]]) -> list[str]:
    """The contrast table as plain text lines (used by the GUI and --check)."""
    lines: list[str] = []
    for label, ratio_text, ratio, minimum, passed in rows:
        if ratio is None:
            lines.append(f"  n/a  {label:<26} {'-':>9}  (min {minimum:.1f}:1)")
        else:
            mark = " ok " if passed else "low "
            # WCAG: 4.5 is AA for body text, 7.0 is AAA; large text (min 3.0) is
            # AA at 3.0 and AAA at 4.5.
            aaa_min = 7.0 if minimum >= 4.5 else 4.5
            grade = "AAA" if ratio >= aaa_min else ("AA " if passed else "-- ")
            lines.append(f"  {mark} {label:<26} {ratio_text:>9}  {grade}  (min {minimum:.1f}:1)")
    return lines


#: Colour-vision simulation matrices (Viénot/Brettel style, good enough to spot
#: a palette that falls apart for a colour-blind user).
VISION_MATRICES: dict[str, tuple[tuple[float, float, float], ...]] = {
    "protanopia": ((0.567, 0.433, 0.0), (0.558, 0.442, 0.0), (0.0, 0.242, 0.758)),
    "deuteranopia": ((0.625, 0.375, 0.0), (0.7, 0.3, 0.0), (0.0, 0.3, 0.7)),
    "tritanopia": ((0.95, 0.05, 0.0), (0.0, 0.433, 0.567), (0.0, 0.475, 0.525)),
}
VISION_MODES = ("normal", "protanopia", "deuteranopia", "tritanopia", "greyscale")


def simulate_color(value: Any, mode: str) -> Any:
    """``value`` as a person with *mode* colour vision would see it."""
    if not mode or mode == "normal" or not isinstance(value, str):
        return value
    rgb = T.to_rgb(value)
    if rgb is None:
        return value
    if mode == "greyscale":
        grey = round(0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2])
        return "#%02x%02x%02x" % (grey, grey, grey)
    matrix = VISION_MATRICES.get(mode)
    if matrix is None:
        return value
    out = []
    for row in matrix:
        channel = row[0] * rgb[0] + row[1] * rgb[1] + row[2] * rgb[2]
        out.append(max(0, min(255, round(channel))))
    return "#%02x%02x%02x" % tuple(out)


def simulate_state(state: Mapping[str, Any], mode: str) -> dict[str, Any]:
    """A copy of a preview state with every colour run through the simulation."""
    out = dict(state)
    if not mode or mode == "normal":
        return out
    for key, value in list(out.items()):
        if isinstance(value, str) and T.is_color(value):
            out[key] = simulate_color(value, mode)
    return out


def palette_from_hex_list(text: str, current: Mapping[str, Any]) -> dict[str, str]:
    """Build a palette from a pasted list of hex colours (or a Coolors link)."""
    source = text.strip()
    if "coolors.co" in source:
        source = " ".join("#" + part for part in source.rstrip("/").split("/")[-1].split("-"))
    found: list[str] = []
    for match in re.findall(r"#?[0-9a-fA-F]{6}\b", source):
        hex_value = match if match.startswith("#") else "#" + match
        if T.is_color(hex_value) and hex_value.lower() not in found:
            found.append(hex_value.lower())
    if len(found) < 3:
        raise T.ThemeError("paste at least three hex colours, for example #1e1e2e #cdd6f4 #89b4fa")

    def lum(hex_value: str) -> float:
        rgb = T.to_rgb(hex_value) or (0, 0, 0)
        channels = []
        for raw in rgb:
            value = raw / 255
            channels.append(value / 12.92 if value <= 0.03928 else ((value + 0.055) / 1.055) ** 2.4)
        return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2]

    ordered = sorted(found, key=lum)
    darkest, lightest = ordered[0], ordered[-1]
    dark = lum(darkest) + lum(lightest) < 1.0
    background, foreground = (darkest, lightest) if dark else (lightest, darkest)
    middle = ordered[1:-1] or [found[0]]
    accent = middle[len(middle) // 2]
    colors = dict(current)
    colors.update({
        "bg": background,
        "text": foreground,
        "panel": T.mix(background, foreground, 0.12),
        "muted": T.mix(foreground, background, 0.35),
        "accent": accent,
        "accent_active": accent,
        "border": T.mix(background, foreground, 0.25),
        "selection": accent,
        "accent_text": T.best_text_on(accent),
        "selection_fg": T.best_text_on(accent),
    })
    fixed, _notes = T.auto_fix_colors(colors)
    return {k: v for k, v in fixed.items() if k in T.COLOR_KEYS}


def apply_generator(pack: Mapping[str, Any], key: str, *, seed: Any = None) -> dict[str, Any]:
    """Return a copy of *pack* with a generator's palette applied."""
    data = copy.deepcopy(dict(pack))
    colors = preview_colors(data)
    if key == "readable":
        new_colors, _notes = T.auto_fix_colors(colors)
    elif key == "aaa":
        new_colors = T.high_contrast_palette(colors, minimum=7.0)
    elif key == "invert":
        new_colors = T.invert_palette(colors)
    elif key == "grayscale":
        new_colors = T.grayscale_palette(colors)
    elif key == "random-dark":
        new_colors = T.random_palette(seed, dark=True)
    elif key == "random-light":
        new_colors = T.random_palette(seed, dark=False)
    elif key == "from-accent":
        new_colors = T.palette_from_accent(colors.get("accent", "#2f81f7"), dark=T.is_dark(colors.get("bg", "#101010")))
    else:
        raise T.ThemeError(f"unknown generator {key!r}")
    data["colors"] = {k: v for k, v in new_colors.items() if k in T.COLOR_KEYS}
    return normalize_pack(data)


def apply_preset(pack: Mapping[str, Any], preset_colors: Mapping[str, Any]) -> dict[str, Any]:
    """Take a preset's palette, keep this pack's name/id/author."""
    data = copy.deepcopy(dict(pack))
    data["colors"] = {
        key: str(value)
        for key, value in preset_colors.items()
        if key in T.COLOR_KEYS and isinstance(value, str) and T.is_color(value)
    }
    if not data["colors"]:
        raise T.ThemeError("that preset has no usable colours")
    return normalize_pack(data)


def preset_options() -> dict[str, dict[str, str]]:
    """Display name -> palette, for built-ins plus every installed pack."""
    try:
        return T.palette_sources(refresh=True)
    except Exception:  # pragma: no cover - only if a folder is unreadable
        return {name: base_palette(tid) for tid, name in T.BUILTIN_NAMES.items()}


def installed_ids() -> set[str]:
    try:
        return {theme.id.lower() for theme in T.get_registry().themes if not theme.is_builtin}
    except Exception:  # pragma: no cover
        return set()


def pack_json(pack: Mapping[str, Any]) -> str:
    return json.dumps(normalize_pack(pack), indent=2, ensure_ascii=False) + "\n"


def terminal_preview(pack: Mapping[str, Any]) -> str:
    """The ANSI-free mock window plus the contrast table, as plain text."""
    data = normalize_pack(pack)
    out: list[str] = []
    try:
        theme = T.theme_from_pack(data)
    except T.ThemeError as exc:
        return f"(cannot preview: {exc})"
    out.append(T.ansi_preview(theme, color=False))
    out.append("")
    out.append("WCAG contrast")
    out.extend(contrast_lines(T.contrast_report(theme.colors)))
    return "\n".join(out)


def read_pack_text(path: Path) -> str:
    """Read a pack file, unwrapping a ``.zip`` bundle if needed."""
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.lower().endswith(".json")]
            preferred = [n for n in names if n.lower().endswith("theme.json")] or names
            if not preferred:
                raise T.ThemeError(f"{path.name}: no .json theme file inside the bundle")
            return archive.read(sorted(preferred)[0]).decode("utf-8-sig", errors="replace")
    return path.read_text(encoding="utf-8-sig", errors="replace")


def load_pack_data(path: Path) -> tuple[dict[str, Any], str]:
    """Load a pack for *editing*, tolerating problems the app would reject."""
    path = Path(path)
    if not path.is_file():
        raise T.ThemeError(f"no such file: {path}")
    text = read_pack_text(path)
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise T.ThemeError(f"{path.name}: invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}") from exc
    if not isinstance(data, Mapping):
        raise T.ThemeError(f"{path.name}: a pack must be a JSON object")
    errors, _warnings = T.validate_pack(data, path.name)
    pack = normalize_pack(data)
    return pack, "; ".join(errors)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

_Base = tk.Tk if TK_AVAILABLE else object


class _ScrollFrame(ttk.Frame if TK_AVAILABLE else object):  # type: ignore[misc]
    """A vertically scrolling container with a ``body`` frame inside."""

    def __init__(self, parent: Any, **kwargs: Any) -> None:
        super().__init__(parent, **kwargs)
        self.canvas = tk.Canvas(
            self, borderwidth=0, highlightthickness=0, background=EDITOR_COLORS["bg"]
        )
        scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.canvas.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)
        self.body = tk.Frame(self.canvas, background=EDITOR_COLORS["bg"])
        self._window = self.canvas.create_window((0, 0), window=self.body, anchor="nw")
        self.body.bind("<Configure>", self._on_body)
        self.canvas.bind("<Configure>", self._on_canvas)
        for widget in (self.canvas, self.body):
            widget.bind("<Enter>", self._wheel_on)
            widget.bind("<Leave>", self._wheel_off)

    def _on_body(self, event: Any) -> None:
        self.canvas.configure(scrollregion=self.canvas.bbox("all"))

    def _on_canvas(self, event: Any) -> None:
        self.canvas.itemconfigure(self._window, width=event.width)

    def _wheel_on(self, _event: Any) -> None:
        self.canvas.bind_all("<MouseWheel>", self._on_wheel)

    def _wheel_off(self, _event: Any) -> None:
        self.canvas.unbind_all("<MouseWheel>")

    def _on_wheel(self, event: Any) -> None:
        step = -1 if getattr(event, "delta", 0) > 0 else 1
        self.canvas.yview_scroll(step, "units")


class ThemeEditor(_Base):  # type: ignore[misc,valid-type]
    """The editor window. Every mutation goes through the small API at the
    bottom of this class so ``--self-test`` can drive it without a mouse."""

    def __init__(
        self,
        pack: Mapping[str, Any] | None = None,
        path: str | Path | None = None,
        *,
        test_mode: bool = False,
    ) -> None:
        if not TK_AVAILABLE:
            raise RuntimeError(f"Tkinter is not available ({TK_ERROR})")
        super().__init__()
        self.test_mode = bool(test_mode)
        self.path = Path(path) if path else None
        self.pack = normalize_pack(pack if pack is not None else build_pack())
        self._undo: list[dict[str, Any]] = []
        self._redo: list[dict[str, Any]] = []
        self._loading = False
        self._refresh_job: Any = None
        self._saved_json = pack_json(self.pack) if path else ""
        self._generator_seed: Any = None

        self.title(EDITOR_TITLE)
        self.configure(background=EDITOR_COLORS["bg"])
        self.geometry("1240x780")
        self.minsize(1000, 640)

        self.mode = tk.StringVar(value="simple")
        self.status = tk.StringVar(value="")
        self.preset_name = tk.StringVar(value="")
        self.use_icons = tk.BooleanVar(value=bool(self.pack["style"].get("use_icons")))
        self.meta_vars: dict[str, tk.StringVar] = {
            key: tk.StringVar(value=str(self.pack["colors"].get(key, "")))
            for key in ()  # placeholder, replaced below
        }
        self.meta_vars = {
            "name": tk.StringVar(value=self.pack["name"]),
            "id": tk.StringVar(value=self.pack["id"]),
            "author": tk.StringVar(value=self.pack["author"]),
            "version": tk.StringVar(value=self.pack["version"]),
            "description": tk.StringVar(value=self.pack["description"]),
            "license": tk.StringVar(value=self.pack["license"]),
            "homepage": tk.StringVar(value=self.pack["homepage"]),
            "font_family": tk.StringVar(value=self.pack["font_family"]),
        }
        self.base_var = tk.StringVar(value=self.pack["base"])
        self.color_vars: dict[str, tk.StringVar] = {
            key: tk.StringVar(value=self.pack["colors"][key]) for key in T.COLOR_KEYS
        }
        self.style_vars: dict[str, tk.StringVar] = {}
        for key in T.STYLE_KEYS:
            if key == "use_icons":
                continue
            value = self.pack["style"].get(key)
            self.style_vars[key] = tk.StringVar(value="" if value is None else str(value))

        self.color_entries: dict[str, tk.Entry] = {}
        self.color_swatches: dict[str, tk.Button] = {}
        self.style_entries: dict[str, tk.Entry] = {}
        self.form_body: tk.Frame | None = None
        self.icons_check: tk.Checkbutton | None = None

        self._build_menu()
        self._build_layout()
        self._build_form()
        self._wire_vars()
        self.refresh()

    # -- construction ----------------------------------------------------
    def _build_menu(self) -> None:
        menubar = tk.Menu(self)

        file_menu = tk.Menu(menubar, tearoff=0)
        file_menu.add_command(label="New theme", accelerator="Ctrl+N", command=self.action_new)
        file_menu.add_command(label="Open pack...", accelerator="Ctrl+O", command=self.action_open)
        file_menu.add_command(label="Reload themes", command=self.action_reload)
        file_menu.add_separator()
        file_menu.add_command(label="Save", accelerator="Ctrl+S", command=self.action_save)
        file_menu.add_command(label="Save as...", accelerator="Ctrl+Shift+S", command=self.action_save_as)
        file_menu.add_separator()
        file_menu.add_command(label="Install into my themes folder", command=self.action_install)
        file_menu.add_command(label="Make a shareable bundle (.zip)...", command=self.action_bundle)
        file_menu.add_separator()
        file_menu.add_command(label="Copy JSON", accelerator="Ctrl+J", command=self.action_copy_json)
        file_menu.add_separator()
        file_menu.add_command(label="Save as a draft...", command=self.action_save_draft)
        file_menu.add_command(label="Open a draft...", command=self.action_open_draft)
        file_menu.add_command(label="Quit", accelerator="Ctrl+Q", command=self.destroy)
        menubar.add_cascade(label="File", menu=file_menu)

        edit_menu = tk.Menu(menubar, tearoff=0)
        edit_menu.add_command(label="Undo", accelerator="Ctrl+Z", command=self.undo)
        edit_menu.add_command(label="Redo", accelerator="Ctrl+Y", command=self.redo)
        edit_menu.add_separator()
        edit_menu.add_command(label="Reset this palette to the base theme", command=self.action_reset_all)
        generator_menu = tk.Menu(edit_menu, tearoff=0)
        for key, label, help_text in GENERATORS:
            generator_menu.add_command(
                label=f"{label} - {help_text}", command=lambda k=key: self.apply_generator(k)
            )
        edit_menu.add_cascade(label="Generators", menu=generator_menu)
        edit_menu.add_separator()
        edit_menu.add_command(label="Pick accent colour...", command=self.action_pick_accent)
        edit_menu.add_command(label="Import colours from a list...", command=self.action_import_colors)
        menubar.add_cascade(label="Edit", menu=edit_menu)

        view_menu = tk.Menu(menubar, tearoff=0)
        view_menu.add_radiobutton(label="Simple", variable=self.mode, value="simple", command=self.set_mode_from_var)
        view_menu.add_radiobutton(label="Everything", variable=self.mode, value="advanced", command=self.set_mode_from_var)
        view_menu.add_separator()
        view_menu.add_command(label="Terminal preview", command=self.action_terminal_preview)
        menubar.add_cascade(label="View", menu=view_menu)

        help_menu = tk.Menu(menubar, tearoff=0)
        help_menu.add_command(label="Quick guide", command=self.action_guide)
        help_menu.add_command(label="Where do packs live?", command=self.action_folders)
        menubar.add_cascade(label="Help", menu=help_menu)

        self.config(menu=menubar)
        self.bind_all("<Control-z>", lambda _e: self.undo())
        self.bind_all("<Control-y>", lambda _e: self.redo())
        self.bind_all("<Control-s>", lambda _e: self.action_save())
        self.bind_all("<Control-Shift-S>", lambda _e: self.action_save_as())
        self.bind_all("<Control-o>", lambda _e: self.action_open())
        self.bind_all("<Control-n>", lambda _e: self.action_new())
        self.bind_all("<Control-j>", lambda _e: self.action_copy_json())
        self.bind_all("<Control-q>", lambda _e: self.destroy())

    def _build_layout(self) -> None:
        self.columnconfigure(0, weight=3, minsize=430)
        self.columnconfigure(1, weight=4, minsize=520)
        self.rowconfigure(1, weight=1)

        toolbar = tk.Frame(self, background=EDITOR_COLORS["panel"])
        toolbar.grid(row=0, column=0, columnspan=2, sticky="ew")
        row1 = tk.Frame(toolbar, background=EDITOR_COLORS["panel"])
        row1.pack(fill="x", padx=6, pady=(6, 2))
        for label, command in (
            ("New", self.action_new),
            ("Open", self.action_open),
            ("Save", self.action_save),
            ("Save as", self.action_save_as),
            ("Install", self.action_install),
            ("Bundle", self.action_bundle),
            ("Copy JSON", self.action_copy_json),
        ):
            self._chrome_button(row1, label, command).pack(side="left", padx=2)
        tk.Frame(row1, background=EDITOR_COLORS["panel"], width=16).pack(side="left")

        row2 = tk.Frame(toolbar, background=EDITOR_COLORS["panel"])
        row2.pack(fill="x", padx=6, pady=(2, 6))
        for label, value in (("Simple", "simple"), ("Everything", "advanced")):
            tk.Radiobutton(
                row2,
                text=label,
                value=value,
                variable=self.mode,
                command=self.set_mode_from_var,
                background=EDITOR_COLORS["panel"],
                foreground=EDITOR_COLORS["text"],
                activebackground=EDITOR_COLORS["panel"],
                activeforeground=EDITOR_COLORS["accent"],
                selectcolor=EDITOR_COLORS["sunken"],
                highlightthickness=0,
                bd=0,
            ).pack(side="left", padx=(0, 8))
        self._chrome_button(row2, "Undo", self.undo).pack(side="left", padx=2)
        self._chrome_button(row2, "Redo", self.redo).pack(side="left", padx=2)

        tk.Label(
            row2,
            text="Generators:",
            background=EDITOR_COLORS["panel"],
            foreground=EDITOR_COLORS["muted"],
        ).pack(side="left", padx=(14, 4))
        short_labels = {
            "readable": "Fix readability",
            "aaa": "High contrast",
            "invert": "Invert",
            "grayscale": "No colour",
            "random-dark": "Random dark",
            "random-light": "Random light",
            "from-accent": "From accent",
        }
        for key, label, help_text in GENERATORS:
            self._chrome_button(
                row2, short_labels.get(key, label), lambda k=key: self.apply_generator(k), tooltip=help_text
            ).pack(side="left", padx=2)

        row3 = tk.Frame(toolbar, background=EDITOR_COLORS["panel"])
        row3.pack(fill="x", padx=6, pady=(0, 6))
        tk.Label(
            row3,
            text="Start from a preset or installed pack:",
            background=EDITOR_COLORS["panel"],
            foreground=EDITOR_COLORS["muted"],
        ).pack(side="left", padx=(0, 4))
        self.preset_box = ttk.Combobox(row3, textvariable=self.preset_name, width=28, state="readonly")
        self.preset_box.pack(side="left", padx=(0, 4))
        self._chrome_button(row3, "Take its colours", lambda: self.apply_preset(self.preset_name.get())).pack(
            side="left", padx=2
        )

        self.form = _ScrollFrame(self)
        self.form.grid(row=1, column=0, sticky="nsew", padx=(6, 3), pady=6)

        right = tk.Frame(self, background=EDITOR_COLORS["bg"])
        right.grid(row=1, column=1, sticky="nsew", padx=(3, 6), pady=6)
        right.rowconfigure(0, weight=1)
        right.columnconfigure(0, weight=1)
        self.notebook = ttk.Notebook(right)
        self.notebook.grid(row=0, column=0, sticky="nsew")

        preview_page = tk.Frame(self.notebook, background=EDITOR_COLORS["bg"])
        checks_page = tk.Frame(self.notebook, background=EDITOR_COLORS["bg"])
        json_page = tk.Frame(self.notebook, background=EDITOR_COLORS["bg"])
        self.notebook.add(preview_page, text="Preview")
        self.notebook.add(checks_page, text="Checks")
        self.notebook.add(json_page, text="JSON")

        self._build_preview(preview_page)
        self._build_checks(checks_page)
        self._build_json(json_page)

        status_bar = tk.Frame(self, background=EDITOR_COLORS["panel"])
        status_bar.grid(row=2, column=0, columnspan=2, sticky="ew")
        tk.Label(
            status_bar,
            textvariable=self.status,
            anchor="w",
            background=EDITOR_COLORS["panel"],
            foreground=EDITOR_COLORS["text"],
        ).pack(side="left", padx=8, pady=4)
        self.status_right = tk.StringVar(value="")
        tk.Label(
            status_bar,
            textvariable=self.status_right,
            anchor="e",
            background=EDITOR_COLORS["panel"],
            foreground=EDITOR_COLORS["muted"],
        ).pack(side="right", padx=8, pady=4)

    def _chrome_button(self, parent: Any, label: str, command: Callable[[], Any], *, tooltip: str = "") -> tk.Button:
        button = tk.Button(
            parent,
            text=label,
            command=command,
            background=EDITOR_COLORS["sunken"],
            foreground=EDITOR_COLORS["text"],
            activebackground=EDITOR_COLORS["accent"],
            activeforeground=EDITOR_COLORS["accent_text"],
            relief="flat",
            bd=0,
            padx=8,
            pady=2,
            highlightthickness=0,
        )
        if tooltip:
            _Tooltip(button, tooltip)
        return button

    def _build_preview(self, parent: Any) -> None:
        """The mock app window. Plain tk widgets everywhere except the table."""
        controls = tk.Frame(parent, background=EDITOR_COLORS["bg"])
        controls.pack(fill="x", padx=10, pady=(10, 0))
        tk.Label(
            controls, text="Vision:", background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["muted"],
        ).pack(side="left")
        self.vision_var = tk.StringVar(value="normal")
        vision = ttk.Combobox(
            controls, textvariable=self.vision_var, values=list(VISION_MODES),
            state="readonly", width=14,
        )
        vision.pack(side="left", padx=(6, 0))
        vision.bind("<<ComboboxSelected>>", lambda _event: self.refresh())
        tk.Label(
            controls,
            text="  colour-blind simulation of the preview only - the pack keeps its colours",
            background=EDITOR_COLORS["bg"], foreground=EDITOR_COLORS["muted"],
        ).pack(side="left")

        window = tk.Frame(parent, background=EDITOR_COLORS["sunken"], highlightthickness=1,
                          highlightbackground=EDITOR_COLORS["border"])
        window.pack(fill="both", expand=True, padx=10, pady=10)

        self.pv_menubar = tk.Frame(window)
        self.pv_menubar.pack(fill="x")
        self.pv_menu_labels = [
            tk.Label(self.pv_menubar, text=text, padx=8, pady=3) for text in ("File", "Edit", "View", "Theme", "Help")
        ]
        for label in self.pv_menu_labels:
            label.pack(side="left")

        self.pv_title = tk.Label(window, text=f"  {T.APP_NAME} V{getattr(T, 'APP_VERSION', '0.10.1')}   -   Local ISOs", anchor="w")
        self.pv_title.pack(fill="x")

        self.pv_toolbar = tk.Frame(window)

        self.pv_toolbar.pack(fill="x", pady=(6, 0))
        self.pv_buttons: dict[str, tk.Button] = {}
        specs = (
            ("download", "Download", False),
            ("scan", "Scan folder", False),
            ("stop", "Stop", True),
            ("delete", "Delete", False),
            ("settings", "Settings", False),
        )
        for key, text, disabled in specs:
            button = tk.Button(self.pv_toolbar, text=text, padx=10, pady=2, bd=0, relief="flat", highlightthickness=0)
            button.pack(side="left", padx=3)
            if disabled:
                button.configure(state="disabled")
            self.pv_buttons[key] = button
        tk.Label(self.pv_toolbar, text="  Search ISOs...", anchor="w").pack(side="left", fill="x", expand=True, padx=8)

        self.pv_tabs = tk.Frame(window)
        self.pv_tabs.pack(fill="x", pady=(8, 0))
        self.pv_tab_labels: list[tk.Label] = []
        for index, text in enumerate(("Local ISO", "Download", "Settings", "Logs")):
            label = tk.Label(self.pv_tabs, text=f"  {text}  ", padx=6, pady=4)
            label.pack(side="left", padx=(0, 1))
            self.pv_tab_labels.append(label)

        body = tk.Frame(window)
        body.pack(fill="both", expand=True, pady=(0, 0))

        self.pv_list = tk.Listbox(
            body, width=24, height=8, bd=0, highlightthickness=0, relief="flat", activestyle="none"
        )
        for name, _size, _state in SAMPLE_ISOS:
            self.pv_list.insert("end", f" {name}")
        self.pv_list.pack(side="left", fill="both")
        try:
            self.pv_list.selection_set(0)
        except Exception:  # pragma: no cover
            pass

        table_holder = tk.Frame(body)
        table_holder.pack(side="left", fill="both", expand=True, padx=(1, 0))
        self.pv_tree = ttk.Treeview(
            table_holder,
            style="Preview.Treeview",
            columns=("size", "state"),
            show="tree headings",
            height=8,
        )
        self.pv_tree.heading("#0", text="Name")
        self.pv_tree.heading("size", text="Size")
        self.pv_tree.heading("state", text="Status")
        self.pv_tree.column("#0", width=190, stretch=True)
        self.pv_tree.column("size", width=80, anchor="w")
        self.pv_tree.column("state", width=110, anchor="w")
        for index, (name, size, state) in enumerate(SAMPLE_ISOS):
            self.pv_tree.insert("", "end", iid=f"row{index}", text=name, values=(size, state))
        self.pv_tree.pack(fill="both", expand=True)
        try:
            self.pv_tree.selection_set("row1")
        except Exception:  # pragma: no cover
            pass

        self.pv_status = tk.Frame(window)
        self.pv_status.pack(fill="x")
        self.pv_status_text = tk.Label(
            self.pv_status, text="  4 ISOs  -  11.7 GB  -  Ready", anchor="w", padx=6, pady=3
        )
        self.pv_status_text.pack(side="left")

        self.pv_font_note = tk.Label(parent, text="", anchor="w")
        self.pv_font_note.pack(fill="x", padx=12, pady=(0, 6))

    def _build_checks(self, parent: Any) -> None:
        self.checks = tk.Text(
            parent,
            wrap="word",
            bd=0,
            highlightthickness=0,
            background=EDITOR_COLORS["sunken"],
            foreground=EDITOR_COLORS["text"],
            insertbackground=EDITOR_COLORS["text"],
            padx=10,
            pady=8,
            height=10,
        )
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=self.checks.yview)
        self.checks.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.checks.pack(fill="both", expand=True)
        self.checks.tag_configure("head", foreground=EDITOR_COLORS["text"])
        self.checks.tag_configure("ok", foreground=EDITOR_COLORS["ok"])
        self.checks.tag_configure("warn", foreground=EDITOR_COLORS["warn"])
        self.checks.tag_configure("err", foreground=EDITOR_COLORS["err"])
        self.checks.tag_configure("dim", foreground=EDITOR_COLORS["muted"])
        self.checks.configure(state="disabled")

    def _build_json(self, parent: Any) -> None:
        self.json_view = tk.Text(
            parent,
            wrap="none",
            bd=0,
            highlightthickness=0,
            background=EDITOR_COLORS["sunken"],
            foreground=EDITOR_COLORS["text"],
            insertbackground=EDITOR_COLORS["text"],
            padx=10,
            pady=8,
        )
        scrollbar = ttk.Scrollbar(parent, orient="vertical", command=self.json_view.yview)
        self.json_view.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side="right", fill="y")
        self.json_view.pack(fill="both", expand=True)
        self.json_view.insert("1.0", "")
        self.json_view.configure(state="disabled")

    def _build_form(self) -> None:
        if self.form_body is not None:
            self.form_body.destroy()
        self.form_body = tk.Frame(self.form.body, background=EDITOR_COLORS["bg"])
        self.form_body.pack(fill="both", expand=True)
        self.color_entries = {}
        self.color_swatches = {}
        self.style_entries = {}

        self._section("Theme")
        self._meta_rows()

        base_row = self._row_frame()
        self._label(base_row, "Base theme")
        combo = ttk.Combobox(
            base_row, textvariable=self.base_var, values=list(T.BUILTIN_ORDER), state="readonly", width=18
        )
        combo.pack(side="left", padx=(0, 8))
        combo.bind("<<ComboboxSelected>>", lambda _e: self.on_base_changed())
        self._hint(base_row, "what you do not set is inherited from here")

        if self.mode.get() == "simple":
            self._section("Colours (the essentials)")
            for key in SIMPLE_KEYS:
                self._color_row(key)
            self._hint_row(
                "accent_text, selection_fg and tree_heading_fg are worked out for you. "
                "Switch to 'Everything' to set them by hand."
            )
            if "accent_text" in self.color_entries:
                pass
        else:
            for group, keys in COLOR_GROUPS:
                self._section(f"Colours - {group}")
                for key in keys:
                    self._color_row(key)
            self._section("Style switches")
            self._style_rows()

        self._section("Actions")
        actions = self._row_frame()
        self._chrome_button(actions, "Make it readable", lambda: self.apply_generator("readable")).pack(side="left", padx=2)
        self._chrome_button(actions, "Pick accent...", self.action_pick_accent).pack(side="left", padx=2)
        self._chrome_button(actions, "Reset all to base", self.action_reset_all).pack(side="left", padx=2)
        reset_panel = self._row_frame()
        self._chrome_button(
            reset_panel,
            "Reset the derived colours to base",
            lambda: [self.reset_color(k) for k in ("accent_text", "selection_fg", "tree_heading_fg")],
        ).pack(side="left", padx=2)
        self._chrome_button(reset_panel, "Terminal preview", self.action_terminal_preview).pack(side="left", padx=2)

    def _section(self, title: str) -> None:
        frame = tk.Frame(self.form_body, background=EDITOR_COLORS["bg"])
        frame.pack(fill="x", pady=(12, 4))
        tk.Label(
            frame,
            text=title.upper(),
            background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["accent"],
            anchor="w",
        ).pack(side="left")
        tk.Frame(frame, background=EDITOR_COLORS["border"], height=1).pack(
            side="left", fill="x", expand=True, padx=(8, 0), pady=8
        )

    def _row_frame(self) -> tk.Frame:
        frame = tk.Frame(self.form_body, background=EDITOR_COLORS["bg"])
        frame.pack(fill="x", pady=2)
        return frame

    def _label(self, parent: Any, text: str, *, width: int = 17) -> tk.Label:
        label = tk.Label(
            parent,
            text=text,
            width=width,
            anchor="w",
            background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["text"],
        )
        label.pack(side="left")
        return label

    def _hint(self, parent: Any, text: str) -> None:
        tk.Label(
            parent,
            text=text,
            anchor="w",
            background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["muted"],
        ).pack(side="left", padx=(6, 0))

    def _hint_row(self, text: str) -> None:
        tk.Label(
            self.form_body,
            text=text,
            anchor="w",
            justify="left",
            wraplength=380,
            background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["muted"],
        ).pack(fill="x", pady=(4, 0))

    def _meta_rows(self) -> None:
        for key, label, help_text in META_FIELDS:
            if key == "font_family":
                continue
            row = self._row_frame()
            self._label(row, label)
            entry = tk.Entry(
                row,
                textvariable=self.meta_vars[key],
                width=26,
                background=EDITOR_COLORS["sunken"],
                foreground=EDITOR_COLORS["text"],
                insertbackground=EDITOR_COLORS["text"],
                relief="flat",
                highlightthickness=1,
                highlightbackground=EDITOR_COLORS["border"],
            )
            entry.pack(side="left", padx=(0, 6))
            if key == "id":
                self._chrome_button(row, "from name", lambda: self.meta_vars["id"].set(slugify(self.meta_vars["name"].get()))).pack(side="left")
            else:
                self._hint(row, help_text[:44])

        row = self._row_frame()
        self._label(row, "Font")
        fonts = sorted(set(tkfont.families())) if tkfont is not None else []
        self.font_box = ttk.Combobox(row, textvariable=self.meta_vars["font_family"], values=fonts, width=26)
        self.font_box.pack(side="left", padx=(0, 6))
        self._hint(row, "empty = the base theme's font")

        self.review_label = tk.Label(
            self.form_body,
            text="",
            anchor="w",
            justify="left",
            wraplength=400,
            background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["warn"],
        )
        self.review_label.pack(fill="x", pady=(4, 0))

    def _color_row(self, key: str) -> None:
        row = self._row_frame()
        self._label(row, COLOR_LABELS.get(key, key))
        swatch = tk.Button(
            row,
            text="",
            width=3,
            relief="flat",
            bd=1,
            highlightthickness=1,
            highlightbackground=EDITOR_COLORS["border"],
            background=self.color_vars[key].get() or "#000000",
            command=lambda k=key: self.action_pick_color(k),
        )
        swatch.pack(side="left", padx=(0, 4))
        entry = tk.Entry(
            row,
            textvariable=self.color_vars[key],
            width=10,
            background=EDITOR_COLORS["sunken"],
            foreground=EDITOR_COLORS["text"],
            insertbackground=EDITOR_COLORS["text"],
            relief="flat",
            highlightthickness=1,
            highlightbackground=EDITOR_COLORS["border"],
        )
        entry.pack(side="left", padx=(0, 4))
        self._chrome_button(row, "reset", lambda k=key: self.reset_color(k)).pack(side="left", padx=(0, 6))
        self._hint(row, COLOR_HELP.get(key, ""))
        self.color_entries[key] = entry
        self.color_swatches[key] = swatch

    def _style_rows(self) -> None:
        for key in T.STYLE_KEYS:
            row = self._row_frame()
            self._label(row, STYLE_LABELS.get(key, key))
            if key == "use_icons":
                self.icons_check = tk.Checkbutton(
                    row,
                    text="draw the toolbar icons",
                    variable=self.use_icons,
                    command=self.on_use_icons,
                    background=EDITOR_COLORS["bg"],
                    foreground=EDITOR_COLORS["text"],
                    activebackground=EDITOR_COLORS["bg"],
                    activeforeground=EDITOR_COLORS["text"],
                    selectcolor=EDITOR_COLORS["sunken"],
                    highlightthickness=0,
                    bd=0,
                )
                self.icons_check.pack(side="left", padx=(0, 6))
                self.style_entries[key] = self.icons_check
            else:
                values = list(T.TTK_THEMES) if key == "ttk_theme" else (list(RELIEFS) if key.endswith("relief") else [])
                if values:
                    widget: Any = ttk.Combobox(
                        row, textvariable=self.style_vars[key], values=values, width=12
                    )
                else:
                    widget = tk.Entry(
                        row,
                        textvariable=self.style_vars[key],
                        width=12,
                        background=EDITOR_COLORS["sunken"],
                        foreground=EDITOR_COLORS["text"],
                        insertbackground=EDITOR_COLORS["text"],
                        relief="flat",
                        highlightthickness=1,
                        highlightbackground=EDITOR_COLORS["border"],
                    )
                widget.pack(side="left", padx=(0, 6))
                self.style_entries[key] = widget
                self._chrome_button(row, "clear", lambda k=key: self.style_vars[k].set("")).pack(side="left", padx=(0, 6))
            self._hint(row, STYLE_HELP.get(key, ""))

    # -- wiring ----------------------------------------------------------
    def _wire_vars(self) -> None:
        for key, var in self.meta_vars.items():
            var.trace_add("write", lambda *_a: self._touch())
        self.base_var.trace_add("write", lambda *_a: self._touch())
        for key, var in self.color_vars.items():
            var.trace_add("write", lambda *_a, k=key: self.on_color_typed(k))
        for key, var in self.style_vars.items():
            var.trace_add("write", lambda *_a: self._touch())

    def _sync_vars(self) -> None:
        """Push the pack into the widgets without triggering edits."""
        self._loading = True
        try:
            self.meta_vars["name"].set(self.pack["name"])
            self.meta_vars["id"].set(self.pack["id"])
            self.meta_vars["author"].set(self.pack["author"])
            self.meta_vars["version"].set(self.pack["version"])
            self.meta_vars["description"].set(self.pack["description"])
            self.meta_vars["license"].set(self.pack["license"])
            self.meta_vars["homepage"].set(self.pack["homepage"])
            self.meta_vars["font_family"].set(self.pack["font_family"])
            self.base_var.set(self.pack["base"])
            for key in T.COLOR_KEYS:
                self.color_vars[key].set(self.pack["colors"][key])
            for key, var in self.style_vars.items():
                value = self.pack["style"].get(key)
                var.set("" if value is None else str(value))
            self.use_icons.set(bool(self.pack["style"].get("use_icons")))
            for key, entry in self.color_entries.items():
                entry.configure(highlightbackground=EDITOR_COLORS["border"])
        finally:
            self._loading = False
        self._paint_swatches()

    def _paint_swatches(self) -> None:
        for key, swatch in self.color_swatches.items():
            value = self.color_vars[key].get().strip()
            rgb = T.to_rgb(value)
            swatch.configure(background=value if rgb else EDITOR_COLORS["err"])

    def _touch(self) -> None:
        if self._loading:
            return
        self.pack["name"] = self.meta_vars["name"].get()
        self.pack["id"] = self.meta_vars["id"].get().strip().lower()
        self.pack["author"] = self.meta_vars["author"].get()
        self.pack["version"] = self.meta_vars["version"].get()
        self.pack["description"] = self.meta_vars["description"].get()
        self.pack["license"] = self.meta_vars["license"].get()
        self.pack["homepage"] = self.meta_vars["homepage"].get()
        self.pack["font_family"] = self.meta_vars["font_family"].get().strip()
        self.pack["base"] = self.base_var.get()
        writer = getattr(self, "review_label", None)
        if writer is not None:
            problems = [p for p in (name_problem(self.pack["name"]), id_problem(self.pack["id"], taken=installed_ids())) if p]
            writer.configure(text=problems[0] if problems else "")
        self.schedule_refresh()

    def on_base_changed(self) -> None:
        if self._loading:
            return
        self.push_undo()
        colors = base_palette(self.base_var.get())
        self.pack["base"] = self.base_var.get()
        self.pack["colors"] = colors
        self.pack["style"] = base_style(self.base_var.get())
        if not self.meta_vars["font_family"].get().strip():
            self.pack["font_family"] = ""
        self._sync_vars()
        self.refresh()

    def on_use_icons(self) -> None:
        if self._loading:
            return
        self.push_undo()
        if self.use_icons.get():
            self.pack["style"]["use_icons"] = True
        else:
            self.pack["style"].pop("use_icons", None)
        self.refresh()

    def on_color_typed(self, key: str) -> None:
        if self._loading:
            return
        value = self.color_vars[key].get().strip()
        entry = self.color_entries.get(key)
        if not value or not T.is_color(value):
            if entry is not None:
                entry.configure(highlightbackground=EDITOR_COLORS["err"], highlightcolor=EDITOR_COLORS["err"])
            self.schedule_refresh()
            return
        if entry is not None:
            entry.configure(highlightbackground=EDITOR_COLORS["border"])
        self.pack["colors"][key] = value
        self._paint_swatches()
        self.schedule_refresh()

    # -- refresh ---------------------------------------------------------
    def schedule_refresh(self) -> None:
        if self._refresh_job is not None:
            try:
                self.after_cancel(self._refresh_job)
            except Exception:  # pragma: no cover
                pass
        self._refresh_job = self.after(90, self.refresh)

    def refresh(self) -> None:
        self._refresh_job = None
        self._style_overrides_into_pack()
        state = preview_state(self.pack)
        mode = self.vision_var.get() if hasattr(self, "vision_var") else "normal"
        self._paint_preview(simulate_state(state, mode))
        result = review(self.pack, taken_ids=installed_ids())
        self._update_checks(result)
        self._update_json()
        self._update_status(result)
        self.update_idletasks()

    def _style_overrides_into_pack(self) -> None:
        if self._loading:
            return
        values: dict[str, Any] = {"use_icons": self.use_icons.get()}
        for key, var in self.style_vars.items():
            text = var.get().strip()
            if text == "":
                values[key] = None if key in STYLE_COLORS else ""
            else:
                values[key] = text
        merged = base_style(self.pack["base"])
        merged.update(style_overrides(values))
        self.pack["style"] = merged
        self.pack["name"] = self.meta_vars["name"].get()
        self.pack["id"] = self.meta_vars["id"].get().strip().lower()
        self.pack["author"] = self.meta_vars["author"].get()
        self.pack["version"] = self.meta_vars["version"].get()
        self.pack["description"] = self.meta_vars["description"].get()
        self.pack["license"] = self.meta_vars["license"].get()
        self.pack["homepage"] = self.meta_vars["homepage"].get()
        self.pack["font_family"] = self.meta_vars["font_family"].get().strip()

    def _paint_preview(self, state: Mapping[str, Any]) -> None:
        c = {key: str(state.get(key) or "#000000") for key in T.COLOR_KEYS}
        panel = c["panel"]
        border = c["border"]
        disabled_bg = str(state.get("disabled_bg") or panel)
        disabled_fg = str(state.get("disabled_fg") or c["muted"])
        pressed = str(state.get("pressed_bg") or c["accent_active"])
        font_family = str(state.get("font_family") or "TkDefaultFont")
        base_font = self._safe_font(font_family)

        self.pv_title.configure(background=c["bg"], foreground=c["text"], font=(base_font, 11, "bold"))
        self.pv_menubar.configure(background=c["panel"])
        for label in self.pv_menu_labels:
            label.configure(background=c["panel"], foreground=c["text"], font=(base_font, 9))
        self.pv_toolbar.configure(background=panel, highlightthickness=1, highlightbackground=border)
        self.pv_title.configure(font=(base_font, 11, "bold"))

        self.pv_buttons["download"].configure(
            background=c["accent"], foreground=c["accent_text"], activebackground=pressed,
            activeforeground=c["accent_text"], font=(base_font, 9),
        )
        self.pv_buttons["scan"].configure(
            background=panel, foreground=c["text"], activebackground=c["accent_active"],
            activeforeground=c["text"], font=(base_font, 9), highlightbackground=border,
        )
        self.pv_buttons["stop"].configure(
            background=disabled_bg, foreground=disabled_fg, disabledforeground=disabled_fg,
            font=(base_font, 9),
        )
        self.pv_buttons["delete"].configure(
            background=c["danger"], foreground=T.best_text_on(c["danger"]),
            activebackground=c["danger_active"], activeforeground=T.best_text_on(c["danger_active"]),
            font=(base_font, 9),
        )
        self.pv_buttons["settings"].configure(
            background=panel, foreground=c["text"], activebackground=c["tab_bg"],
            activeforeground=c["text"], font=(base_font, 9),
        )
        for key, widget in self.pv_buttons.items():
            widget.configure(relief=str(state.get("button_relief") or "flat"))

        self.pv_tabs.configure(background=c["bg"], highlightthickness=1, highlightbackground=border)
        for index, label in enumerate(self.pv_tab_labels):
            if index == 0:
                label.configure(background=c["tab_selected_bg"], foreground=c["text"])
            else:
                label.configure(background=c["tab_bg"], foreground=c["muted"])
            label.configure(font=(base_font, 9), relief=str(state.get("tab_relief") or "flat"))

        self.pv_list.configure(
            background=c["tree_bg"], foreground=c["text"], selectbackground=c["selection"],
            selectforeground=c["selection_fg"], font=(base_font, 9),
            highlightthickness=1, highlightbackground=border,
        )
        style = ttk.Style(self)
        try:
            style.configure(
                "Preview.Treeview",
                background=c["tree_bg"],
                fieldbackground=c["tree_bg"],
                foreground=c["text"],
                bordercolor=border,
                lightcolor=c["tree_bg"],
                darkcolor=c["tree_bg"],
                rowheight=max(18, 15 + self._font_size(base_font)),
                font=(base_font, 9),
            )
            style.map(
                "Preview.Treeview",
                background=[("selected", c["selection"])],
                foreground=[("selected", c["selection_fg"])],
            )
            style.configure(
                "Preview.Treeview.Heading",
                background=c["tree_heading_bg"],
                foreground=c["tree_heading_fg"],
                relief=str(state.get("tree_heading_relief") or "flat"),
                font=(base_font, 9, "bold"),
            )
        except Exception:  # pragma: no cover - some ttk engines refuse fieldbackground
            pass

        self.pv_status.configure(background=c["panel"], highlightthickness=1, highlightbackground=border)
        self.pv_status_text.configure(background=c["panel"], foreground=c["muted"], font=(base_font, 9))
        self.pv_list.configure(height=8)
        self.pv_font_note.configure(
            text=(
                f"font: {font_family or 'base theme'}    base: {self.pack['base']}    "
                f"ttk engine requested: {state.get('ttk_theme') or 'auto'}    "
                f"relief: {state.get('button_relief') or 'flat'}"
            ),
            background=EDITOR_COLORS["bg"],
            foreground=EDITOR_COLORS["muted"],
        )

    def _safe_font(self, family: str) -> str:
        if not family:
            return "TkDefaultFont"
        try:
            if tkfont is not None and family in set(tkfont.families()):
                return family
        except Exception:  # pragma: no cover
            pass
        return "TkDefaultFont"

    def _font_size(self, family: str) -> int:
        try:
            if tkfont is not None:
                return abs(int(tkfont.Font(family=family, size=9).actual("size")))
        except Exception:  # pragma: no cover
            pass
        return 9

    def _update_checks(self, result: Mapping[str, Any]) -> None:
        theme = result.get("theme")
        lines: list[tuple[str, str]] = []
        if theme is not None:
            lines.append(("head", f"{theme.name}  [{theme.id}]  base {theme.base}\n"))
        else:
            lines.append(("err", "This pack cannot be previewed yet - see the problems below.\n"))
        lines.append(("head", "\nReadability (WCAG)\n"))
        for label, ratio_text, ratio, minimum, passed in result.get("rows", []):
            if ratio is None:
                lines.append(("dim", f"   n/a  {label:<26} {'-':>9}   min {minimum:.1f}:1\n"))
            elif passed:
                lines.append(("ok", f"    ok  {label:<26} {ratio_text:>9}   min {minimum:.1f}:1\n"))
            else:
                lines.append(("warn", f"   low  {label:<26} {ratio_text:>9}   min {minimum:.1f}:1\n"))
        lines.append(("head", "\nProblems\n"))
        errors = result.get("errors", [])
        warnings = result.get("warnings", [])
        if not errors and not warnings:
            lines.append(("ok", "   none - this pack will load\n"))
        for message in errors:
            lines.append(("err", f"   error  {message}\n"))
        for message in warnings:
            lines.append(("warn", f"   warn   {message}\n"))
        if result.get("low"):
            lines.append(
                ("dim", "\nTip: 'Fix readability' moves the text colours until every pair passes.\n")
            )
        if self.path is None:
            lines.append(("dim", "\nNot saved yet - use File > Save or Install.\n"))
        self.checks.configure(state="normal")
        self.checks.delete("1.0", "end")
        for tag, text in lines:
            self.checks.insert("end", text, tag)
        self.checks.configure(state="disabled")

    def _update_json(self) -> None:
        text = pack_json(self.pack)
        if self.json_view.get("1.0", "end-1c") == text:
            return
        self.json_view.configure(state="normal")
        self.json_view.delete("1.0", "end")
        self.json_view.insert("1.0", text)
        self.json_view.configure(state="disabled")

    def _update_status(self, result: Mapping[str, Any]) -> None:
        mode = "Simple" if self.mode.get() == "simple" else "Everything"
        where = str(self.path) if self.path else "unsaved"
        state = "modified" if self.is_dirty() else "saved"
        problems = len(result.get("errors", []))
        lows = len(result.get("low", []))
        self.status.set(
            f"{mode} mode   |   {self.pack['id'] or 'no id'}   |   {where}"
            f"   |   {state}   |   {problems} problem(s), {lows} low-contrast pair(s)"
        )
        self.status_right.set("Ctrl+S save  -  Ctrl+Z undo  -  Ctrl+J copy JSON")

    # -- editing API -----------------------------------------------------
    def is_dirty(self) -> bool:
        current = pack_json(self.pack)
        if not self._saved_json:
            return current != pack_json(build_pack())
        return current != self._saved_json

    def push_undo(self) -> None:
        if self._loading:
            return
        self._undo.append(copy.deepcopy(self.pack))
        del self._undo[:-60]
        self._redo.clear()

    def undo(self) -> bool:
        if not self._undo:
            return False
        self._redo.append(copy.deepcopy(self.pack))
        self.pack = self._undo.pop()
        self._sync_vars()
        self.refresh()
        return True

    def redo(self) -> bool:
        if not self._redo:
            return False
        self._undo.append(copy.deepcopy(self.pack))
        self.pack = self._redo.pop()
        self._sync_vars()
        self.refresh()
        return True

    def load_pack(self, pack: Mapping[str, Any], path: str | Path | None = None) -> None:
        self.pack = normalize_pack(pack)
        self.path = Path(path) if path else None
        self._undo.clear()
        self._redo.clear()
        self._saved_json = pack_json(self.pack)
        self._sync_vars()
        self._build_form()
        self.refresh()

    def new_pack(self, base: str = T.FALLBACK_THEME_ID, name: str = "My Theme", author: str = "") -> None:
        pack = build_pack(name=name, author=author, base=base)
        self.load_pack(pack, None)
        self._saved_json = ""

    def set_color(self, key: str, value: str, *, record: bool = True) -> None:
        """Programmatic colour change (same path the colour picker uses)."""
        if key not in T.COLOR_KEYS:
            raise T.ThemeError(f"unknown colour {key!r}")
        if not T.is_color(value):
            raise T.ThemeError(f"{value!r} is not a colour")
        if record:
            self.push_undo()
        self.color_vars[key].set(value.strip())
        self.refresh()

    def reset_color(self, key: str) -> None:
        self.set_color(key, base_palette(self.pack["base"]).get(key, "#000000"))

    def action_reset_all(self) -> None:
        self.push_undo()
        self.pack["colors"] = base_palette(self.pack["base"])
        self.pack["style"] = base_style(self.pack["base"])
        self._sync_vars()
        self.refresh()

    def apply_generator(self, key: str, *, seed: Any = None) -> bool:
        self.push_undo()
        try:
            self.pack = apply_generator(self.pack, key, seed=seed)
        except T.ThemeError:
            self._undo.pop()
            return False
        self._sync_vars()
        self.refresh()
        return True

    def apply_preset(self, name: str) -> bool:
        if not name:
            return False
        options = preset_options()
        colors = options.get(name)
        if not colors:
            return False
        self.push_undo()
        try:
            self.pack = apply_preset(self.pack, colors)
        except T.ThemeError:
            self._undo.pop()
            return False
        self._sync_vars()
        self.refresh()
        return True

    def set_mode(self, mode: str) -> None:
        self.mode.set("advanced" if mode == "advanced" else "simple")
        self._build_form()
        self._paint_swatches()
        self.refresh()

    def set_mode_from_var(self) -> None:
        self.set_mode(self.mode.get())

    def refresh_presets(self) -> None:
        names = sorted(preset_options())
        self.preset_box.configure(values=names)
        if names and not self.preset_name.get():
            self.preset_name.set(names[0])

    # -- file actions ----------------------------------------------------
    def save_to(self, path: str | Path) -> Path:
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        text = pack_json(self.pack)
        target.write_text(text, encoding="utf-8")
        self.path = target
        self._saved_json = text
        if target.suffix.lower() != ".ipmtheme.json" and target.name.lower().endswith(".json"):
            pass
        self.refresh()
        return target

    def open_path(self, path: str | Path) -> str:
        """Load a pack for editing; returns any validation complaint (or '')."""
        pack, problem = load_pack_data(Path(path))
        self.load_pack(pack, path)
        self.refresh_presets()
        return problem

    def install_to(self, dest_dir: str | Path | None = None) -> Path:
        source = self._materialise()
        target = T.install_pack(source, dest_dir if dest_dir is not None else T.ensure_user_theme_dir())
        self.refresh_presets()
        return target

    def bundle_to(self, dest: str | Path | None = None) -> Path:
        source = self._materialise()
        return T.bundle_pack(source, dest)

    def _materialise(self) -> Path:
        """Make sure the pack exists on disk (an unsaved theme goes to a temp file)."""
        if self.path is not None:
            self.save_to(self.path)
            return self.path
        folder = Path(tempfile.mkdtemp(prefix="ipm-theme-"))
        return self.save_to(folder / f"{self.pack['id'] or 'my-theme'}.ipmtheme.json")

    def checks_text(self) -> str:
        return self.checks.get("1.0", "end-1c")

    def json_text(self) -> str:
        return self.json_view.get("1.0", "end-1c")

    def terminal_text(self) -> str:
        return terminal_preview(self.pack)

    def status_line(self) -> str:
        return self.status.get()

    # -- dialogs ---------------------------------------------------------
    def action_new(self) -> None:
        if self.is_dirty() and not self._confirm("Discard the unsaved changes?"):
            return
        if self.test_mode:
            self.new_pack()
            return
        dialog = _NewPackDialog(self, self.pack["author"])
        if dialog.result is not None:
            name, base, author = dialog.result
            self.new_pack(base=base, name=name, author=author)

    def action_open(self) -> None:
        if self.test_mode:
            return
        path = filedialog.askopenfilename(
            title="Open a theme pack",
            filetypes=[("Theme packs", "*.ipmtheme.json"), ("JSON", "*.json"), ("Zip bundles", "*.zip"), ("All files", "*.*")],
        )
        if path:
            self.open_path(path)

    def action_reload(self) -> None:
        T.invalidate_registry()
        self.refresh_presets()
        self.refresh()

    def action_save(self) -> None:
        if self.path is None:
            self.action_save_as()
            return
        self._save_with_warnings(self.path)

    def action_save_as(self) -> None:
        if self.test_mode:
            return
        default_name = f"{self.pack['id'] or 'my-theme'}.ipmtheme.json"
        path = filedialog.asksaveasfilename(
            title="Save the theme pack",
            defaultextension=".ipmtheme.json",
            initialfile=default_name,
            filetypes=[("Theme packs", "*.ipmtheme.json"), ("JSON", "*.json")],
        )
        if path:
            self._save_with_warnings(path)

    def _save_with_warnings(self, path: str | Path) -> bool:
        result = review(self.pack, taken_ids=installed_ids())
        if result["errors"] and not self._confirm(
            "This pack still has problems:\n\n" + "\n".join(f"- {e}" for e in result["errors"][:6]) + "\n\nSave anyway?"
        ):
            return False
        self.save_to(path)
        return True

    def action_install(self) -> None:
        try:
            saved = self.install_to()
        except T.ThemeError as exc:
            self._error(f"Could not install: {exc}")
            return
        self._info(f"Installed into your themes folder:\n{saved}\n\nRestart the app, or use Theme > Reload themes.")

    def action_bundle(self) -> None:
        dest = None
        if not self.test_mode:
            dest = filedialog.asksaveasfilename(
                title="Save the shareable bundle",
                defaultextension=".zip",
                initialfile=f"{self.pack['id'] or 'my-theme'}.ipmtheme.zip",
                filetypes=[("Zip bundles", "*.zip")],
            )
            if not dest:
                return
        try:
            saved = self.bundle_to(dest)
        except T.ThemeError as exc:
            self._error(f"Could not bundle: {exc}")
            return
        size = saved.stat().st_size if saved.is_file() else 0
        self._info(f"Bundle written ({size:,} bytes):\n{saved}\n\nShare this one file - anyone can install it.")

    def action_copy_json(self) -> None:
        try:
            self.clipboard_clear()
            self.clipboard_append(pack_json(self.pack))
        except Exception as exc:  # pragma: no cover
            self._error(f"Clipboard refused the data: {exc}")

    def action_pick_color(self, key: str) -> None:
        if self.test_mode:
            return
        current = self.color_vars[key].get()
        rgb, value = colorchooser.askcolor(color=current, title=f"Pick {COLOR_LABELS.get(key, key)}")
        if value:
            self.set_color(key, value)

    def drafts_dir(self) -> Path:
        """``~/.ipm/themes/_drafts`` - work in progress, never loaded by the app."""
        folder = T.user_theme_dir() / "_drafts"
        folder.mkdir(parents=True, exist_ok=True)
        return folder

    def action_save_draft(self) -> None:
        if self.test_mode:
            return
        name = simpledialog.askstring(
            "Save a draft", "Name for this draft:", initialvalue=self.pack.get("name") or "My Theme", parent=self,
        )
        if not name:
            return
        slug = slugify(name) or "draft"
        target = self.drafts_dir() / f"{slug}.ipmtheme.json"
        try:
            target.write_text(pack_json(self.pack), encoding="utf-8")
        except OSError as exc:
            messagebox.showerror("Save a draft", str(exc), parent=self)
            return
        self.set_status(f"Draft saved: {target}")

    def action_open_draft(self) -> None:
        if self.test_mode:
            return
        folder = self.drafts_dir()
        drafts = sorted(folder.glob("*.ipmtheme.json"))
        if not drafts:
            messagebox.showinfo("Open a draft", f"No drafts yet.\n\nThey are saved in {folder}", parent=self)
            return
        path = filedialog.askopenfilename(
            title="Open a draft", initialdir=str(folder),
            filetypes=[("Theme packs", "*.ipmtheme.json"), ("All files", "*.*")], parent=self,
        )
        if not path:
            return
        self.open_path(Path(path))

    def action_import_colors(self) -> None:
        if self.test_mode:
            return
        text = simpledialog.askstring(
            "Import colours",
            "Paste hex colours (a Coolors link, or a list like #1e1e2e #cdd6f4 #89b4fa):",
            parent=self,
        )
        if not text:
            return
        try:
            colors = palette_from_hex_list(text, preview_colors(self.pack))
        except T.ThemeError as exc:
            messagebox.showwarning("Import colours", str(exc), parent=self)
            return
        self.push_undo()
        self.pack["colors"] = colors
        self._sync_vars()
        self.refresh()
        self.set_status("Palette built from the pasted colours")

    def action_pick_accent(self) -> None:
        if self.test_mode:
            return
        rgb, value = colorchooser.askcolor(color=self.color_vars["accent"].get(), title="Pick the accent colour")
        if not value:
            return
        self.push_undo()
        self.color_vars["accent"].set(value)
        colors = preview_colors(self.pack)
        colors["accent"] = value
        self.pack["colors"] = T.auto_fix_colors(colors)[0]
        self._sync_vars()
        self.refresh()

    def action_terminal_preview(self) -> None:
        text = self.terminal_text()
        if self.test_mode:
            return
        window = tk.Toplevel(self)
        window.title("Terminal preview")
        window.configure(background=EDITOR_COLORS["bg"])
        frame = tk.Frame(window, background=EDITOR_COLORS["bg"])
        frame.pack(fill="both", expand=True, padx=8, pady=8)
        widget = tk.Text(
            frame,
            wrap="none",
            width=90,
            height=32,
            background=EDITOR_COLORS["sunken"],
            foreground=EDITOR_COLORS["text"],
            font=("Consolas", 9),
            bd=0,
        )
        widget.pack(fill="both", expand=True)
        widget.insert("1.0", text)
        widget.configure(state="disabled")
        self._chrome_button(frame, "Close", window.destroy).pack(side="right", pady=(6, 0))

    def action_guide(self) -> None:
        guide = (
            "Simple mode\n"
            "  Set the nine colours that shape the look. The three derived colours\n"
            "  (text on accent, text on selection, table header text) follow along.\n\n"
            "Everything mode\n"
            "  All 17 palette entries, the style switches, the font and every\n"
            "  metadata field.\n\n"
            "Generators\n"
            "  'Fix readability' moves text colours until every WCAG pair passes.\n"
            "  'High contrast' pushes them to AAA (7:1). 'Invert', 'Remove colour'\n"
            "  and the random ones are quick ways to find a look.\n\n"
            "Sharing\n"
            "  Install puts the pack in your themes folder. Bundle writes one .zip\n"
            "  you can send to anyone.\n\n"
            "Keyboard: Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Z undo, Ctrl+J copy JSON."
        )
        self._info(guide, title="Quick guide")

    def action_folders(self) -> None:
        folders = T.search_paths()
        text = "The app looks for packs here, in order:\n\n" + "\n".join(f"{i + 1}. {p}" for i, p in enumerate(folders))
        text += "\n\n'Install' copies the current theme into the first writable user folder."
        self._info(text, title="Where packs live")

    def _info(self, message: str, *, title: str = "Theme Studio") -> None:
        if self.test_mode:
            return
        messagebox.showinfo(title, message, parent=self)

    def _error(self, message: str, *, title: str = "Theme Studio") -> None:
        if self.test_mode:
            return
        messagebox.showerror(title, message, parent=self)

    def _confirm(self, question: str) -> bool:
        if self.test_mode:
            return True
        return bool(messagebox.askyesno(EDITOR_TITLE, question, parent=self))


class _Tooltip:
    """A one-line hover hint (keeps the toolbar readable)."""

    def __init__(self, widget: Any, text: str) -> None:
        self.widget = widget
        self.text = text
        self.tip: Any = None
        widget.bind("<Enter>", self._show, add="+")
        widget.bind("<Leave>", self._hide, add="+")

    def _show(self, _event: Any = None) -> None:
        if self.tip is not None or not self.text:
            return
        try:
            x = self.widget.winfo_rootx() + 10
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + 4
            self.tip = tk.Toplevel(self.widget)
            self.tip.wm_overrideredirect(True)
            self.tip.wm_geometry(f"+{x}+{y}")
            tk.Label(
                self.tip,
                text=self.text,
                background="#2b3138",
                foreground=EDITOR_COLORS["text"],
                padx=6,
                pady=2,
                bd=1,
                relief="solid",
            ).pack()
        except Exception:  # pragma: no cover
            self.tip = None

    def _hide(self, _event: Any = None) -> None:
        if self.tip is not None:
            try:
                self.tip.destroy()
            except Exception:  # pragma: no cover
                pass
            self.tip = None


class _NewPackDialog:
    """Name / base / author for a brand-new theme."""

    def __init__(self, parent: Any, author: str = "") -> None:
        self.result: tuple[str, str, str] | None = None
        window = tk.Toplevel(parent)
        window.title("New theme")
        window.configure(background=EDITOR_COLORS["bg"])
        window.transient(parent)
        window.grab_set()
        window.resizable(False, False)

        name_var = tk.StringVar(value="My Theme")
        base_var = tk.StringVar(value=T.FALLBACK_THEME_ID)
        author_var = tk.StringVar(value=author)

        for row, (label, var, values) in enumerate(
            (
                ("Name", name_var, []),
                ("Base theme", base_var, list(T.BUILTIN_ORDER)),
                ("Author", author_var, []),
            )
        ):
            tk.Label(
                window, text=label, anchor="w", background=EDITOR_COLORS["bg"], foreground=EDITOR_COLORS["text"]
            ).grid(row=row, column=0, sticky="w", padx=10, pady=(10 if row == 0 else 4, 0))
            if values:
                widget: Any = ttk.Combobox(window, textvariable=var, values=values, state="readonly", width=24)
            else:
                widget = tk.Entry(
                    window,
                    textvariable=var,
                    width=26,
                    background=EDITOR_COLORS["sunken"],
                    foreground=EDITOR_COLORS["text"],
                    insertbackground=EDITOR_COLORS["text"],
                )
            widget.grid(row=row, column=1, sticky="w", padx=10, pady=(10 if row == 0 else 4, 0))

        buttons = tk.Frame(window, background=EDITOR_COLORS["bg"])
        buttons.grid(row=3, column=0, columnspan=2, sticky="e", padx=10, pady=10)

        def accept() -> None:
            self.result = (name_var.get().strip() or "My Theme", base_var.get(), author_var.get().strip())
            window.destroy()

        self.parent = parent
        create = parent._chrome_button(buttons, "Create", accept)
        create.pack(side="right", padx=4)
        parent._chrome_button(buttons, "Cancel", window.destroy).pack(side="right", padx=4)
        window.bind("<Return>", lambda _e: accept())
        window.bind("<Escape>", lambda _e: window.destroy())
        try:
            window.update_idletasks()
            parent.wait_window(window)
        except Exception:  # pragma: no cover
            pass


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def check_pack(path: Path) -> dict[str, Any]:
    """The strict report ``--check`` prints.

    ``load_pack_data`` is deliberately forgiving, because the GUI must be able
    to open a half-finished pack, show what is wrong and let the author repair
    it. ``--check`` answers the opposite question: *would this file pass the
    rules in docs/THEME_FORMAT.md?*  So the raw ``T.validate_pack`` errors
    decide pass/fail, even though the app itself would still fill the missing
    colours in from the base palette.
    """
    path = Path(path)
    if not path.is_file():
        raise T.ThemeError(f"no such file: {path}")
    text = read_pack_text(path)
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise T.ThemeError(
            f"{path.name}: invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}"
        ) from exc
    if not isinstance(data, Mapping):
        raise T.ThemeError(f"{path.name}: a pack must be a JSON object")

    result = review(normalize_pack(data))
    raw_errors, raw_warnings = T.validate_pack(data, path.name)
    errors = list(raw_errors)
    for message in result["errors"]:
        if message not in errors:
            errors.append(message)
    warnings = list(raw_warnings)
    for message in result["warnings"]:
        if message not in warnings:
            warnings.append(message)
    return {
        "path": path,
        "pack": result["pack"],
        "errors": errors,
        "warnings": warnings,
        "rows": result["rows"],
        "repaired": bool(raw_errors),
    }


def run_check(paths: Sequence[str], *, quiet: bool = False) -> int:
    """``--check``: validate packs and print their contrast table. No GUI."""
    code = EXIT_OK
    for raw in paths:
        path = Path(raw)
        try:
            report = check_pack(path)
        except T.ThemeError as exc:
            print(f"FAIL  {path.name}: {exc}")
            code = EXIT_INVALID
            continue
        pack = report["pack"]
        errors, warnings = report["errors"], report["warnings"]
        if errors:
            code = EXIT_INVALID
        print(f"{'FAIL' if errors else 'OK  '}  {path.name}: {len(errors)} error(s), {len(warnings)} warning(s)")
        if quiet:
            continue
        print(f"      name={pack['name']!r} id={pack['id']!r} base={pack['base']} colors={len(pack['colors'])}")
        for message in errors:
            print(f"      error: {message}")
        if report["repaired"]:
            print("      note:  the editor would still open this file (missing colours come")
            print("             from the base palette), but it does not pass format validation")
        for message in warnings:
            print(f"      warn:  {message}")
        if report["rows"]:
            suffix = " (after filling the gaps from the base)" if report["repaired"] else ""
            print("      WCAG contrast" + suffix)
            for line in contrast_lines(report["rows"]):
                print("      " + line.strip())
    return code


def _selftest_pure(checks: list[tuple[str, bool, str]]) -> None:
    """The parts that need no display."""

    def check(name: str, condition: Any, detail: str = "") -> None:
        checks.append((name, bool(condition), str(detail)))

    base = build_pack(name="Selftest", pack_id="selftest")
    check("build_pack has every colour", len(base["colors"]) == len(T.COLOR_KEYS), f"{len(base['colors'])} keys")
    check("build_pack validates", not review(base)["errors"], review(base)["errors"][:2])
    check("slugify", slugify("Midnight Blue!!") == "midnight-blue", slugify("Midnight Blue!!"))
    check("empty id is rejected", bool(id_problem("")), id_problem(""))
    check("reserved id is rejected", "reserved" in id_problem("modern-dark"), id_problem("modern-dark"))
    check("installed id is flagged", "installed" in id_problem("nord", taken={"nord"}), id_problem("nord", taken={"nord"}))

    for key, label, _help in GENERATORS:
        pack = apply_generator(base, key, seed=7)
        result = review(pack)
        outcome = "ok" if not result["low"] else "best effort"
        check(f"generator {key} ({label})", len(pack["colors"]) == len(T.COLOR_KEYS) and pack["id"] == "selftest",
              f"{len(pack['colors'])} keys, {outcome}")
        check(f"generator {key} stays paintable", result["theme"] is not None, "")

    aaa = apply_generator(base, "aaa")
    check("high contrast reaches 7:1", not review(aaa)["low"], f"{len(review(aaa)['low'])} low")

    options = preset_options()
    check("presets exist", len(options) >= 4, f"{len(options)} sources")
    first = sorted(options)[0]
    preset_pack = apply_preset(base, options[first])
    check("preset keeps identity", preset_pack["id"] == "selftest" and preset_pack["name"] == "Selftest", first)

    check("json round-trips", normalize_pack(json.loads(pack_json(base))) == normalize_pack(base))
    check("terminal preview is plain", "\033" not in terminal_preview(base), "")
    check("contrast lines", len(contrast_lines(T.contrast_report(base["colors"]))) == len(T.CONTRAST_PAIRS))

    with tempfile.TemporaryDirectory(prefix="ipm-selftest-") as tmp:
        folder = Path(tmp)
        target = folder / "selftest.ipmtheme.json"
        target.write_text(pack_json(base), encoding="utf-8")
        loaded, problem = load_pack_data(target)
        check("saved pack reloads", loaded["id"] == "selftest" and not problem, problem)
        installed = T.install_pack(target, folder / "themes")
        check("install copies the pack", installed.is_file(), str(installed))
        bundle = T.bundle_pack(target, folder / "selftest.zip")
        check("bundle is a zip", zipfile.is_zipfile(bundle), str(bundle))
        check("editor can read its own bundle", load_pack_data(bundle)[0]["id"] == "selftest", "")

        report = check_pack(target)
        check("check_pack accepts a good pack", not report["errors"], report["errors"][:2])
        broken = folder / "broken.ipmtheme.json"
        broken.write_text('{"id": "broken", "name": "Broken", "base": "nord", "colors": {}}', encoding="utf-8")
        report = check_pack(broken)
        check("check_pack flags missing colours", bool(report["errors"]), f"{len(report['errors'])} error(s)")
        check("check_pack still reads a broken pack", report["pack"]["id"] == "broken", "")
        garbage = folder / "garbage.ipmtheme.json"
        garbage.write_text("{not json", encoding="utf-8")
        try:
            check_pack(garbage)
            check("check_pack rejects bad JSON", False, "no error raised")
        except T.ThemeError as exc:
            check("check_pack rejects bad JSON", "line 1" in str(exc), str(exc))

        with contextlib.redirect_stdout(io.StringIO()) as quiet_out:
            good_code = run_check([str(target)], quiet=True)
            bad_code = run_check([str(broken)], quiet=True)
            missing_code = run_check([str(folder / "nope.ipmtheme.json")], quiet=True)
        check("run_check exits 0 for a good pack", good_code == EXIT_OK, str(good_code))
        check("run_check exits 1 for a broken pack", bad_code == EXIT_INVALID, str(bad_code))
        check("run_check exits 1 for a missing file", missing_code == EXIT_INVALID, str(missing_code))
        check("quiet output is one line per file", len(quiet_out.getvalue().strip().splitlines()) == 3,
              repr(quiet_out.getvalue()[:60]))

    hoisted = _hoist_quiet(["--check", "a.json", "--quiet"])
    check("quiet is hoisted ahead of --check", hoisted == ["--quiet", "--check", "a.json"], str(hoisted))
    parsed = build_parser().parse_args(hoisted)
    check("--check parses with a trailing --quiet", parsed.quiet and parsed.check == ["a.json"], str(parsed.check))


def _selftest_gui(checks: list[tuple[str, bool, str]]) -> None:
    """Drive the real window through every action, without a mouse."""

    def check(name: str, condition: Any, detail: str = "") -> None:
        checks.append((name, bool(condition), str(detail)))

    editor = ThemeEditor(test_mode=True)
    editor.withdraw()
    try:
        editor.update()
        check("window paints", editor.winfo_exists() == 1, "")
        check("starts in simple mode", editor.mode.get() == "simple", editor.mode.get())
        check("simple mode shows essentials", set(editor.color_entries) == set(SIMPLE_KEYS),
              f"{len(editor.color_entries)} rows")

        editor.set_color("bg", "#101418")
        check("colour edit lands in the pack", editor.pack["colors"]["bg"] == "#101418", editor.pack["colors"]["bg"])
        editor.update()

        before = dict(editor.pack["colors"])
        check("generator readable", editor.apply_generator("readable"), "")
        editor.update()
        after = dict(editor.pack["colors"])
        check("readable keeps a full palette", set(after) == set(T.COLOR_KEYS), f"{len(after)} keys")
        check("readable output passes AA", not review(editor.pack)["low"], "")

        check("undo is available", editor.undo(), "")
        check("undo restores the earlier palette", editor.pack["colors"] == before, editor.pack["colors"].get("bg", ""))
        check("redo is available", editor.redo(), "")
        check("redo returns the fixed palette", editor.pack["colors"] == after, "")

        editor.set_color("danger", "#b91c1c")
        editor.reset_color("danger")
        check("reset returns the base colour",
              editor.pack["colors"]["danger"] == base_palette(editor.pack["base"])["danger"],
              editor.pack["colors"]["danger"])

        editor.set_mode("advanced")
        editor.update()
        check("advanced mode shows everything", set(editor.color_entries) == set(T.COLOR_KEYS),
              f"{len(editor.color_entries)} rows")
        check("advanced mode shows style switches", "ttk_theme" in editor.style_entries, "")
        check("icons checkbox exists", editor.icons_check is not None, "")
        editor.set_mode("simple")
        editor.update()

        names = sorted(preset_options())
        picked = editor.apply_preset(names[0]) if names else False
        check("preset applies", picked, names[0] if names else "none")
        editor.update()

        editor.set_color("accent", "#e0a020")
        editor.apply_generator("from-accent")
        editor.update()
        check("from-accent keeps the accent", editor.pack["colors"]["accent"] == "#e0a020",
              editor.pack["colors"]["accent"])
        check("checks panel has content", "Readability" in editor.checks_text(), editor.checks_text()[:40].replace("\n", " "))
        check("json panel has content", '"colors"' in editor.json_text(), "")
        check("terminal preview text", "ISO Package Manager" in editor.terminal_text(), "")
        check("status line", "mode" in editor.status_line(), editor.status_line())

        with tempfile.TemporaryDirectory(prefix="ipm-editor-selftest-") as tmp:
            folder = Path(tmp)
            saved = editor.save_to(folder / "selftest-editor.ipmtheme.json")
            check("save writes a file", saved.is_file(), str(saved))
            check("saved file is clean", not review(editor.pack)["errors"], review(editor.pack)["errors"][:2])
            check("not dirty after save", not editor.is_dirty(), "")
            installed = editor.install_to(folder / "themes")
            check("install from the editor", installed.is_file(), str(installed))
            bundled = editor.bundle_to(folder / "pack.zip")
            check("bundle from the editor", zipfile.is_zipfile(bundled), str(bundled))
            reopened = ThemeEditor(test_mode=True)
            reopened.withdraw()
            try:
                problem = reopened.open_path(saved)
                check("editor reopens its own file", reopened.pack["colors"]["accent"] == "#e0a020", problem)
                check("reopened pack is valid", not review(reopened.pack)["errors"], "")
            finally:
                reopened.destroy()
    finally:
        editor.destroy()


def run_self_test() -> int:
    checks: list[tuple[str, bool, str]] = []
    _selftest_pure(checks)
    if TK_AVAILABLE:
        try:
            _selftest_gui(checks)
        except Exception as exc:  # pragma: no cover - only on a broken Tk
            checks.append(("GUI self-test ran", False, f"{exc.__class__.__name__}: {exc}"))
    else:
        print(f"note  Tkinter is not available ({TK_ERROR}) - GUI checks skipped")

    failures = [name for name, ok, _detail in checks if not ok]
    for name, ok, detail in checks:
        mark = "PASS" if ok else "FAIL"
        print(f"{mark}  {name}" + (f"   {detail}" if detail else ""))
    print()
    print(f"{len(checks) - len(failures)}/{len(checks)} checks passed")
    if failures:
        print("failed: " + ", ".join(failures))
        return EXIT_INVALID
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="theme_editor",
        description="Theme Studio - a visual editor for ISO Package Manager theme packs.",
        epilog=(
            "Docs: docs/THEME_EDITOR.md   Format: docs/THEME_FORMAT.md   "
            "Sharing: docs/SHARING.md"
        ),
    )
    parser.add_argument("pack", nargs="?", default="", help="a .ipmtheme.json (or .zip) to open")
    parser.add_argument("--pack", dest="pack_option", default="", help="same as the positional pack")
    parser.add_argument("--new", dest="new_name", default="", help="start a brand-new theme with this name")
    parser.add_argument("--base", default=T.FALLBACK_THEME_ID, choices=list(T.BUILTIN_ORDER),
                        help="base theme for --new")
    parser.add_argument("--author", default="", help="author for --new")
    parser.add_argument("--mode", default="simple", choices=["simple", "advanced"],
                        help="which level of detail to open with")
    parser.add_argument("--check", nargs="+", metavar="FILE", help="validate packs and exit (no GUI)")
    parser.add_argument("--quiet", "-q", action="store_true", help="with --check: one line per file")
    parser.add_argument("--self-test", action="store_true", help="run the built-in checks and exit")
    return parser


def _hoist_quiet(argv: Sequence[str]) -> list[str]:
    """Move ``--quiet``/``-q`` ahead of ``--check``.

    ``--check`` uses ``nargs="+"``, so a trailing ``--quiet`` would otherwise be
    read as another pack name (``--check: expected at least one argument``).
    Hoisting keeps ``--check a.json --quiet`` and ``--quiet --check a.json``
    equivalent.
    """
    quiet = [arg for arg in argv if arg in ("--quiet", "-q")]
    rest = [arg for arg in argv if arg not in ("--quiet", "-q")]
    return quiet + rest


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(_hoist_quiet(list(sys.argv[1:] if argv is None else argv)))

    if args.self_test:
        return run_self_test()
    if args.check:
        return run_check(args.check, quiet=args.quiet)
    if not TK_AVAILABLE:
        print(f"error: this needs Tkinter, which is missing here ({TK_ERROR})", file=sys.stderr)
        print("      On Debian/Ubuntu: sudo apt install python3-tk", file=sys.stderr)
        print("      Use --check FILE or --self-test to work without a display.", file=sys.stderr)
        return EXIT_NO_TK

    target = args.pack_option or args.pack
    if target:
        try:
            pack, problem = load_pack_data(Path(target))
        except T.ThemeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return EXIT_USAGE
        if problem:
            print(f"note: {Path(target).name} has problems the app would reject:\n  {problem}")
        editor = ThemeEditor(pack, path=target)
    elif args.new_name:
        pack = build_pack(name=args.new_name, author=args.author, base=args.base)
        pack["id"] = slugify(args.new_name)
        editor = ThemeEditor(pack)
    else:
        editor = ThemeEditor(build_pack())

    editor.refresh_presets()
    editor.set_mode(args.mode)
    try:
        editor.mainloop()
    except KeyboardInterrupt:  # pragma: no cover
        return EXIT_USAGE
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
