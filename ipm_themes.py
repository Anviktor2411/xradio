#!/usr/bin/env python3
"""ipm_themes - drop-in theme-pack support for ISO Package Manager.

This module lets anyone create a theme for ISO Package Manager as a small JSON
file ("theme pack"), drop it in a themes folder, and share it with other people.
It is deliberately dependency-free and GUI-free at import time, so it can be used
from the Tk application, from the terminal edition, and from the standalone
``tools/ipmtheme.py`` validator.

Quick start for theme authors
-----------------------------

::

    python tools/ipmtheme.py new "My Theme" --base modern-dark
    python tools/ipmtheme.py validate themes/my-theme.ipmtheme.json
    python tools/ipmtheme.py preview  themes/my-theme.ipmtheme.json
    python tools/ipmtheme.py install  themes/my-theme.ipmtheme.json

Where packs are loaded from (first match wins for duplicate ids)
--------------------------------------------------------------

1. ``$IPM_THEMES_DIR`` - one or more directories, separated by ``os.pathsep``
   (``;`` on Windows, ``:`` on Linux/macOS).
2. ``<app folder>/themes`` - the ``themes`` folder next to the app, next to the
   ``.pyz`` or next to the ``.exe`` (:func:`container_dir` works out which).
3. ``~/.ipm/themes`` - the user's own packs (created on demand).
4. ``<archive>/themes`` - packs stored *inside* the running ``.pyz`` or a
   PyInstaller bundle, so one downloaded file already has themes to pick from.
   These are read straight out of the archive and lose to 1-3 on a duplicate id.

A pack is a JSON file. The extension may be ``.ipmtheme.json``, ``.ipmtheme`` or
plain ``.json``. A ``.zip`` bundle containing ``theme.json`` (plus an optional
screenshot) is also accepted, which is the recommended way to share a theme that
has a preview image.

Public API (stable)
-------------------

* :data:`BUILTIN_THEMES` - the four shipped themes, keyed by id.
* :func:`validate_pack` - ``(errors, warnings)`` for a decoded pack. Pure; no Tk.
* :func:`load_pack_file` / :func:`load_themes` - load one file / a whole registry.
* :func:`resolve` - id *or* display name -> :class:`Theme` (never raises).
* :func:`to_style_kwargs` - everything ``main.py`` needs to paint a theme.
* :func:`install_pack`, :func:`new_pack`, :func:`export_builtin`,
  :func:`bundle_pack`, :func:`ansi_preview`, :func:`contrast_report`.
"""

from __future__ import annotations

import colorsys
import difflib
import json
import os
import random
import re
import shutil
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

__all__ = [
    "SCHEMA_VERSION",
    "COLOR_KEYS",
    "REQUIRED_COLORS",
    "STYLE_KEYS",
    "ThemeError",
    "Theme",
    "ThemeRegistry",
    "BUILTIN_THEMES",
    "BUILTIN_ORDER",
    "PACK_SUFFIXES",
    "SCHEMA_HINT",
    "ARCHIVE_SUFFIXES",
    "search_paths",
    "user_theme_dir",
    "ensure_user_theme_dir",
    "app_theme_dir",
    "bundled_theme_dir",
    "container_dir",
    "is_pack_member",
    "iter_pack_files",
    "validate_pack",
    "load_pack_file",
    "load_themes",
    "get_registry",
    "invalidate_registry",
    "resolve",
    "menu_structure",
    "to_style_kwargs",
    "is_color",
    "to_rgb",
    "contrast_ratio",
    "contrast_report",
    "new_pack",
    "install_pack",
    "bundle_pack",
    "export_builtin",
    "ansi_preview",
    "theme_from_pack",
    "CONTRAST_PAIRS",
    "to_hex",
    "rgb_to_hex",
    "mix",
    "lighten",
    "darken",
    "inverted",
    "is_dark",
    "best_text_on",
    "nudge_for_contrast",
    "auto_fix_colors",
    "palette_from_accent",
    "random_palette",
    "invert_palette",
    "grayscale_palette",
    "high_contrast_palette",
    "palette_sources",
]

SCHEMA_VERSION = 1
APP_NAME = "ISO Package Manager"
APP_VERSION = "0.10.1"
try:  # the launcher owns the version number; this is only a fallback
    from ipm_launcher import APP_VERSION as APP_VERSION  # noqa: F811
except Exception:  # pragma: no cover - running without the launcher
    pass

PACK_SUFFIXES: tuple[str, ...] = (".ipmtheme.json", ".ipmtheme", ".json")

# ---------------------------------------------------------------------------
# Palette contract - these are the keys main.py paints with
# ---------------------------------------------------------------------------

#: Every colour a theme may define.
COLOR_KEYS: tuple[str, ...] = (
    "bg",
    "panel",
    "text",
    "muted",
    "accent",
    "accent_active",
    "danger",
    "danger_active",
    "border",
    "selection",
    "tree_bg",
    "tree_heading_bg",
    "tab_bg",
    "tab_selected_bg",
    "accent_text",
    "tree_heading_fg",
    "selection_fg",
)

#: Colours a pack *must* set (the rest fall back to sensible derivations).
REQUIRED_COLORS: tuple[str, ...] = (
    "bg",
    "panel",
    "text",
    "muted",
    "accent",
    "accent_active",
    "danger",
    "danger_active",
    "border",
    "selection",
)

#: Non-colour style switches a pack may set.
STYLE_KEYS: tuple[str, ...] = (
    "ttk_theme",
    "button_relief",
    "tab_relief",
    "tree_heading_relief",
    "disabled_bg",
    "disabled_fg",
    "pressed_bg",
    "focus_border",
    "use_icons",
)

#: Pack keys that are metadata rather than palette/style.
META_KEYS: tuple[str, ...] = (
    "$schema",
    "schema_version",
    "id",
    "name",
    "author",
    "version",
    "description",
    "license",
    "homepage",
    "screenshot",
    "base",
    "font_family",
    "colors",
    "style",
    "min_app_version",
)

_TOP_LEVEL_KEYS: tuple[str, ...] = META_KEYS

#: ttk themes a pack may request. "auto" means "let the app decide".
TTK_THEMES: tuple[str, ...] = ("auto", "clam", "alt", "default", "classic", "vista", "xpnative", "winnative", "aqua")

_RELIEFS: tuple[str, ...] = ("flat", "raised", "sunken", "groove", "ridge", "solid")

_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
_VERSION_RE = re.compile(r"^\d+(\.\d+){0,3}([-+][0-9A-Za-z.-]+)?$")
_HEX_RE = re.compile(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$")
_NAMED_TOKEN_RE = re.compile(r"^[a-z]+[0-9]?$")
_CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")

#: Vocabulary the words of a Tk/X11 colour name are built from. Keeping this
#: list means "light gray" is accepted while "not a colour" is rejected, which
#: is what stops a typo like {"bg": "nope"} from silently loading as a colour.
_COLOR_WORDS: frozenset[str] = frozenset(
    """
    alice antique aqua aquamarine azure beige bisque black blanched blue blueviolet
    blurwood brown burlywood cadet chartreuse chocolate coral corn cornflower cornsilk
    crimson cyan dark debian deep deeppink dim dodger dodgerblue fire firebrick floral
    floralwhite forest forestgreen fuchsia gainsboro ghost ghostwhite gold golden
    goldenrod gray green greenyellow grey honey honeydew hot hotpink indian indianred
    indigo ivory khaki khakiblue lawn lawn-green lawngreen lavender lemon lemonchiffon
    light lime limegreen linen magenta maroon mauve medium midnight midnightblue mint
    mintcream misty mistyrose moccasin nava navajo navajowhite navy navyblue old oldlace
    olive olivedrab orange orangered orchid pale palegoldenrod palegreen paleturquoise
    palevioletred papaya papayawhip peach peachpuff peru pink plum powder powderblue
    purple raspberry rebecca rebeccapurple red rosy rosybrown royal royalblue saddle
    saddlebrown salmon sandy sandybrown sea seagreen seashell sienna silver sky skyblue
    slate slateblue slategray slategrey snow spring springgreen steel steelblue tan teal
    thistle tomato turquoise violet violetred wheat white whitesmoke wood yellow
    yellowgreen
    """.split()
)


def _named_color_ok(value: str) -> bool:
    """True when *value* looks like a Tk/X11 colour name (``light gray``)."""
    words = value.lower().split()
    if not words or len(words) > 4:
        return False
    for word in words:
        if not _NAMED_TOKEN_RE.match(word):
            return False
        stem = word[:-1] if word[-1].isdigit() else word
        if stem not in _COLOR_WORDS:
            return False
    return True

#: A small subset of Tk's named colours, enough for WCAG contrast maths and for
#: a friendly "did you mean #xxxxxx?" hint. Named colours outside this table are
#: still accepted - they are verified by Tk itself when the GUI runs.
TK_NAMED_COLORS: dict[str, tuple[int, int, int]] = {
    "black": (0, 0, 0),
    "white": (255, 255, 255),
    "red": (255, 0, 0),
    "green": (0, 128, 0),
    "blue": (0, 0, 255),
    "yellow": (255, 255, 0),
    "cyan": (0, 255, 255),
    "aqua": (0, 255, 255),
    "magenta": (255, 0, 255),
    "fuchsia": (255, 0, 255),
    "orange": (255, 165, 0),
    "purple": (128, 0, 128),
    "brown": (165, 42, 42),
    "pink": (255, 192, 203),
    "navy": (0, 0, 128),
    "teal": (0, 128, 128),
    "olive": (128, 128, 0),
    "maroon": (128, 0, 0),
    "silver": (192, 192, 192),
    "gray": (128, 128, 128),
    "grey": (128, 128, 128),
    "lightgray": (211, 211, 211),
    "lightgrey": (211, 211, 211),
    "darkgray": (169, 169, 169),
    "darkgrey": (169, 169, 169),
    "dimgray": (105, 105, 105),
    "dimgrey": (105, 105, 105),
    "whitesmoke": (245, 245, 245),
    "gainsboro": (220, 220, 220),
    "lime": (0, 255, 0),
    "limegreen": (50, 205, 50),
    "gold": (255, 215, 0),
    "skyblue": (135, 206, 235),
    "steelblue": (70, 130, 180),
    "slategray": (112, 128, 144),
    "indigo": (75, 0, 130),
    "crimson": (220, 20, 60),
    "salmon": (250, 128, 114),
    "tomato": (255, 99, 71),
    "khaki": (240, 230, 140),
    "ivory": (255, 255, 240),
    "beige": (245, 245, 220),
    "snow": (255, 250, 250),
    "azure": (240, 255, 255),
    "mintcream": (245, 255, 250),
    "seashell": (255, 245, 238),
    "linen": (250, 240, 230),
    "darkslategray": (47, 79, 79),
    "darkslategrey": (47, 79, 79),
    "midnightblue": (25, 25, 112),
    "rebeccapurple": (102, 51, 153),
}


class ThemeError(Exception):
    """Raised when a theme pack cannot be loaded."""


# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------

def _clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return low if value < low else high if value > high else value


def is_color(value: Any) -> bool:
    """True for ``#rgb`` / ``#rrggbb`` or a Tk-style colour name."""
    if not isinstance(value, str):
        return False
    value = value.strip()
    if not value or _CONTROL_RE.search(value):
        return False
    return bool(_HEX_RE.match(value) or _named_color_ok(value))


def to_rgb(value: Any) -> tuple[int, int, int] | None:
    """``#rrggbb`` / ``#rgb`` / known name -> ``(r, g, b)``; ``None`` if unknown."""
    if not isinstance(value, str):
        return None
    text = value.strip()
    if _HEX_RE.match(text):
        digits = text[1:]
        if len(digits) == 3:
            digits = "".join(ch * 2 for ch in digits)
        return tuple(int(digits[i : i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]
    return TK_NAMED_COLORS.get(text.lower())


def _relative_luminance(rgb: tuple[int, int, int]) -> float:
    channels = []
    for raw in rgb:
        c = raw / 255.0
        channels.append(c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4)
    r, g, b = channels
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast_ratio(color_a: Any, color_b: Any) -> float | None:
    """WCAG 2.1 contrast ratio (1.0 .. 21.0), or ``None`` if undecidable."""
    rgb_a = to_rgb(color_a)
    rgb_b = to_rgb(color_b)
    if rgb_a is None or rgb_b is None:
        return None
    lum_a = _relative_luminance(rgb_a)
    lum_b = _relative_luminance(rgb_b)
    lighter, darker = (lum_a, lum_b) if lum_a >= lum_b else (lum_b, lum_a)
    return (lighter + 0.05) / (darker + 0.05)


def _hex_like(value: Any) -> str | None:
    """Return ``#rrggbb`` for a known colour, else ``None`` (for hints)."""
    rgb = to_rgb(value)
    if rgb is None:
        return None
    return "#{:02x}{:02x}{:02x}".format(*rgb)


#: (foreground key, background key, minimum ratio, severity)
CONTRAST_PAIRS: tuple[tuple[str, str, float, str], ...] = (
    ("text", "bg", 4.5, "warn"),
    ("text", "panel", 4.5, "warn"),
    ("muted", "bg", 3.0, "warn"),
    ("accent_text", "accent", 3.0, "warn"),
    ("selection_fg", "selection", 3.0, "warn"),
    ("tree_heading_fg", "tree_heading_bg", 4.5, "warn"),
    ("text", "tree_bg", 4.5, "warn"),
)


def contrast_report(colors: Mapping[str, Any]) -> list[tuple[str, str, float | None, float, bool]]:
    """Check a palette's legibility.

    Returns a list of ``(label, ratio_text, ratio, minimum, ok)`` tuples so both
    the validator and the CLI can print the same numbers.
    """
    rows: list[tuple[str, str, float | None, float, bool]] = []
    for fg_key, bg_key, minimum, _severity in CONTRAST_PAIRS:
        fg = colors.get(fg_key)
        bg = colors.get(bg_key)
        ratio = contrast_ratio(fg, bg)
        label = f"{fg_key} on {bg_key}"
        ratio_text = "n/a" if ratio is None else f"{ratio:.2f}:1"
        rows.append((label, ratio_text, ratio, minimum, ratio is not None and ratio >= minimum))
    return rows


# ---------------------------------------------------------------------------
# Style defaults per base theme
# ---------------------------------------------------------------------------

_STYLE_DEFAULTS_BY_BASE: dict[str, dict[str, Any]] = {
    "default": {
        "ttk_theme": "auto",
        "button_relief": "flat",
        "tab_relief": "flat",
        "tree_heading_relief": "flat",
        "disabled_bg": "#e5e7eb",
        "disabled_fg": "#9ca3af",
        "pressed_bg": None,
        "focus_border": None,
        "use_icons": False,
    },
    "modern-dark": {
        "ttk_theme": "auto",
        "button_relief": "flat",
        "tab_relief": "flat",
        "tree_heading_relief": "flat",
        "disabled_bg": "#374151",
        "disabled_fg": "#9ca3af",
        "pressed_bg": None,
        "focus_border": None,
        "use_icons": False,
    },
    "windows-xp": {
        "ttk_theme": "xpnative",
        "button_relief": "raised",
        "tab_relief": "raised",
        "tree_heading_relief": "raised",
        "disabled_bg": "#d4d0c8",
        "disabled_fg": "#404040",
        "pressed_bg": "#bdbab3",
        "focus_border": "border",
        "use_icons": False,
    },
    "graphical": {
        "ttk_theme": "clam",
        "button_relief": "raised",
        "tab_relief": "flat",
        "tree_heading_relief": "flat",
        "disabled_bg": "#e5e7eb",
        "disabled_fg": "muted",
        "pressed_bg": "#e0f2fe",
        "focus_border": "accent",
        "use_icons": True,
    },
}


def _base_palette(base: str) -> dict[str, str]:
    """Fully resolved palette for a built-in base theme."""
    return dict(BUILTIN_PALETTES[base])


# Built-in palettes, transcribed from main.py's _apply_style so the shipped
# themes look exactly as before. tests/test_ipm_themes.py re-checks these against
# the application source, so a drift there fails the test suite.
BUILTIN_PALETTES: dict[str, dict[str, str]] = {
    "default": {
        "bg": "#f3f4f6",
        "panel": "#ffffff",
        "text": "#111827",
        "muted": "#4b5563",
        "accent": "#2563eb",
        "accent_active": "#1d4ed8",
        "danger": "#dc2626",
        "danger_active": "#b91c1c",
        "border": "#d1d5db",
        "selection": "#2563eb",
        "tree_bg": "#ffffff",
        "tree_heading_bg": "#f3f4f6",
        "tab_bg": "#e5e7eb",
        "tab_selected_bg": "#ffffff",
    },
    "modern-dark": {
        "bg": "#0f172a",
        "panel": "#111827",
        "text": "#e5e7eb",
        "muted": "#9ca3af",
        "accent": "#2563eb",
        "accent_active": "#1d4ed8",
        "danger": "#dc2626",
        "danger_active": "#b91c1c",
        "border": "#1f2937",
        "selection": "#1f3a8a",
        "tree_bg": "#111827",
        "tree_heading_bg": "#0f172a",
        "tab_bg": "#111827",
        "tab_selected_bg": "#0f172a",
    },
    "windows-xp": {
        "bg": "#d4d0c8",
        "panel": "#ece9d8",
        "text": "#000000",
        "muted": "#404040",
        "accent": "#d4d0c8",
        "accent_active": "#c9c6bf",
        "danger": "#d4d0c8",
        "danger_active": "#c9c6bf",
        "border": "#808080",
        "selection": "#316ac5",
        "tree_bg": "#ffffff",
        "tree_heading_bg": "#d4d0c8",
        "tab_bg": "#ece9d8",
        "tab_selected_bg": "#ffffff",
    },
    "graphical": {
        "bg": "#e0f2fe",
        "panel": "#ffffff",
        "text": "#0f172a",
        "muted": "#334155",
        "accent": "#0ea5e9",
        "accent_active": "#0284c7",
        "danger": "#ef4444",
        "danger_active": "#dc2626",
        "border": "#94a3b8",
        "selection": "#0ea5e9",
        "tree_bg": "#ffffff",
        "tree_heading_bg": "#bae6fd",
        "tab_bg": "#c7d2fe",
        "tab_selected_bg": "#ffffff",
    },
}

#: Display names for the built-ins (what the menu shows).
BUILTIN_NAMES: dict[str, str] = {
    "default": "Default",
    "modern-dark": "Modern Dark",
    "windows-xp": "Windows XP",
    "graphical": "Graphical",
}

#: Menu order for the built-ins.
BUILTIN_ORDER: tuple[str, ...] = ("default", "modern-dark", "windows-xp", "graphical")

#: Font stack per built-in, matching main.py.
BUILTIN_FONTS: dict[str, str] = {
    "default": "Segoe UI",
    "modern-dark": "Segoe UI",
    "windows-xp": "Tahoma",
    "graphical": "Segoe UI",
}

#: Fallback if a pack's base is unusable.
FALLBACK_THEME_ID = "modern-dark"

#: Where a pack should point its editor for completion. Packs live in ``themes/``
#: one level below the schema in this project, hence the ``../``.
SCHEMA_HINT = "../theme-pack.schema.json"


# ---------------------------------------------------------------------------
# Theme object
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Theme:
    """A resolved theme: built-in or a user pack."""

    id: str
    name: str
    colors: dict[str, str]
    style: dict[str, Any]
    font_family: str
    base: str = "modern-dark"
    author: str = ""
    version: str = ""
    description: str = ""
    license: str = ""
    homepage: str = ""
    screenshot: str = ""
    min_app_version: str = ""
    kind: str = "pack"  # "builtin" | "pack"
    path: Path | None = None
    raw: dict[str, Any] = field(default_factory=dict)

    # -- convenience -------------------------------------------------------
    @property
    def is_builtin(self) -> bool:
        return self.kind == "builtin"

    @property
    def variant(self) -> str:
        """``flat`` | ``graphical`` | ``xp`` - drives main.py's style branches."""
        if self.style.get("button_relief") == "raised" or self.style.get("use_icons"):
            if self.base == "windows-xp" or self.style.get("ttk_theme") == "xpnative":
                return "xp"
            return "graphical"
        return "flat"

    @property
    def use_icons(self) -> bool:
        return bool(self.style.get("use_icons"))

    @property
    def label(self) -> str:
        """Menu label: the display name, plus the id when it is a pack."""
        return self.name

    @property
    def source(self) -> str:
        """Human-readable origin: ``built-in``, a file path, or an archive member."""
        if self.is_builtin:
            return "built-in"
        if not self.path:
            return "pack"
        split = _archive_member(self.path)
        if split is not None:
            archive, member = split
            return f"{member} (inside {archive.name})"
        return str(self.path)

    def color(self, key: str, default: str = "#000000") -> str:
        return str(self.colors.get(key, default))

    def to_dict(self) -> dict[str, Any]:
        """Serialise back to pack format (used by export/reload)."""
        return {
            "schema_version": SCHEMA_VERSION,
            "id": self.id,
            "name": self.name,
            "author": self.author,
            "version": self.version or "1.0.0",
            "description": self.description,
            "license": self.license,
            "homepage": self.homepage,
            "screenshot": self.screenshot,
            "base": self.base,
            "font_family": self.font_family,
            "colors": {k: self.colors[k] for k in COLOR_KEYS if k in self.colors},
            "style": dict(self.style),
        }

    def style_kwargs(self) -> dict[str, Any]:
        """Flat dict for the GUI: all palette keys + style switches."""
        out: dict[str, Any] = {}
        for key in COLOR_KEYS:
            out[key] = self.color(key)
        out["font_family"] = self.font_family
        out["id"] = self.id
        out["name"] = self.name
        out["base"] = self.base
        out["variant"] = self.variant
        out["is_graphical"] = self.variant == "graphical"
        out["is_xp"] = self.variant == "xp"
        out["is_flat"] = self.variant == "flat"
        out["use_icons"] = self.use_icons
        for key in STYLE_KEYS:
            value = self.style.get(key)
            if isinstance(value, str) and value in self.colors:
                value = self.colors[value]  # "auto: use this palette entry"
            out[key] = value
        return out


def _builtin_theme(theme_id: str) -> Theme:
    palette = _base_palette(theme_id)
    palette.setdefault("accent_text", "#ffffff")
    palette.setdefault("tree_heading_fg", palette["text"])
    palette.setdefault("selection_fg", "#ffffff")
    style = dict(_STYLE_DEFAULTS_BY_BASE[theme_id])
    for key, value in list(style.items()):
        if isinstance(value, str) and value in palette:
            style[key] = palette[value]
    return Theme(
        id=theme_id,
        name=BUILTIN_NAMES[theme_id],
        colors=palette,
        style=style,
        font_family=BUILTIN_FONTS[theme_id],
        base=theme_id,
        author=APP_NAME,
        version=APP_VERSION,
        description=f"Built-in {BUILTIN_NAMES[theme_id]} theme.",
        license="Same as the application",
        kind="builtin",
        path=None,
        raw={},
    )


BUILTIN_THEMES: dict[str, Theme] = {tid: _builtin_theme(tid) for tid in BUILTIN_ORDER}


# ---------------------------------------------------------------------------
# Where packs live
# ---------------------------------------------------------------------------

#: File extensions that mean "this path is really an archive we are inside".
ARCHIVE_SUFFIXES: tuple[str, ...] = (".pyz", ".zip", ".egg", ".whl", ".pex")


def _archive_root(path: str | os.PathLike[str]) -> Path | None:
    """Return the archive *path* lives in, or ``None`` for a normal path.

    A zipapp sets ``__file__`` to something like
    ``<folder>/iso-package-manager.pyz/ipm_themes.py`` and a one-file
    PyInstaller build sits in a temporary extraction folder, so "the folder
    next to this module" is not a real folder at all.  Walking the parts until
    one ends in :data:`ARCHIVE_SUFFIXES` recovers the archive itself.
    """
    try:
        parts = Path(path).parts
    except Exception:  # pragma: no cover - non path-like input
        return None
    for index, part in enumerate(parts):
        if part.lower().endswith(ARCHIVE_SUFFIXES):
            return Path(*parts[: index + 1])
    return None


def _archive_member(path: str | os.PathLike[str]) -> tuple[Path, str] | None:
    """Split ``<archive>/<member>`` into its two halves (``None`` otherwise)."""
    archive = _archive_root(path)
    if archive is None:
        return None
    remainder = Path(path).parts[len(archive.parts):]
    if not remainder:
        return None
    return archive, "/".join(remainder)


def container_dir() -> Path:
    """The folder the running program lives in - where ``themes`` is expected.

    * a plain checkout (``python main.py``) -> the folder holding the modules;
    * a zipapp (``iso-package-manager-0.10.1.pyz``) -> the folder with the ``.pyz``;
    * a one-file ``.exe`` -> the folder with the ``.exe``.
    """
    if getattr(sys, "frozen", False):
        executable = getattr(sys, "executable", "") or ""
        if executable:
            try:
                return Path(executable).resolve().parent
            except Exception:  # pragma: no cover - defensive
                return Path(executable).parent

    try:
        module_path = Path(__file__).resolve()
    except Exception:  # pragma: no cover - exotic importers only
        module_path = Path.cwd() / "ipm_themes.py"

    archive = _archive_root(module_path)
    if archive is not None:
        return archive.parent

    extracted = getattr(sys, "_MEIPASS", None)
    if extracted:
        return Path(extracted)
    return module_path.parent


def bundled_theme_dir() -> Path | None:
    """``themes`` *inside* the running archive/bundle, or ``None`` if there is none."""
    extracted = getattr(sys, "_MEIPASS", None)
    if extracted:
        return Path(extracted) / "themes"
    try:
        module_path = Path(__file__).resolve()
    except Exception:  # pragma: no cover - defensive
        return None
    archive = _archive_root(module_path)
    if archive is not None and archive.is_file():
        return archive / "themes"
    return None


#: Base name used when addressing a ``themes`` folder inside an archive.
THEMES_FOLDER_NAME = "themes"


def is_pack_member(path: str | os.PathLike[str]) -> bool:
    """True when *path* addresses an existing pack stored inside a zip archive."""
    split = _archive_member(path)
    if split is None:
        return False
    archive, member = split
    # The lookup key is the *whole* member name ("themes/nord.ipmtheme.json"),
    # while the pack-suffix test only cares about the file name part of it.
    full = member.replace("\\", "/").strip("/").lower()
    if not full or not _looks_like_pack(Path(full)):
        return False
    try:
        if not archive.is_file():
            return False
        with zipfile.ZipFile(archive) as handle:
            names = {n.replace("\\", "/").strip("/").lower() for n in handle.namelist()}
    except (OSError, zipfile.BadZipFile):
        return False
    return full in names


def app_theme_dir(app_dir: str | os.PathLike[str] | None = None) -> Path:
    """``<app folder>/themes`` - packs shipped or dropped next to the app."""
    if app_dir is None:
        app_dir = container_dir()
    return Path(app_dir) / THEMES_FOLDER_NAME


def user_theme_dir() -> Path:
    """``~/.ipm/themes`` - the user's own packs. Not created here."""
    return Path.home() / ".ipm" / "themes"


def ensure_user_theme_dir() -> Path:
    """Create ``~/.ipm/themes`` if needed and return it."""
    target = user_theme_dir()
    try:
        target.mkdir(parents=True, exist_ok=True)
    except Exception as exc:  # pragma: no cover - only on a broken home dir
        raise ThemeError(f"could not create {target}: {exc}") from exc
    return target


def search_paths(app_dir: str | os.PathLike[str] | None = None) -> list[Path]:
    """Theme folders in load order: env var, app folder, user, then the archive."""
    paths: list[Path] = []

    env = os.environ.get("IPM_THEMES_DIR", "")
    if env.strip():
        for chunk in env.split(os.pathsep):
            chunk = chunk.strip().strip('"')
            if chunk:
                paths.append(Path(chunk).expanduser())

    paths.append(app_theme_dir(app_dir))
    paths.append(user_theme_dir())

    # Packs baked into a .pyz / one-file bundle come last on purpose: whatever
    # the user installs themselves always beats what shipped inside the file.
    if app_dir is None:
        bundled = bundled_theme_dir()
        if bundled is not None:
            paths.append(bundled)

    seen: set[str] = set()
    unique: list[Path] = []
    for path in paths:
        try:
            key = str(path.resolve()).lower()
        except Exception:
            key = str(path).lower()
        if key not in seen:
            seen.add(key)
            unique.append(path)
    return unique


def _theme_display_name(pack_id: str) -> str:
    return pack_id.replace("-", " ").replace("_", " ").title()


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

def _suggest(key: str, options: Sequence[str]) -> str:
    match = difflib.get_close_matches(key, list(options), n=1, cutoff=0.6)
    return f" (did you mean {match[0]!r}?)" if match else ""


def _check_text(value: Any, field_name: str, errors: list[str], *, max_len: int = 200) -> str:
    if not isinstance(value, str) or not value.strip():
        errors.append(f"{field_name}: must be a non-empty string")
        return ""
    text = value.strip()
    if _CONTROL_RE.search(text):
        errors.append(f"{field_name}: contains control characters")
    if len(text) > max_len:
        errors.append(f"{field_name}: too long ({len(text)} > {max_len} characters)")
    return text


def validate_pack(data: Any, filename: str = "<pack>") -> tuple[list[str], list[str]]:
    """Validate a decoded pack.

    Returns ``(errors, warnings)``. Errors mean the pack will not load; warnings
    are things an author should fix but that still render. This function is pure
    (no Tk, no filesystem access) which is what makes it usable from CI.
    """
    errors: list[str] = []
    warnings: list[str] = []

    if not isinstance(data, dict):
        return ([f"{filename}: top level must be a JSON object, got {type(data).__name__}"], [])

    # -- schema version ----------------------------------------------------
    version = data.get("schema_version", SCHEMA_VERSION)
    if not isinstance(version, int) or isinstance(version, bool):
        errors.append("schema_version: must be an integer")
    elif version != SCHEMA_VERSION:
        errors.append(
            f"schema_version: {version} is not supported by this build (expected {SCHEMA_VERSION})"
        )

    # -- identity ----------------------------------------------------------
    pack_id = data.get("id")
    if not isinstance(pack_id, str) or not pack_id.strip():
        errors.append("id: required, e.g. \"my-theme\"")
        pack_id = ""
    else:
        pack_id = pack_id.strip()
        if not _ID_RE.match(pack_id):
            errors.append(
                "id: must be 1-64 characters of a-z, 0-9, '.', '_' or '-', and start with a letter or digit"
            )
        if pack_id.lower() in BUILTIN_THEMES:
            errors.append(f"id: {pack_id!r} is reserved by a built-in theme - pick another id")

    name = _check_text(data.get("name", ""), "name", errors, max_len=48)
    if name and name.lower() in {t.name.lower() for t in BUILTIN_THEMES.values()}:
        warnings.append(f"name: {name!r} is the same as a built-in theme - it will be listed twice")
    if name and pack_id and not errors:
        expected = _theme_display_name(pack_id)
        if name != expected and name.lower() != expected.lower():
            warnings.append(
                f"name: {name!r} differs from the id {pack_id!r} (displayed name is what users see - fine if intentional)"
            )

    author = data.get("author", "")
    if author and not isinstance(author, str):
        errors.append("author: must be a string when present")
    if not author:
        warnings.append("author: empty - other people cannot credit you when they share your theme")

    version_text = data.get("version", "1.0.0")
    if not isinstance(version_text, str) or not _VERSION_RE.match(version_text.strip()):
        errors.append("version: must look like 1, 1.0 or 1.0.0")
    elif version_text.strip() in ("1", "1.0", "0.1.0"):
        warnings.append(f"version: {version_text!r} - bump it with every change so users can tell packs apart")

    for field_name in ("description", "license", "homepage", "screenshot", "min_app_version"):
        value = data.get(field_name, "")
        if value in ("", None):
            continue
        if not isinstance(value, str):
            errors.append(f"{field_name}: must be a string")
        elif _CONTROL_RE.search(value):
            errors.append(f"{field_name}: contains control characters")

    homepage = data.get("homepage", "")
    if isinstance(homepage, str) and homepage.strip() and not homepage.strip().lower().startswith(("http://", "https://")):
        warnings.append("homepage: expected an http(s) URL")

    screenshot = data.get("screenshot", "")
    if isinstance(screenshot, str) and screenshot.strip():
        shot = screenshot.strip().replace("\\", "/")
        if shot.startswith("/") or ".." in shot.split("/"):
            errors.append("screenshot: must be a plain relative filename inside the pack folder or bundle")
        elif not shot.lower().endswith((".png", ".gif", ".jpg", ".jpeg", ".webp")):
            warnings.append(f"screenshot: {shot!r} is not a common image type (png/gif/jpg/webp)")

    if not data.get("license"):
        warnings.append("license: empty - say how others may share your theme (MIT is a common choice)")

    # -- base --------------------------------------------------------------
    base = data.get("base", FALLBACK_THEME_ID)
    if not isinstance(base, str) or base.strip().lower() not in BUILTIN_THEMES:
        errors.append(
            "base: must be one of " + ", ".join(f'"{b}"' for b in BUILTIN_ORDER)
        )
        base = FALLBACK_THEME_ID
    else:
        base = base.strip().lower()

    font_family = data.get("font_family", "")
    if font_family and (not isinstance(font_family, str) or _CONTROL_RE.search(font_family)):
        errors.append("font_family: must be a font name string")

    # -- unknown keys ------------------------------------------------------
    for key in data:
        if key not in _TOP_LEVEL_KEYS:
            warnings.append(f"unknown top-level key {key!r}{_suggest(key, _TOP_LEVEL_KEYS)}")

    # -- colours -----------------------------------------------------------
    colors = data.get("colors", {})
    if not isinstance(colors, dict):
        errors.append("colors: must be an object of colour values")
        colors = {}
    else:
        for key in colors:
            if key not in COLOR_KEYS:
                warnings.append(
                    f"colors.{key}: unknown colour{_suggest(key, COLOR_KEYS)} - "
                    f"valid keys are: {', '.join(COLOR_KEYS)}"
                )
        for key in REQUIRED_COLORS:
            if key not in colors:
                errors.append(f"colors.{key}: required colour is missing")
        for key, value in colors.items():
            if not is_color(value):
                hint = ""
                rgb = _hex_like(value) if isinstance(value, str) else None
                if isinstance(value, str) and value.strip() and not value.strip().startswith("#"):
                    hint = " (use #rrggbb, or a Tk colour name such as 'white')"
                elif rgb:
                    hint = f" (use {rgb})"
                errors.append(f"colors.{key}: {value!r} is not a valid colour{hint}")
                continue
            if isinstance(value, str) and not value.strip().startswith("#") and value.strip().lower() not in TK_NAMED_COLORS:
                warnings.append(
                    f"colors.{key}: {value!r} is a named colour this validator cannot measure - Tk checks it at runtime"
                )

    if not errors:
        resolved = _resolve_colors(base, colors)
        if resolved.get("accent") == resolved.get("bg"):
            warnings.append("colors.accent equals colors.bg - buttons will be invisible")
        if resolved.get("panel") == resolved.get("bg") and resolved.get("border") == resolved.get("bg"):
            warnings.append("colors.panel, colors.bg and colors.border are all the same - panels will not stand out")
        for label, ratio_text, ratio, minimum, ok in contrast_report(resolved):
            if ratio is not None and not ok:
                warnings.append(
                    f"contrast: {label} is only {ratio_text} (want >= {minimum:.1f}:1) - hard to read for some users"
                )

        # A pack that changes nothing is probably a copy/paste mistake.
        base_palette = BUILTIN_PALETTES[base]
        comparable = [k for k in base_palette if k in resolved]
        same = [k for k in comparable if resolved[k].lower() == base_palette[k].lower()]
        if comparable and len(same) == len(comparable):
            warnings.append(f"colors: identical to the base theme {base!r} - did you forget to edit them?")

    # -- style -------------------------------------------------------------
    style = data.get("style", {})
    if style and not isinstance(style, dict):
        errors.append("style: must be an object")
    elif isinstance(style, dict):
        for key in style:
            if key not in STYLE_KEYS:
                warnings.append(f"style.{key}: unknown option{_suggest(key, STYLE_KEYS)}")
        ttk_theme = style.get("ttk_theme")
        if ttk_theme is not None and (not isinstance(ttk_theme, str) or ttk_theme not in TTK_THEMES):
            errors.append("style.ttk_theme: must be one of " + ", ".join(f'"{t}"' for t in TTK_THEMES))
        use_icons = style.get("use_icons")
        if use_icons is not None and not isinstance(use_icons, bool):
            errors.append("style.use_icons: must be true or false")
        for key in ("button_relief", "tab_relief", "tree_heading_relief"):
            value = style.get(key)
            if value is not None and (not isinstance(value, str) or value not in _RELIEFS):
                errors.append(f"style.{key}: must be one of " + ", ".join(_RELIEFS))
        for key in ("disabled_bg", "disabled_fg", "pressed_bg", "focus_border"):
            value = style.get(key)
            if value is None:
                continue
            if not isinstance(value, str) or not (value.strip() in COLOR_KEYS or is_color(value)):
                errors.append(f"style.{key}: must be a colour, a palette key name, or null")
            elif value not in COLOR_KEYS and not value.startswith("#") and value.lower() not in TK_NAMED_COLORS:
                warnings.append(
                    f"style.{key}: {value!r} is neither a palette key nor a known colour - "
                    f"palette keys are: {', '.join(COLOR_KEYS)}"
                )

    return errors, warnings


# An optional colour follows the colour it is derived from. This matters when a
# pack overrides the parent but leaves the optional one out: inheriting it from
# the base instead would, for example, give a dark pack built on a light base a
# white table background with its own light text on top of it.
_DERIVED_FROM = {
    "tree_bg": "panel",
    "tab_bg": "panel",
    "tree_heading_bg": "bg",
    "tab_selected_bg": "bg",
    "tree_heading_fg": "text",
}


def _resolve_colors(base: str, overrides: Mapping[str, Any]) -> dict[str, str]:
    """Merge a pack's colours over its base palette with sensible fallbacks."""
    resolved = {k: v for k, v in BUILTIN_PALETTES[base].items()}
    given: set[str] = set()
    for key, value in overrides.items():
        if key in COLOR_KEYS and isinstance(value, str) and is_color(value):
            resolved[key] = value.strip()
            given.add(key)

    # Only keep a base colour the pack did not implicitly replace.
    for key, parent in _DERIVED_FROM.items():
        if key not in given and parent in given:
            resolved[key] = resolved[parent]
    if "accent_text" not in given and "accent" in given:
        resolved["accent_text"] = best_text_on(resolved["accent"])
    if "selection_fg" not in given and "selection" in given:
        resolved["selection_fg"] = best_text_on(resolved["selection"])

    resolved.setdefault("accent_text", "#ffffff")
    resolved.setdefault("selection_fg", "#ffffff")
    resolved.setdefault("tree_heading_fg", resolved.get("text", "#000000"))
    resolved.setdefault("tree_bg", resolved.get("panel", "#ffffff"))
    resolved.setdefault("tree_heading_bg", resolved.get("bg", "#ffffff"))
    resolved.setdefault("tab_bg", resolved.get("panel", "#ffffff"))
    resolved.setdefault("tab_selected_bg", resolved.get("bg", "#ffffff"))
    return resolved


def _resolve_style(base: str, overrides: Mapping[str, Any], colors: Mapping[str, str]) -> dict[str, Any]:
    style = dict(_STYLE_DEFAULTS_BY_BASE[base])
    for key, value in overrides.items():
        if key not in STYLE_KEYS:
            continue
        if key in ("disabled_bg", "disabled_fg", "pressed_bg", "focus_border") and value is None:
            style[key] = None
            continue
        style[key] = value
    return style


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def _looks_like_pack(path: Path) -> bool:
    lowered = path.name.lower()
    return any(lowered.endswith(suffix) for suffix in PACK_SUFFIXES)


def _archive_pack_files(folder: Path) -> list[Path]:
    """Pack files stored inside a zip archive, addressed as pseudo-paths.

    ``iter_pack_files`` cannot use :meth:`Path.iterdir` on a folder that only
    exists inside a ``.pyz``, so the members are listed with :mod:`zipfile` and
    returned as ``<archive>/themes/<pack>.json`` paths.  Everything else in the
    library (reading, updating, reporting) works on those paths unchanged.
    """
    split = _archive_member(folder)
    if split is None:
        return []
    archive, prefix = split
    try:
        if not archive.is_file():
            return []
        with zipfile.ZipFile(archive) as handle:
            names = handle.namelist()
    except (OSError, zipfile.BadZipFile):
        return []

    prefix = prefix.strip("/").lower()
    found: list[Path] = []
    for name in names:
        member = name.replace("\\", "/").strip("/")
        if not member or member.endswith("/"):
            continue
        lowered = member.lower()
        if prefix:
            if not lowered.startswith(prefix + "/"):
                continue
            leaf = lowered[len(prefix) + 1:]
        else:
            leaf = lowered
        if "/" in leaf or not _looks_like_pack(Path(leaf)):
            continue
        found.append(archive / member)
    return sorted(found, key=lambda item: item.name.lower())


def iter_pack_files(dirs: Iterable[Path] | None = None) -> list[Path]:
    """All candidate pack files, in load order (real folders or inside an archive)."""
    folders = list(dirs) if dirs is not None else search_paths()
    found: list[Path] = []
    for folder in folders:
        folder = Path(folder)
        try:
            is_real = folder.is_dir()
        except OSError:  # pragma: no cover - unreadable/odd path
            is_real = False

        if not is_real:
            found.extend(_archive_pack_files(folder))
            continue

        try:
            entries = sorted(folder.iterdir(), key=lambda p: p.name.lower())
        except Exception:
            continue
        for entry in entries:
            try:
                if entry.is_file() and (_looks_like_pack(entry) or entry.name.lower().endswith(".zip")):
                    found.append(entry)
            except Exception:
                continue
    return found


def _read_pack_bytes(path: Path) -> tuple[bytes, str]:
    """Return ``(json_bytes, member_name)`` for a plain pack or a zip bundle."""
    split = _archive_member(path)
    if split is not None and not path.is_file():
        archive, member = split
        try:
            with zipfile.ZipFile(archive) as handle:
                lookup = {n.replace("\\", "/").strip("/").lower(): n for n in handle.namelist()}
                real = lookup.get(member.strip("/").lower())
                if real is None:
                    raise ThemeError(f"{member}: not inside {archive.name}")
                return handle.read(real), member
        except FileNotFoundError as exc:
            raise ThemeError(f"no such file: {archive}") from exc
        except zipfile.BadZipFile as exc:
            raise ThemeError(f"{archive.name}: not a readable zip archive ({exc})") from exc

    if path.name.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            names = [n for n in archive.namelist() if n.lower().endswith(".json") and "/" not in n.strip("/")]
            if not names:
                names = [n for n in archive.namelist() if n.lower().endswith(".json")]
            if not names:
                raise ThemeError("bundle contains no .json theme file")
            preferred = [n for n in names if n.lower().endswith("theme.json")] or names
            member = sorted(preferred)[0]
            return archive.read(member), member
    return path.read_bytes(), path.name


def load_pack_file(path: str | os.PathLike[str], *, trust_id: bool = False) -> Theme:
    """Load and validate one pack file. Raises :class:`ThemeError`."""
    pack_path = Path(path)
    if not pack_path.is_file() and not is_pack_member(pack_path):
        raise ThemeError(f"no such file: {pack_path}")

    try:
        raw_bytes, member = _read_pack_bytes(pack_path)
    except ThemeError:
        raise
    except zipfile.BadZipFile as exc:
        raise ThemeError(f"{pack_path.name}: not a readable zip bundle ({exc})") from exc

    text = raw_bytes.decode("utf-8-sig", errors="replace")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ThemeError(f"{pack_path.name}: invalid JSON at line {exc.lineno}, column {exc.colno}: {exc.msg}") from exc

    errors, _warnings = validate_pack(data, pack_path.name)
    if errors:
        raise ThemeError(f"{pack_path.name}: " + "; ".join(errors))

    return theme_from_pack(data, path=pack_path)


def theme_from_pack(
    data: Mapping[str, Any], *, path: str | os.PathLike[str] | None = None
) -> Theme:
    """Turn an already-decoded pack into a :class:`Theme`.

    Uses the same defaults and fallbacks as :func:`load_pack_file` but touches no
    files, which is what lets the theme editor paint a pack that has never been
    saved. Raises :class:`ThemeError` only for a missing ``id`` or a non-object
    pack - run :func:`validate_pack` first if you want the full story.
    """
    if not isinstance(data, Mapping):
        raise ThemeError("a theme pack must be a JSON object")

    pack_id = str(data.get("id") or "").strip()
    if not pack_id:
        raise ThemeError('id: required, e.g. "my-theme"')

    base = str(data.get("base") or FALLBACK_THEME_ID).strip().lower()
    if base not in BUILTIN_PALETTES:
        base = FALLBACK_THEME_ID

    colors_override = data.get("colors")
    if not isinstance(colors_override, Mapping):
        colors_override = {}
    style_override = data.get("style")
    if not isinstance(style_override, Mapping):
        style_override = {}

    colors = _resolve_colors(base, colors_override)
    style = _resolve_style(base, style_override, colors)

    return Theme(
        id=pack_id,
        name=str(data.get("name") or pack_id).strip(),
        colors=colors,
        style=style,
        font_family=str(data.get("font_family") or BUILTIN_FONTS[base]).strip(),
        base=base,
        author=str(data.get("author", "") or ""),
        version=str(data.get("version", "") or ""),
        description=str(data.get("description", "") or ""),
        license=str(data.get("license", "") or ""),
        homepage=str(data.get("homepage", "") or ""),
        screenshot=str(data.get("screenshot", "") or ""),
        min_app_version=str(data.get("min_app_version", "") or ""),
        kind="pack",
        path=Path(path) if path is not None else None,
        raw=dict(data),
    )


class ThemeRegistry:
    """A load of built-ins plus every pack found on disk."""

    def __init__(self, themes: list[Theme], problems: list[str], dirs: list[Path]):
        self.themes = themes
        self.problems = problems
        self.dirs = dirs
        self._by_key: dict[str, Theme] = {}
        for theme in themes:
            self._by_key.setdefault(theme.id.lower(), theme)
            self._by_key.setdefault(theme.name.lower(), theme)

    # -- lookups -----------------------------------------------------------
    def __len__(self) -> int:
        return len(self.themes)

    def __iter__(self) -> Iterator[Theme]:
        return iter(self.themes)

    def get(self, key: str) -> Theme | None:
        if not isinstance(key, str):
            return None
        return self._by_key.get(key.strip().lower())

    def packs(self) -> list[Theme]:
        return [t for t in self.themes if not t.is_builtin]

    def builtins(self) -> list[Theme]:
        return [t for t in self.themes if t.is_builtin]

    def ids(self) -> list[str]:
        return [t.id for t in self.themes]

    def names(self) -> list[str]:
        return [t.name for t in self.themes]

    def reload(self) -> "ThemeRegistry":
        fresh = load_themes(self.dirs)
        self.themes = fresh.themes
        self.problems = fresh.problems
        self.dirs = fresh.dirs
        self._by_key = fresh._by_key
        return self


def load_themes(
    dirs: Iterable[str | os.PathLike[str]] | None = None,
    *,
    include_builtin: bool = True,
    only_builtin: bool = False,
) -> ThemeRegistry:
    """Load every pack under *dirs* (default: :func:`search_paths`)."""
    if only_builtin:
        return ThemeRegistry([BUILTIN_THEMES[tid] for tid in BUILTIN_ORDER], [], [])

    folders = [Path(d) for d in dirs] if dirs is not None else search_paths()
    themes: list[Theme] = [BUILTIN_THEMES[tid] for tid in BUILTIN_ORDER] if include_builtin else []
    taken: dict[str, Path] = {}
    problems: list[str] = []

    for file_path in iter_pack_files(folders):
        try:
            theme = load_pack_file(file_path)
        except ThemeError as exc:
            problems.append(str(exc))
            continue
        except Exception as exc:  # defensive: never let one bad pack kill the app
            problems.append(f"{file_path.name}: {exc.__class__.__name__}: {exc}")
            continue

        key = theme.id.lower()
        if key in BUILTIN_THEMES:
            problems.append(f"{file_path}: id {theme.id!r} is reserved by a built-in theme - skipped")
            continue
        if key in taken:
            problems.append(
                f"{file_path}: duplicate id {theme.id!r} (already loaded from {taken[key]}) - skipped"
            )
            continue
        taken[key] = file_path
        themes.append(theme)

    packs = sorted((t for t in themes if not t.is_builtin), key=lambda t: (t.name.lower(), t.id))
    ordered = [t for t in themes if t.is_builtin] + packs
    return ThemeRegistry(ordered, problems, folders)


_REGISTRY: ThemeRegistry | None = None


def get_registry(refresh: bool = False) -> ThemeRegistry:
    """Cached registry (the GUI calls this on start-up and on "Reload themes")."""
    global _REGISTRY
    if _REGISTRY is None or refresh:
        _REGISTRY = load_themes()
    return _REGISTRY


def invalidate_registry() -> None:
    """Drop the cache so the next :func:`get_registry` rescans the folders."""
    global _REGISTRY
    _REGISTRY = None


def resolve(theme: str | Theme | None, *, refresh: bool = False) -> Theme:
    """id *or* display name -> :class:`Theme`. Never raises; falls back safely."""
    if isinstance(theme, Theme):
        return theme
    registry = get_registry(refresh=refresh)
    if isinstance(theme, str) and theme.strip():
        found = registry.get(theme)
        if found is not None:
            return found
    fallback = registry.get(FALLBACK_THEME_ID) or BUILTIN_THEMES[FALLBACK_THEME_ID]
    return fallback


def menu_structure(refresh: bool = False) -> list[dict[str, Any]]:
    """Items for the Theme menu: built-ins, a separator, then packs."""
    registry = get_registry(refresh=refresh)
    items: list[dict[str, Any]] = []
    for theme in registry.builtins():
        items.append({"kind": "theme", "id": theme.id, "label": theme.name, "builtin": True})
    packs = registry.packs()
    if packs:
        items.append({"kind": "separator"})
        seen: dict[str, int] = {}
        for theme in packs:
            key = theme.name.lower()
            seen[key] = seen.get(key, 0) + 1
            label = theme.name if seen[key] == 1 else f"{theme.name} ({theme.id})"
            items.append(
                {
                    "kind": "theme",
                    "id": theme.id,
                    "label": label,
                    "builtin": False,
                    "author": theme.author,
                }
            )
    return items


def to_style_kwargs(theme: str | Theme | None, *, refresh: bool = False) -> dict[str, Any]:
    """Everything ``main.py::_apply_style`` needs, as one flat dict."""
    return resolve(theme, refresh=refresh).style_kwargs()


# ---------------------------------------------------------------------------
# Authoring / sharing helpers
# ---------------------------------------------------------------------------

def new_pack(name: str, base: str = FALLBACK_THEME_ID, author: str = "", pack_id: str = "") -> dict[str, Any]:
    """Build a starter pack dict: the base palette, ready to edit."""
    base = (base or FALLBACK_THEME_ID).strip().lower()
    if base not in BUILTIN_THEMES:
        raise ThemeError(f"base must be one of: {', '.join(BUILTIN_ORDER)}")
    clean_name = (name or "").strip()
    if not clean_name:
        raise ThemeError("a theme needs a name")

    if not pack_id:
        slug = re.sub(r"[^a-z0-9]+", "-", clean_name.lower()).strip("-")
        pack_id = slug or "my-theme"
    pack_id = pack_id.strip().lower()
    if not _ID_RE.match(pack_id):
        raise ThemeError(f"{pack_id!r} is not a valid id (use a-z, 0-9, '-', '_', '.')")
    if pack_id in BUILTIN_THEMES:
        raise ThemeError(f"{pack_id!r} is reserved by a built-in theme")

    base_theme = BUILTIN_THEMES[base]
    colors = {k: base_theme.colors[k] for k in COLOR_KEYS if k in base_theme.colors}
    style = dict(base_theme.style)
    return {
        "$schema": SCHEMA_HINT,
        "schema_version": SCHEMA_VERSION,
        "id": pack_id,
        "name": clean_name,
        "author": author or "",
        "version": "1.0.0",
        "description": f"A custom theme for {APP_NAME} based on {BUILTIN_NAMES[base]}.",
        "license": "MIT",
        "homepage": "",
        "screenshot": "",
        "base": base,
        "font_family": base_theme.font_family,
        "colors": colors,
        "style": {k: v for k, v in style.items() if not (isinstance(v, str) and v in colors)},
    }


def export_builtin(theme_id: str, dest: str | os.PathLike[str]) -> Path:
    """Write a built-in theme to disk as a pack (a good way to start an edit)."""
    theme = BUILTIN_THEMES.get((theme_id or "").strip().lower())
    if theme is None:
        raise ThemeError(f"unknown built-in theme {theme_id!r}")
    payload = {"$schema": SCHEMA_HINT, **theme.to_dict()}
    payload.pop("style", None)
    # The built-in ids are reserved, so an exported copy gets a fresh id that
    # still validates - the file is a starting point, not another "graphical".
    new_id = f"{theme.id}-copy"
    payload["id"] = new_id
    payload["name"] = _theme_display_name(new_id)
    payload["description"] = (
        f"A copy of the built-in {theme.name} theme - edit it and make it your own."
    )
    payload["author"] = ""
    payload["version"] = "1.0.0"
    payload["license"] = "MIT"
    target = Path(dest)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return target


def install_pack(source: str | os.PathLike[str], dest_dir: str | os.PathLike[str] | None = None) -> Path:
    """Copy a pack into a themes folder (default ``~/.ipm/themes``)."""
    src = Path(source)
    if not src.is_file():
        raise ThemeError(f"no such file: {src}")
    theme = load_pack_file(src)  # validate before copying

    folder = Path(dest_dir) if dest_dir is not None else ensure_user_theme_dir()
    folder.mkdir(parents=True, exist_ok=True)

    if src.name.lower().endswith(".zip"):
        target = folder / f"{theme.id}.ipmtheme.zip"
    else:
        target = folder / f"{theme.id}.ipmtheme.json"

    try:
        if src.resolve() == target.resolve():
            return target
    except Exception:
        pass
    shutil.copyfile(src, target)
    invalidate_registry()
    return target


def bundle_pack(source: str | os.PathLike[str], dest: str | os.PathLike[str] | None = None) -> Path:
    """Zip a pack plus its screenshot - the easy way to share a theme."""
    src = Path(source)
    theme = load_pack_file(src)
    target = Path(dest) if dest else src.with_name(f"{theme.id}.ipmtheme.zip")

    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        if src.name.lower().endswith(".zip"):
            with zipfile.ZipFile(src) as original:
                for member in original.namelist():
                    if member != "theme.json":
                        archive.writestr(member, original.read(member))
            archive.writestr("theme.json", json.dumps(theme.to_dict(), indent=2, ensure_ascii=False) + "\n")
        else:
            archive.writestr("theme.json", src.read_text(encoding="utf-8-sig"))
        shot = theme.screenshot.strip()
        if shot:
            candidate = (src.parent / shot).resolve()
            try:
                inside = candidate.parent == src.parent.resolve()
            except Exception:
                inside = False
            if candidate.is_file() and inside:
                archive.write(candidate, shot)
    return target


# ---------------------------------------------------------------------------
# Colour maths - the theme editor's toolbox
# ---------------------------------------------------------------------------


def rgb_to_hex(rgb: Sequence[int]) -> str:
    """``(r, g, b)`` -> ``#rrggbb``, clamping out-of-range channels."""
    r, g, b = (max(0, min(255, int(v))) for v in rgb)
    return f"#{r:02x}{g:02x}{b:02x}"


def to_hex(color: Any, default: str = "#000000") -> str:
    """``#rrggbb`` for anything the app can paint, falling back to *default*."""
    rgb = to_rgb(color) or to_rgb(default) or (0, 0, 0)
    return rgb_to_hex(rgb)


def mix(color_a: Any, color_b: Any, amount: float) -> str:
    """Blend *color_a* towards *color_b*: ``0.0`` keeps a, ``1.0`` returns b."""
    ratio = _clamp(float(amount))
    a = to_rgb(color_a) or (0, 0, 0)
    b = to_rgb(color_b) or (0, 0, 0)
    return rgb_to_hex([round(x + (y - x) * ratio) for x, y in zip(a, b)])


def lighten(color: Any, amount: float = 0.1) -> str:
    """Move *color* towards white by *amount* (0 .. 1)."""
    return mix(color, "#ffffff", amount)


def darken(color: Any, amount: float = 0.1) -> str:
    """Move *color* towards black by *amount* (0 .. 1)."""
    return mix(color, "#000000", amount)


def inverted(color: Any) -> str:
    """Photographic negative of a colour."""
    r, g, b = to_rgb(color) or (0, 0, 0)
    return rgb_to_hex([255 - r, 255 - g, 255 - b])


def is_dark(color: Any) -> bool:
    """True when *color* reads as a dark backdrop (white text wins on it)."""
    return (contrast_ratio(color, "#ffffff") or 0.0) >= (contrast_ratio(color, "#000000") or 0.0)


def best_text_on(background: Any, *, light: str = "#ffffff", dark: str = "#101010") -> str:
    """Whichever of two text colours is more readable on *background*."""
    if (contrast_ratio(background, light) or 0.0) >= (contrast_ratio(background, dark) or 0.0):
        return light
    return dark


def _complete_palette(colors: Mapping[str, Any]) -> dict[str, str]:
    """Fill in the derived colours a palette is allowed to leave out.

    Mirrors :func:`_resolve_colors`, so every helper here hands back all
    :data:`COLOR_KEYS` and :func:`contrast_report` can always be trusted.
    """
    resolved: dict[str, str] = {}
    for key, value in colors.items():
        if isinstance(key, str) and isinstance(value, str) and to_rgb(value) is not None:
            resolved[key] = to_hex(value)
    resolved.setdefault("accent_text", best_text_on(resolved.get("accent", "#2563eb")))
    resolved.setdefault("selection_fg", best_text_on(resolved.get("selection", "#2563eb")))
    resolved.setdefault("tree_heading_fg", resolved.get("text", "#000000"))
    resolved.setdefault("tree_bg", resolved.get("panel", "#ffffff"))
    resolved.setdefault("tree_heading_bg", resolved.get("bg", "#ffffff"))
    resolved.setdefault("tab_bg", resolved.get("panel", "#ffffff"))
    resolved.setdefault("tab_selected_bg", resolved.get("bg", "#ffffff"))
    return resolved


def _average_color(colors: Sequence[Any]) -> str:
    rgbs = [rgb for rgb in (to_rgb(color) for color in colors) if rgb is not None]
    if not rgbs:
        return "#808080"
    return rgb_to_hex([round(sum(rgb[i] for rgb in rgbs) / len(rgbs)) for i in range(3)])


def _worst_ratio(color: Any, pairs: Sequence[tuple[str, float]], colors: Mapping[str, str]) -> float:
    """Smallest contrast ratio *color* has against any of its backdrops."""
    ratios: list[float] = []
    for bg_key, _floor in pairs:
        background = colors.get(bg_key)
        if background is None:
            continue
        ratio = contrast_ratio(color, background)
        if ratio is not None:
            ratios.append(ratio)
    return min(ratios) if ratios else 999.0


def _pairs_ok(color: Any, pairs: Sequence[tuple[str, float]], colors: Mapping[str, str]) -> bool:
    for bg_key, floor in pairs:
        background = colors.get(bg_key)
        ratio = contrast_ratio(color, background) if background is not None else None
        if ratio is None or ratio < floor:
            return False
    return True


def _best_nudge(
    start: str, pairs: Sequence[tuple[str, float]], colors: Mapping[str, str], backdrop: str
) -> tuple[str, bool]:
    """Nearest colour to *start* that satisfies every pair, and whether it does."""
    light_first = is_dark(backdrop)
    best, best_score = start, _worst_ratio(start, pairs, colors)
    for direction in (light_first, not light_first):
        for step in range(1, 21):
            amount = step / 20.0
            candidate = lighten(start, amount) if direction else darken(start, amount)
            score = _worst_ratio(candidate, pairs, colors)
            if score > best_score:
                best, best_score = candidate, score
            if _pairs_ok(candidate, pairs, colors):
                return candidate, True
    return best, _pairs_ok(best, pairs, colors)


def nudge_for_contrast(
    color: Any, background: Any, minimum: float, *, lighter: bool | None = None
) -> tuple[str, bool]:
    """Shift *color* away from *background* until the pair reaches *minimum*.

    Returns ``(new_color, reached)``. ``reached`` is ``False`` when even full
    white/black stays below the minimum, in which case the best try comes back.
    """
    start = to_hex(color)
    if not isinstance(minimum, (int, float)) or minimum <= 0:
        return start, True

    target = to_hex(background)
    wanted = float(minimum)
    current = contrast_ratio(start, target) or 0.0
    if current >= wanted:
        return start, True

    light_first = is_dark(target) if lighter is None else bool(lighter)
    best, best_score = start, current
    for direction in (light_first, not light_first):
        for step in range(1, 21):
            amount = step / 20.0
            candidate = lighten(start, amount) if direction else darken(start, amount)
            ratio = contrast_ratio(candidate, target) or 0.0
            if ratio > best_score:
                best, best_score = candidate, ratio
            if ratio >= wanted:
                return candidate, True
    return best, best_score >= wanted


def auto_fix_colors(
    colors: Mapping[str, Any], *, minimum: float | None = None
) -> tuple[dict[str, str], list[str]]:
    """Make every :data:`CONTRAST_PAIRS` pair legible by moving foregrounds only.

    Backgrounds are never touched, so a palette keeps the look its author chose
    and only its text moves. Pass *minimum* to demand more than the standard
    threshold everywhere (``7.0`` is WCAG AAA). Returns ``(colors, notes)``.
    """
    fixed = _complete_palette(colors)

    wanted: dict[str, list[tuple[str, float]]] = {}
    for fg_key, bg_key, pair_min, _severity in CONTRAST_PAIRS:
        floor = float(pair_min if minimum is None else minimum)
        wanted.setdefault(fg_key, []).append((bg_key, floor))

    notes: list[str] = []
    for fg_key in sorted(wanted):
        pairs = [(bg, floor) for bg, floor in wanted[fg_key] if bg in fixed]
        if fg_key not in fixed or not pairs:
            continue
        start = fixed[fg_key]
        if _pairs_ok(start, pairs, fixed):
            continue
        backdrop = _average_color([fixed[bg] for bg, _floor in pairs])
        changed, reached = _best_nudge(start, pairs, fixed, backdrop)
        if changed == start:
            continue
        fixed[fg_key] = changed
        verdict = "fixed" if reached else "best effort"
        notes.append(
            f"{fg_key}: {start} -> {changed} ({verdict}, worst {_worst_ratio(changed, pairs, fixed):.2f}:1)"
        )
    return fixed, notes


def _hsv_hex(hue: float, saturation: float, value: float) -> str:
    r, g, b = colorsys.hsv_to_rgb(hue % 1.0, _clamp(float(saturation)), _clamp(float(value)))
    return rgb_to_hex([round(r * 255), round(g * 255), round(b * 255)])


def palette_from_accent(accent: Any, *, dark: bool = True) -> dict[str, str]:
    """Build a complete, legible palette out of a single accent colour."""
    if to_rgb(accent) is None:
        raise ThemeError(f"{accent!r} is not a colour")
    base = to_hex(accent)

    if dark:
        bg = mix("#0a0e17", base, 0.10)
        panel = mix(bg, "#ffffff", 0.07)
        tree_bg = panel
        heading_bg = bg
        tab_bg = panel
        tab_selected_bg = bg
        border = mix(bg, "#ffffff", 0.20)
        text = mix(bg, "#ffffff", 0.88)
        muted = mix(bg, "#ffffff", 0.55)
        danger = mix(bg, "#f87171", 0.80)
        selection = mix(bg, base, 0.62)
        accent_active = lighten(base, 0.12)
    else:
        bg = mix("#ffffff", base, 0.08)
        panel = "#ffffff"
        tree_bg = "#ffffff"
        heading_bg = mix("#ffffff", base, 0.16)
        tab_bg = mix("#ffffff", base, 0.12)
        tab_selected_bg = "#ffffff"
        border = mix("#ffffff", base, 0.38)
        text = mix("#ffffff", "#0b1020", 0.92)
        muted = mix("#ffffff", "#0b1020", 0.62)
        danger = darken("#dc2626", 0.05)
        selection = mix("#ffffff", base, 0.55)
        accent_active = darken(base, 0.18)

    colors = {
        "bg": bg,
        "panel": panel,
        "text": text,
        "muted": muted,
        "accent": base,
        "accent_active": accent_active,
        "danger": danger,
        "danger_active": darken(danger, 0.15),
        "border": border,
        "selection": selection,
        "tree_bg": tree_bg,
        "tree_heading_bg": heading_bg,
        "tree_heading_fg": text,
        "tab_bg": tab_bg,
        "tab_selected_bg": tab_selected_bg,
        "accent_text": best_text_on(base),
        "selection_fg": best_text_on(selection),
    }
    fixed, _notes = auto_fix_colors(colors)
    return fixed


def random_palette(seed: Any = None, *, dark: bool = True) -> dict[str, str]:
    """A random but always legible palette - the "surprise me" button."""
    rng = random.Random(seed)
    hue = rng.random()
    if dark:
        accent = _hsv_hex(hue, rng.uniform(0.45, 0.80), rng.uniform(0.72, 0.96))
    else:
        accent = _hsv_hex(hue, rng.uniform(0.55, 0.90), rng.uniform(0.45, 0.62))
    return palette_from_accent(accent, dark=dark)


def invert_palette(colors: Mapping[str, Any]) -> dict[str, str]:
    """Flip a palette light<->dark (negating each colour), then auto-fix it."""
    flipped = {key: inverted(value) for key, value in _complete_palette(colors).items()}
    fixed, _notes = auto_fix_colors(flipped)
    return fixed


def grayscale_palette(colors: Mapping[str, Any]) -> dict[str, str]:
    """Drop the hue from a palette while keeping its lightness."""
    grey: dict[str, str] = {}
    for key, value in _complete_palette(colors).items():
        rgb = to_rgb(value)
        if rgb is None:
            continue
        luminance = round(0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2])
        grey[key] = rgb_to_hex([luminance, luminance, luminance])
    fixed, _notes = auto_fix_colors(grey)
    return fixed


def _movable_background(
    background: Any, minimum: float, *, light: str = "#ffffff", dark: str = "#101010"
) -> tuple[str, str]:
    """Return ``(background, text)`` where the label clears *minimum*.

    Used for accents and selections, which carry a plain black-or-white label.
    A mid-grey backdrop is a dead zone: neither pure white nor pure black can
    reach a high target on it, so the backdrop itself has to move. The smallest
    shift that works wins, and the hue survives because the moves are gradual.
    """
    bg = to_hex(background)
    wanted = float(minimum) if isinstance(minimum, (int, float)) and minimum > 0 else 7.0

    for candidate in (dark, light) if is_dark(bg) else (light, dark):
        if (contrast_ratio(candidate, bg) or 0.0) >= wanted:
            return bg, candidate

    best: tuple[int, str, str] | None = None
    for direction in (False, True):
        for step in range(1, 21):
            amount = step / 20.0
            moved = lighten(bg, amount) if direction else darken(bg, amount)
            label = best_text_on(moved, light=light, dark=dark)
            if (contrast_ratio(label, moved) or 0.0) >= wanted:
                if best is None or step < best[0]:
                    best = (step, moved, label)
                break
    if best is None:
        # Cannot happen in practice (pure black or white always clears the
        # target against its opposite label), but never return a broken pair.
        moved = dark if is_dark(bg) else light
        return moved, best_text_on(moved, light=light, dark=dark)
    return best[1], best[2]


def high_contrast_palette(colors: Mapping[str, Any], *, minimum: float = 7.0) -> dict[str, str]:
    """Push every foreground to at least *minimum* contrast (7:1 is WCAG AAA)."""
    base = _complete_palette(colors)
    wanted = float(minimum) if isinstance(minimum, (int, float)) and minimum > 0 else 7.0
    for bg_key, fg_key in (("accent", "accent_text"), ("selection", "selection_fg")):
        moved, label = _movable_background(base[bg_key], wanted)
        base[bg_key], base[fg_key] = moved, label
    fixed, _notes = auto_fix_colors(base, minimum=wanted)
    return fixed


def palette_sources(refresh: bool = False) -> dict[str, dict[str, str]]:
    """``display name -> palette`` for every theme the app can currently see."""
    sources: dict[str, dict[str, str]] = {}
    for theme_id in BUILTIN_ORDER:
        theme = BUILTIN_THEMES[theme_id]
        sources[theme.name] = dict(theme.colors)
    for theme in get_registry(refresh=refresh).themes:
        if theme.is_builtin:
            continue
        sources.setdefault(theme.label, dict(theme.colors))
    return sources


# ---------------------------------------------------------------------------
# Terminal preview
# ---------------------------------------------------------------------------

_RESET = "\033[0m"


def _ansi(rgb: tuple[int, int, int], *, background: bool) -> str:
    r, g, b = rgb
    return f"\033[{'48' if background else '38'};2;{r};{g};{b}m"


def _supports_truecolor(stream: Any) -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if os.environ.get("COLORTERM", "").lower() in ("truecolor", "24bit"):
        return True
    if os.environ.get("WT_SESSION"):
        return True
    try:
        return bool(stream.isatty())
    except Exception:
        return False


def ansi_preview(theme: str | Theme | None, *, color: bool | None = None, width: int = 52) -> str:
    """A little mock window in the theme's colours, for the terminal.

    ``color=True`` forces escape codes (used by the tests); ``None`` decides from
    the terminal.
    """
    resolved = resolve(theme)
    use_color = _supports_truecolor(sys.stdout) if color is None else bool(color)
    c = resolved.colors

    def paint(text: str, fg: str | None = None, bg: str | None = None) -> str:
        if not use_color:
            return text
        prefix = ""
        rgb_bg = to_rgb(bg) if bg else None
        rgb_fg = to_rgb(fg) if fg else None
        if rgb_bg:
            prefix += _ansi(rgb_bg, background=True)
        if rgb_fg:
            prefix += _ansi(rgb_fg, background=False)
        return f"{prefix}{text}{_RESET}" if prefix else text

    lines: list[str] = []
    title = f"{APP_NAME} V{APP_VERSION}"
    lines.append(paint(f" {title}".ljust(width), c["text"], c["bg"]))
    lines.append(paint(" " + "-" * (width - 1), c["muted"], c["bg"]))
    tab = "  Local   Internet   Settings   Logs  "
    lines.append(paint(tab.ljust(width), c["text"], c["tab_selected_bg"]))
    lines.append(paint(" " + " " * (width - 1), c["text"], c["bg"]))
    row = "  [ Search ]   [ Stop ]        Ubuntu 24.04 ISO"
    lines.append(paint(row.ljust(width), c["text"], c["panel"]))
    row2 = "  filename.iso          4.6 GB   Ready"
    lines.append(paint(row2.ljust(width), c["muted"], c["panel"]))
    lines.append(paint(" " + " " * (width - 1), c["text"], c["bg"]))
    lines.append(paint("  [ Download ]", c["accent_text"], c["accent"]))
    lines.append(paint("  [ Delete   ]", c["selection_fg"], c["selection"]))

    header = f"{resolved.name}"
    if resolved.author:
        header += f"  by {resolved.author}"
    if resolved.version:
        header += f"  v{resolved.version}"
    header += f"   [{resolved.id}]"
    if resolved.is_builtin:
        header += "  (built-in)"
    out = [header, ""]
    out.extend(lines)
    out.append("")
    swatches = []
    for key in ("bg", "panel", "text", "muted", "accent", "accent_active", "danger", "border", "selection"):
        value = c.get(key, "")
        rgb = to_rgb(value)
        if use_color and rgb:
            swatches.append(paint("   ", None, value) + f" {key} {value}")
        else:
            swatches.append(f" {value:<9} {key}")
    out.append("  " + "   ".join(swatches[:5]))
    out.append("  " + "   ".join(swatches[5:]))
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Runtime helpers for the GUI
# ---------------------------------------------------------------------------

def tk_color_is_valid(widget: Any, value: Any) -> bool:
    """Ask Tk itself whether a colour is usable (catches typos in names)."""
    if not is_color(value):
        return False
    if not value.strip().startswith("#"):
        try:
            widget.winfo_rgb(value)
            return True
        except Exception:
            return False
    return True


def missing_extras() -> list[str]:
    """Names of optional modules that are absent (used for friendly messages)."""
    missing: list[str] = []
    try:
        import tkinter  # noqa: F401
    except Exception:
        missing.append("tkinter")
    return missing
