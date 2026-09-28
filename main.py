import os
import sys
import threading
import queue
import time
import hashlib
import shutil
import subprocess
import re
import urllib.request
import urllib.parse
import urllib.error
import ssl
import json
import tempfile
from datetime import datetime
from pathlib import Path
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog
from typing import Callable

try:
    import webview  # type: ignore
except Exception:
    webview = None


from ipm_http import http_get_text
from ipm_ia import IA_SOURCES, LICENSE_NOTE, ia_iso_search, is_ia_source
from ipm_models import ISO_EXTENSIONS, IsoItem, RemoteIsoItem
from ipm_search import (
    archive_search_all,
    duckduckgo_iso_search,
    google_cse_iso_search,
    has_archive_support,
    searxng_iso_search,
    web_search_iso_urls,
)
from ipm_utils import (
    build_iso_focused_query,
    extract_iso_urls_from_text,
    human_bytes,
    infer_version_from_iso_name,
    is_plausible_iso_download_url,
    is_windows,
    join_url,
    open_path_default,
    open_url_default,
    parse_apache_listing_for_links,
    parse_checksum_lines,
    platform_label,
    seven_zip_hint,
    sha256_file,
    ssl_context_for_https,
    which_7z,
)
from ipm_winops import (
    get_mount_point,
    mount_backend,
    mount_iso,
    mount_supported,
    open_explorer,
    run_powershell,
    unmount_iso,
)
from ipm_windows import (
    SERVER_SOURCES,
    WINDOWS_SOURCES,
    is_windows_source,
    windows_iso_search,
)


# ---------------------------------------------------------------------------
# Theme packs (ipm_themes.py) - optional, additive
# ---------------------------------------------------------------------------
# When ipm_themes.py sits next to this file, ISO Package Manager also loads
# themes that users install themselves (.ipmtheme.json files). If the module is
# missing, the four built-in themes below keep working exactly as before.
try:
    import ipm_themes as _ipm_themes
except Exception:  # pragma: no cover - theme packs are optional
    _ipm_themes = None

APP_VERSION = "0.10.1"
try:  # the launcher owns the version number; this is only a fallback
    from ipm_launcher import APP_VERSION as APP_VERSION  # noqa: F811
except Exception:  # pragma: no cover - running without the launcher
    pass

try:  # the in-app Theme Shop is optional as well
    import ipm_shop as _ipm_shop
except Exception:  # pragma: no cover
    _ipm_shop = None


def _ipm_theme_menu_entries() -> list:
    """Menu labels: the built-ins, then a separator, then installed packs."""
    builtins = ["Default", "Modern Dark", "Windows XP", "Graphical"]
    if _ipm_themes is None:
        return builtins
    try:
        packs = [t.name for t in _ipm_themes.get_registry().packs()]
    except Exception:
        packs = []
    if not packs:
        return builtins
    return builtins + [None] + packs


def _hex_to_rgb(value: str) -> tuple[int, int, int]:
    value = (value or "").strip().lstrip("#")
    if len(value) == 3:
        value = "".join(ch * 2 for ch in value)
    if len(value) != 6:
        return (0, 0, 0)
    try:
        return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))  # type: ignore[return-value]
    except ValueError:
        return (0, 0, 0)


def _mix(first: str, second: str, amount: float) -> str:
    """``amount`` 0 -> first, 1 -> second."""
    a, b = _hex_to_rgb(first), _hex_to_rgb(second)
    out = tuple(round(a[i] + (b[i] - a[i]) * max(0.0, min(1.0, amount))) for i in range(3))
    return "#%02x%02x%02x" % out


def _is_dark(value: str) -> bool:
    r, g, b = _hex_to_rgb(value)
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) < 128


def _use_dark_title_bar(window, dark: bool) -> None:
    """Windows 10/11: paint the title bar to match the theme. No-op elsewhere."""
    if not is_windows():
        return
    try:
        import ctypes
        window.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(window.winfo_id())
        value = ctypes.c_int(1 if dark else 0)
        for attribute in (20, 19):  # 20 = Win10 20H1+, 19 = earlier builds
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(
                hwnd, attribute, ctypes.byref(value), ctypes.sizeof(value)
            ) == 0:
                break
    except Exception:
        pass


def _ipm_theme_labels() -> list:
    """The same labels without the separator (for the Settings combobox)."""
    return [label for label in _ipm_theme_menu_entries() if label]


def _ipm_pack_kwargs(theme: str):
    """Flat palette/style dict for an installed pack, else ``None``.

    Returns ``None`` for the built-in themes so the hand-written palettes in
    ``_apply_style`` stay the single source of truth for them.
    """
    if _ipm_themes is None or not theme:
        return None
    try:
        found = _ipm_themes.get_registry().get(theme)
        if found is None or found.is_builtin:
            return None
        return found.style_kwargs()
    except Exception:
        return None


def can_open_in_app_webview() -> bool:
    return webview is not None


_UI_TRANSLATIONS: dict[str, dict[str, str]] = {
    "German": {
        "Theme": "Thema",
        "Language": "Sprache",
        "Local": "Lokal",
        "Internet": "Internet",
        "Local ISOs": "Lokale ISOs",
        "Folders": "Ordner",
        "Add Folder": "Ordner hinzufügen",
        "Remove Selected": "Auswahl entfernen",
        "Scan": "Scannen",
        "Stop": "Stopp",
        "Mount + Open": "Einbinden + Öffnen",
        "Eject ISO": "ISO auswerfen",
        "Show Details": "Details anzeigen",
        "Extract": "Extrahieren",
        "Copy Path": "Pfad kopieren",
        "Internet Sources": "Internetquellen",
        "Source": "Quelle",
        "Category:": "Kategorie:",
        "Source:": "Quelle:",
        "ISO directory URL:": "ISO-Verzeichnis-URL:",
        "Checksum URL (optional):": "Prüfsummen-URL (optional):",
        "Web Search:": "Websuche:",
        "Mode:": "Modus:",
        "Provider:": "Anbieter:",
        "SearxNG URL:": "SearxNG-URL:",
        "Google API key:": "Google-API-Schlüssel:",
        "Google CSE ID (cx):": "Google CSE-ID (cx):",
        "Search Web": "Web suchen",
        "Refresh List": "Liste aktualisieren",
        "Open Source Page": "Quellseite öffnen",
        "Download URL": "URL herunterladen",
        "Audit Sources": "Quellen prüfen",
        "Search:": "Suche:",
        "Name": "Name",
        "Path": "Pfad",
        "Size": "Größe",
        "Modified": "Geändert",
        "URL": "URL",
        "SHA-256": "SHA-256",
        "Pick a source and click Refresh": "Quelle auswählen und auf Aktualisieren klicken",
        "Load more": "Mehr laden",
        "Download": "Herunterladen",
        "Copy URL": "URL kopieren",
        "Validate": "Prüfen",
        "Clear": "Leeren",
        "Distro:": "Distro:",
        "Ready": "Bereit",
    }
    ,
    "Spanish": {
        "Theme": "Tema",
        "Language": "Idioma",
        "Local": "Local",
        "Internet": "Internet",
        "Local ISOs": "ISOs locales",
        "Folders": "Carpetas",
        "Add Folder": "Añadir carpeta",
        "Remove Selected": "Eliminar selección",
        "Scan": "Escanear",
        "Stop": "Detener",
        "Mount + Open": "Montar + abrir",
        "Eject ISO": "Expulsar ISO",
        "Show Details": "Mostrar detalles",
        "Extract": "Extraer",
        "Copy Path": "Copiar ruta",
        "Internet Sources": "Fuentes de Internet",
        "Source": "Fuente",
        "Category:": "Categoría:",
        "Source:": "Fuente:",
        "ISO directory URL:": "URL del directorio ISO:",
        "Checksum URL (optional):": "URL de checksum (opcional):",
        "Web Search:": "Búsqueda web:",
        "Mode:": "Modo:",
        "Provider:": "Proveedor:",
        "SearxNG URL:": "URL de SearxNG:",
        "Google API key:": "Clave API de Google:",
        "Google CSE ID (cx):": "ID de Google CSE (cx):",
        "Search Web": "Buscar en la web",
        "Refresh List": "Actualizar lista",
        "Open Source Page": "Abrir página de origen",
        "Download URL": "Descargar URL",
        "Audit Sources": "Auditar fuentes",
        "Search:": "Buscar:",
        "Name": "Nombre",
        "Path": "Ruta",
        "Size": "Tamaño",
        "Modified": "Modificado",
        "URL": "URL",
        "SHA-256": "SHA-256",
        "Pick a source and click Refresh": "Elige una fuente y haz clic en Actualizar",
        "Load more": "Cargar más",
        "Download": "Descargar",
        "Copy URL": "Copiar URL",
        "Validate": "Validar",
        "Clear": "Limpiar",
        "Distro:": "Distro:",
        "Ready": "Listo",
    },
    "French": {
        "Theme": "Thème",
        "Language": "Langue",
        "Local": "Local",
        "Internet": "Internet",
        "Local ISOs": "ISOs locaux",
        "Folders": "Dossiers",
        "Add Folder": "Ajouter un dossier",
        "Remove Selected": "Supprimer la sélection",
        "Scan": "Analyser",
        "Stop": "Arrêter",
        "Mount + Open": "Monter + ouvrir",
        "Eject ISO": "Éjecter l'ISO",
        "Show Details": "Afficher les détails",
        "Extract": "Extraire",
        "Copy Path": "Copier le chemin",
        "Internet Sources": "Sources Internet",
        "Source": "Source",
        "Category:": "Catégorie :",
        "Source:": "Source :",
        "ISO directory URL:": "URL du répertoire ISO :",
        "Checksum URL (optional):": "URL de somme de contrôle (optionnel) :",
        "Web Search:": "Recherche Web :",
        "Mode:": "Mode :",
        "Provider:": "Fournisseur :",
        "SearxNG URL:": "URL SearxNG :",
        "Google API key:": "Clé API Google :",
        "Google CSE ID (cx):": "ID Google CSE (cx) :",
        "Search Web": "Rechercher sur le Web",
        "Refresh List": "Actualiser la liste",
        "Open Source Page": "Ouvrir la page source",
        "Download URL": "Télécharger l’URL",
        "Audit Sources": "Auditer les sources",
        "Search:": "Rechercher :",
        "Name": "Nom",
        "Path": "Chemin",
        "Size": "Taille",
        "Modified": "Modifié",
        "URL": "URL",
        "SHA-256": "SHA-256",
        "Pick a source and click Refresh": "Choisissez une source puis cliquez sur Actualiser",
        "Load more": "Charger plus",
        "Download": "Télécharger",
        "Copy URL": "Copier l’URL",
        "Validate": "Vérifier",
        "Clear": "Effacer",
        "Distro:": "Distro :",
        "Ready": "Prêt",
    },
    "Russian": {
        "Theme": "Тема",
        "Language": "Язык",
        "Local": "Локально",
        "Internet": "Интернет",
        "Local ISOs": "Локальные ISO",
        "Folders": "Папки",
        "Add Folder": "Добавить папку",
        "Remove Selected": "Удалить выбранное",
        "Scan": "Сканировать",
        "Stop": "Стоп",
        "Mount + Open": "Смонтировать + открыть",
        "Eject ISO": "Извлечь ISO",
        "Show Details": "Показать детали",
        "Extract": "Извлечь",
        "Copy Path": "Копировать путь",
        "Internet Sources": "Источники в интернете",
        "Source": "Источник",
        "Category:": "Категория:",
        "Source:": "Источник:",
        "ISO directory URL:": "URL каталога ISO:",
        "Checksum URL (optional):": "URL контрольной суммы (необязательно):",
        "Web Search:": "Веб-поиск:",
        "Mode:": "Режим:",
        "Provider:": "Провайдер:",
        "SearxNG URL:": "URL SearxNG:",
        "Google API key:": "Ключ API Google:",
        "Google CSE ID (cx):": "ID Google CSE (cx):",
        "Search Web": "Искать в интернете",
        "Refresh List": "Обновить список",
        "Open Source Page": "Открыть страницу источника",
        "Download URL": "Скачать URL",
        "Audit Sources": "Проверить источники",
        "Search:": "Поиск:",
        "Name": "Имя",
        "Path": "Путь",
        "Size": "Размер",
        "Modified": "Изменено",
        "URL": "URL",
        "SHA-256": "SHA-256",
        "Pick a source and click Refresh": "Выберите источник и нажмите «Обновить»",
        "Load more": "Загрузить ещё",
        "Download": "Скачать",
        "Copy URL": "Копировать URL",
        "Validate": "Проверить",
        "Clear": "Очистить",
        "Distro:": "Дистрибутив:",
        "Ready": "Готово",
    },
    "Portuguese": {
        "Theme": "Tema",
        "Language": "Idioma",
        "Local": "Local",
        "Internet": "Internet",
        "Local ISOs": "ISOs locais",
        "Folders": "Pastas",
        "Add Folder": "Adicionar pasta",
        "Remove Selected": "Remover selecionados",
        "Scan": "Verificar",
        "Stop": "Parar",
        "Mount + Open": "Montar + abrir",
        "Eject ISO": "Ejetar ISO",
        "Show Details": "Mostrar detalhes",
        "Extract": "Extrair",
        "Copy Path": "Copiar caminho",
        "Internet Sources": "Fontes da Internet",
        "Source": "Fonte",
        "Category:": "Categoria:",
        "Source:": "Fonte:",
        "ISO directory URL:": "URL do diretório ISO:",
        "Checksum URL (optional):": "URL do checksum (opcional):",
        "Web Search:": "Pesquisa na web:",
        "Mode:": "Modo:",
        "Provider:": "Provedor:",
        "SearxNG URL:": "URL do SearxNG:",
        "Google API key:": "Chave da API do Google:",
        "Google CSE ID (cx):": "ID do Google CSE (cx):",
        "Search Web": "Pesquisar na web",
        "Refresh List": "Atualizar lista",
        "Open Source Page": "Abrir página de origem",
        "Download URL": "Baixar URL",
        "Audit Sources": "Auditar fontes",
        "Search:": "Pesquisar:",
        "Name": "Nome",
        "Path": "Caminho",
        "Size": "Tamanho",
        "Modified": "Modificado",
        "URL": "URL",
        "SHA-256": "SHA-256",
        "Pick a source and click Refresh": "Escolha uma fonte e clique em Atualizar",
        "Load more": "Carregar mais",
        "Download": "Baixar",
        "Copy URL": "Copiar URL",
        "Validate": "Validar",
        "Clear": "Limpar",
        "Distro:": "Distro:",
        "Ready": "Pronto",
    },
    "Chinese (Simplified)": {
        "Theme": "主题",
        "Language": "语言",
        "Local": "本地",
        "Internet": "互联网",
        "Local ISOs": "本地 ISO",
        "Folders": "文件夹",
        "Add Folder": "添加文件夹",
        "Remove Selected": "删除所选",
        "Scan": "扫描",
        "Stop": "停止",
        "Mount + Open": "挂载并打开",
        "Eject ISO": "弹出 ISO",
        "Show Details": "显示详情",
        "Extract": "解压",
        "Copy Path": "复制路径",
        "Internet Sources": "互联网来源",
        "Source": "来源",
        "Category:": "类别:",
        "Source:": "来源:",
        "ISO directory URL:": "ISO 目录 URL:",
        "Checksum URL (optional):": "校验和 URL（可选）:",
        "Web Search:": "网页搜索:",
        "Mode:": "模式:",
        "Provider:": "提供方:",
        "SearxNG URL:": "SearxNG URL:",
        "Google API key:": "Google API 密钥:",
        "Google CSE ID (cx):": "Google CSE ID (cx):",
        "Search Web": "搜索网页",
        "Refresh List": "刷新列表",
        "Open Source Page": "打开来源页面",
        "Download URL": "下载 URL",
        "Audit Sources": "审计来源",
        "Search:": "搜索:",
        "Name": "名称",
        "Path": "路径",
        "Size": "大小",
        "Modified": "修改时间",
        "URL": "URL",
        "SHA-256": "SHA-256",
        "Pick a source and click Refresh": "选择来源并点击刷新",
        "Load more": "加载更多",
        "Download": "下载",
        "Copy URL": "复制 URL",
        "Validate": "校验",
        "Clear": "清除",
        "Distro:": "发行版:",
        "Ready": "就绪",
    },
}


def _self_invocation_prefix() -> list[str] | None:
    """argv prefix that re-runs this program, or None when it cannot be found.

    Handles the three ways the app ships:

    * a plain source tree          -> ``python /path/main.py``
    * a PyInstaller onefile binary -> ``/path/ISO Package Manager``
    * a zipapp (``.pyz``)          -> ``python /path/iso-package-manager.pyz``

    The zipapp case is why ``__file__`` alone is not enough: inside a zip
    archive it points at ``archive.pyz/main.py``, which is not a real file
    on disk, so the helper process has to be launched through the archive
    itself (``sys.argv[0]``).
    """
    if getattr(sys, "frozen", False):
        # Single-file build: the executable is the entry point, the module
        # path inside the bundle does not exist on disk.
        return [sys.executable]

    candidates: list[str] = []
    try:
        candidates.append(os.path.abspath(__file__))
    except Exception:
        pass
    argv0 = (sys.argv[0] or "") if sys.argv else ""
    if argv0 and not argv0.startswith("-"):
        candidates.append(os.path.abspath(argv0))

    for cand in candidates:
        if cand and os.path.isfile(cand):
            return [sys.executable, cand]
    return None


def open_url_in_app(url: str, title: str = "Browser", ipc_path: str | None = None) -> bool:
    if webview is None:
        return False

    try:
        prefix = _self_invocation_prefix()
        if prefix is None:
            return False
        args = prefix + ["--webview", url, "--title", title]
        if ipc_path:
            args.extend(["--ipc", ipc_path])
        kwargs: dict = {
            "stdout": subprocess.DEVNULL,
            "stderr": subprocess.DEVNULL,
        }
        if os.name == "nt":
            # Keep the helper process from flashing an extra console window.
            kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)
        subprocess.Popen(args, **kwargs)
        return True
    except Exception:
        return False


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"ISO Package Manager V{APP_VERSION}")
        want_w, want_h = 980, 640
        try:
            want_w = max(720, min(want_w, self.winfo_screenwidth() - 80))
            want_h = max(480, min(want_h, self.winfo_screenheight() - 120))
        except Exception:
            pass
        self.geometry(f"{want_w}x{want_h}")
        self.minsize(680, 460)

        self._style = ttk.Style()

        self._theme_var = tk.StringVar(value="Modern Dark")
        self._lang_var = tk.StringVar(value="English")
        self._icons: dict[str, tk.PhotoImage] = {}

        self._settings_path = Path.home() / ".ipm_settings.json"
        self._settings: dict[str, object] = {}
        self._load_settings_file()

        try:
            g = self._settings.get("geometry")
            if isinstance(g, str) and g.strip():
                self._restore_geometry(g)
        except Exception:
            pass

        self._build_menu()
        self._apply_style(self._theme_var.get())

        self._scan_thread = None
        self._stop_scan = threading.Event()
        self._work_q: queue.Queue = queue.Queue()

        self._hash_thread = None
        self._stop_hash = threading.Event()

        self._download_thread = None
        self._stop_download = threading.Event()
        self._download_queue: queue.Queue = queue.Queue()
        self._download_queue_lock = threading.Lock()
        self._download_queue_count = 0
        self._download_jobs: dict[str, dict] = {}
        self._download_job_seq = 0
        self._download_manager_stop = threading.Event()
        self._download_active = threading.Event()
        self._download_manager_thread = None

        self._validate_thread = None
        self._stop_validate = threading.Event()

        self._audit_thread = None
        self._stop_audit = threading.Event()

        self._items: list[IsoItem] = []
        self._remote_items: list[RemoteIsoItem] = []
        self._local_meta_cache: dict[str, str] = {}

        self._build_ui()
        self._apply_loaded_settings_to_ui_vars()
        try:
            self._apply_style(self._theme_var.get())
        except Exception:
            pass
        self._apply_language()
        self._retheme_widgets()
        self._poll_queue()

        self._download_manager_thread = threading.Thread(target=self._download_manager_worker, daemon=True)
        self._download_manager_thread.start()

        try:
            self._load_download_jobs()
        except Exception:
            pass

        try:
            self.protocol("WM_DELETE_WINDOW", self._on_close)
        except Exception:
            pass

        # Default folders: Desktop + Downloads (or restored from settings)
        restored = self._settings.get("folders")
        if isinstance(restored, list) and restored:
            for p in restored:
                try:
                    s = str(p).strip()
                    if s:
                        self._folders_list.insert(tk.END, s)
                except Exception:
                    continue
        else:
            home = Path.home()
            defaults = []
            for p in (home / "Desktop", home / "Downloads"):
                if p.exists():
                    defaults.append(p)
            for p in defaults:
                self._folders_list.insert(tk.END, str(p))

    def _build_menu(self):
        menubar = tk.Menu(self)
        theme_menu = tk.Menu(menubar, tearoff=0)
        for label in _ipm_theme_menu_entries():
            if label is None:
                theme_menu.add_separator()
                continue
            theme_menu.add_radiobutton(
                label=label,
                value=label,
                variable=self._theme_var,
                command=lambda v=label: self._set_theme(v),
            )
        theme_menu.add_separator()
        theme_menu.add_command(label=self._t("Get more themes..."), command=self._open_theme_shop)
        theme_menu.add_command(label=self._t("Reload themes"), command=self._reload_themes)
        menubar.add_cascade(label=self._t("Theme"), menu=theme_menu)

        lang_menu = tk.Menu(menubar, tearoff=0)
        for label in ("English", "German", "Spanish", "French", "Russian", "Portuguese", "Chinese (Simplified)"):
            lang_menu.add_radiobutton(
                label=label,
                value=label,
                variable=self._lang_var,
                command=lambda v=label: self._set_language(v),
            )
        menubar.add_cascade(label=self._t("Language"), menu=lang_menu)
        self.configure(menu=menubar)
        self._paint_menus()

    # Labels and plain frames that live on a card must use the card colour.
    _PANEL_FRAME_STYLES = ("Panel.TFrame", "Card.TFrame")
    _PANEL_LABEL_MAP = {
        "": "Panel.TLabel",
        "TLabel": "Panel.TLabel",
        "Muted.TLabel": "PanelMuted.TLabel",
        "Title.TLabel": "PanelTitle.TLabel",
    }

    def _normalize_panel_backgrounds(self, widget=None, on_panel: bool = False) -> None:
        """Walk the widget tree and carry the panel colour down into it.

        Tk has no inheritance for ttk backgrounds, so a plain frame or a label
        dropped inside a card keeps the window colour and shows up as a box.
        This walks once after the UI is built and hands every child the right
        style, which means new widgets never have to remember to ask for it.
        """
        if widget is None:
            widget = self
            on_panel = False

        try:
            children = widget.winfo_children()
        except Exception:
            return

        for child in children:
            child_on_panel = on_panel
            try:
                cls = child.winfo_class()
            except Exception:
                continue

            if cls in ("TFrame", "TLabelframe"):
                try:
                    style = str(child.cget("style") or "")
                except Exception:
                    style = ""
                if style in self._PANEL_FRAME_STYLES:
                    child_on_panel = True
                elif on_panel and style in ("", "TFrame"):
                    # A bare container inside a card: give it the card colour.
                    try:
                        child.configure(style="Panel.TFrame")
                    except Exception:
                        pass
            elif cls == "TNotebook":
                child_on_panel = False
            elif cls == "TLabel" and on_panel:
                try:
                    style = str(child.cget("style") or "")
                except Exception:
                    style = ""
                target = self._PANEL_LABEL_MAP.get(style)
                if target:
                    try:
                        child.configure(style=target)
                    except Exception:
                        pass

            self._normalize_panel_backgrounds(child, child_on_panel)

    def _stripe_rows(self, tree) -> None:
        """Alternate row colours so long lists stay readable."""
        colors = getattr(self, "_colors", {})
        odd = colors.get("row_alt")
        if not odd:
            return
        try:
            tree.tag_configure("ipm_odd", background=odd)
            tree.tag_configure("ipm_even", background=colors.get("tree_bg", colors.get("panel", "")))
            for index, row in enumerate(tree.get_children()):
                tree.item(row, tags=("ipm_odd" if index % 2 else "ipm_even",))
        except Exception:
            pass

    def _paint_menus(self) -> None:
        """Give the menu bar and its drop-downs the palette colours."""
        colors = getattr(self, "_menu_colors", None)
        if not colors:
            return

        def paint(widget) -> None:
            try:
                widget.configure(**colors)
            except Exception:
                try:                       # older Tk: skip what it does not know
                    widget.configure(background=colors["background"], foreground=colors["foreground"])
                except Exception:
                    return
            try:
                end = widget.index("end")
            except Exception:
                end = None
            if end is None:
                return
            for index in range(end + 1):
                try:
                    if widget.type(index) == "cascade":
                        name = widget.entrycget(index, "menu")
                        child = widget.nametowidget(name) if name else None
                        if child is not None:
                            paint(child)
                except Exception:
                    continue

        try:
            name = self.cget("menu")
            if name:
                paint(self.nametowidget(name))
        except Exception:
            pass

    def _t(self, text: str) -> str:
        lang = (getattr(self, "_lang_var", None).get() if hasattr(self, "_lang_var") else "English")
        if not lang or lang == "English":
            return text
        return _UI_TRANSLATIONS.get(lang, {}).get(text, text)

    def _set_language(self, lang: str) -> None:
        self._lang_var.set(lang)
        self._build_menu()
        self._apply_language()

    def _apply_language(self) -> None:
        def is_any_translation(value: str, key: str) -> bool:
            if value == key:
                return True
            for _lang, mapping in _UI_TRANSLATIONS.items():
                try:
                    if mapping.get(key) == value:
                        return True
                except Exception:
                    continue
            return False

        try:
            if hasattr(self, "_notebook") and self._notebook.winfo_exists():
                self._notebook.tab(self._tab_local, text=self._t("Local"))
                self._notebook.tab(self._tab_internet, text=self._t("Internet"))
                if hasattr(self, "_tab_settings"):
                    self._notebook.tab(self._tab_settings, text=self._t("Settings"))
                if hasattr(self, "_tab_logs"):
                    self._notebook.tab(self._tab_logs, text=self._t("Logs"))
        except Exception:
            pass

        def set_text(attr: str, value: str) -> None:
            w = getattr(self, attr, None)
            if not w:
                return
            try:
                if w.winfo_exists():
                    w.configure(text=value)
            except Exception:
                pass

        set_text("_lbl_local_header", self._t("Local ISOs"))
        set_text("_lbl_folders", self._t("Folders"))
        set_text("_lbl_filter_distro", self._t("Distro:"))
        set_text("_lbl_filter_search", self._t("Search:"))
        set_text("_btn_clear_filter", self._t("Clear"))

        set_text("_btn_add_folder", self._t("Add Folder"))
        set_text("_btn_remove_folder", self._t("Remove Selected"))
        set_text("_btn_scan", self._t("Scan"))
        set_text("_btn_stop_scan", self._t("Stop"))
        set_text("_btn_mount_open", self._t("Mount + Open"))
        set_text("_btn_details", self._t("Show Details"))
        set_text("_btn_dupes", self._t("Find Duplicates"))
        set_text("_btn_extract", self._t("Extract"))
        set_text("_btn_copy", self._t("Copy Path"))
        set_text("_btn_eject", self._t("Eject ISO"))

        set_text("_lbl_internet_header", self._t("Internet Sources"))
        set_text("_lbl_net_source", self._t("Source"))
        set_text("_lbl_net_category", self._t("Category:"))
        set_text("_lbl_net_source2", self._t("Source:"))
        set_text("_lbl_net_iso_dir", self._t("ISO directory URL:"))
        set_text("_lbl_net_checksum", self._t("Checksum URL (optional):"))
        set_text("_lbl_net_web_search", self._t("Web Search:"))
        set_text("_lbl_net_mode", self._t("Mode:"))
        set_text("_lbl_net_provider", self._t("Provider:"))
        set_text("_lbl_settings_header", self._t("Settings"))
        set_text("_lbl_settings_provider", self._t("Provider:"))
        set_text("_lbl_settings_searx", self._t("SearxNG URL:"))
        set_text("_lbl_settings_gkey", self._t("Google API key:"))
        set_text("_lbl_settings_gcx", self._t("Google CSE ID (cx):"))
        set_text("_lbl_settings_custom_mirrors", self._t("Custom mirrors (optional):"))
        set_text("_lbl_settings_page_size", self._t("Page size:"))
        set_text("_btn_save_settings", self._t("Save Settings"))
        set_text("_lbl_settings_look", self._t("Appearance"))
        set_text("_lbl_settings_web", self._t("Web search"))
        set_text("_lbl_settings_folders", self._t("Folders"))
        set_text("_btn_settings_add_folder", self._t("Add Folder"))
        set_text("_btn_settings_rm_folder", self._t("Remove Selected"))
        set_text("_lbl_settings_sub", self._t("Preferences are stored in your user profile and applied right away."))
        set_text("_lbl_settings_theme_hint", self._t("Community packs appear here once installed."))
        set_text("_lbl_settings_page_size_hint", self._t("How many results one search page returns."))
        set_text("_lbl_settings_provider_hint", self._t("DuckDuckGo needs no keys. The other two do."))
        set_text("_lbl_settings_mirrors_hint", self._t("Comma separated base URLs, tried before the built-in list."))
        set_text("_lbl_settings_folders_hint", self._t("These folders are scanned on the Local tab."))
        set_text("_lbl_net_searx", self._t("SearxNG URL:"))
        set_text("_lbl_net_gkey", self._t("Google API key:"))
        set_text("_lbl_net_gcx", self._t("Google CSE ID (cx):"))

        set_text("_btn_web_search", self._t("Search Web"))
        set_text("_btn_refresh_list", self._t("Refresh List"))
        set_text("_btn_open_source", self._t("Open Source Page"))
        set_text("_btn_download_url", self._t("Download URL"))
        set_text("_btn_audit", self._t("Audit Sources"))
        set_text("_btn_stop_download", self._t("Stop"))
        set_text("_btn_job_pause", self._t("Pause"))
        set_text("_btn_job_resume", self._t("Resume"))
        set_text("_btn_job_retry", self._t("Retry"))
        set_text("_btn_job_open", self._t("Open folder"))
        set_text("_btn_job_remove", self._t("Remove"))
        set_text("_btn_job_clear", self._t("Clear finished"))

        set_text("_lbl_net_search", self._t("Search:"))
        set_text("_btn_load_more_remote", self._t("Load more"))
        set_text("_btn_download", self._t("Download"))
        set_text("_btn_copy_url", self._t("Copy URL"))
        set_text("_btn_validate", self._t("Validate"))

        try:
            if hasattr(self, "_tree") and self._tree.winfo_exists():
                self._tree.heading("name", text=self._t("Name"))
                self._tree.heading("path", text=self._t("Path"))
                self._tree.heading("size", text=self._t("Size"))
                self._tree.heading("modified", text=self._t("Modified"))
                try:
                    self._tree.heading("info", text=self._t("Info"))
                except Exception:
                    pass
        except Exception:
            pass

        try:
            if hasattr(self, "_remote_tree") and self._remote_tree.winfo_exists():
                self._remote_tree.heading("name", text=self._t("Name"))
                self._remote_tree.heading("url", text=self._t("URL"))
                self._remote_tree.heading("sha256", text=self._t("SHA-256"))
        except Exception:
            pass

        try:
            if hasattr(self, "_jobs_tree") and self._jobs_tree.winfo_exists():
                for key, title in (("job", "Job"), ("status", "Status"), ("progress", "Progress"),
                                   ("size", "Size"), ("speed", "Speed"), ("eta", "Left")):
                    self._jobs_tree.heading(key, text=self._t(title))
            self._update_jobs_summary()
        except Exception:
            pass

        try:
            if hasattr(self, "_status_var") and is_any_translation(self._status_var.get(), "Ready"):
                self._status_var.set(self._t("Ready"))
        except Exception:
            pass

        try:
            if hasattr(self, "_net_status_var") and is_any_translation(self._net_status_var.get(), "Pick a source and click Refresh"):
                self._net_status_var.set(self._t("Pick a source and click Refresh"))
        except Exception:
            pass

    def _on_close(self) -> None:
        try:
            try:
                self._download_manager_stop.set()
            except Exception:
                pass
            self._collect_settings_from_ui()
            self._save_settings_file()
            self._save_download_jobs(force=True)
        except Exception:
            pass
        try:
            self.destroy()
        except Exception:
            pass

    def _reload_themes(self, use: str | None = None) -> None:
        """Re-read the theme folders, refresh the menu and the Settings list."""
        if _ipm_themes is not None:
            try:
                _ipm_themes.get_registry(refresh=True)
            except Exception:
                pass
        try:
            self._build_menu()
        except Exception:
            pass
        combo = getattr(self, "_settings_theme_combo", None)
        if combo is not None:
            try:
                if combo.winfo_exists():
                    combo.configure(values=_ipm_theme_labels())
            except Exception:
                pass
        if use:
            self._set_theme(use)
            try:
                self._settings_theme_var.set(use)
            except Exception:
                pass

    def _open_theme_shop(self) -> None:
        """Open the Theme Shop window (downloads community themes)."""
        if _ipm_shop is None:
            messagebox.showinfo(
                "Theme Shop",
                "The Theme Shop needs ipm_shop.py next to the application.\n\n"
                "You can also browse the themes at\n"
                "https://anviktor2411.github.io/iso-package-manager/",
                parent=self,
            )
            return
        existing = getattr(self, "_theme_shop_win", None)
        try:
            if existing is not None and existing.winfo_exists():
                existing.deiconify()
                existing.lift()
                existing.focus_force()
                return
        except Exception:
            pass
        try:
            self._theme_shop_win = _ipm_shop.open_shop(self, on_installed=self._reload_themes)
        except Exception as exc:
            messagebox.showerror("Theme Shop", str(exc), parent=self)

    def _set_theme(self, theme: str):
        self._theme_var.set(theme)
        if theme == "Graphical":
            self._icons.clear()
        self._apply_style(theme)
        self._retheme_widgets()

    def _retheme_widgets(self):
        colors = getattr(self, "_colors", {})
        bg = colors.get("bg")
        if bg:
            try:
                self.configure(background=bg)
            except Exception:
                pass

        for name in ("_folders_list", "_settings_folders_list", "_log_list"):
            widget = getattr(self, name, None)
            if widget is None:
                continue
            try:
                if not widget.winfo_exists():
                    continue
                widget.configure(
                    background=colors.get("tree_bg", colors.get("panel", "#ffffff")),
                    foreground=colors.get("text", "#000000"),
                    selectbackground=colors.get("selection", "#316ac5"),
                    selectforeground=colors.get("selection_fg", "#ffffff"),
                    highlightbackground=colors.get("border", colors.get("panel", "#ffffff")),
                    highlightcolor=colors.get("accent", colors.get("selection", "#316ac5")),
                )
            except Exception:
                pass

        try:
            self._paint_job_tags()
        except Exception:
            pass

        try:
            self._normalize_panel_backgrounds()
        except Exception:
            pass

        self._apply_graphical_theme_assets(self._theme_var.get())

    def _apply_graphical_theme_assets(self, theme: str) -> None:
        enabled = theme == "Graphical"
        if enabled:
            self._ensure_icons()

        def set_btn(btn: ttk.Button | None, icon_key: str | None):
            if not btn:
                return
            try:
                if enabled and icon_key:
                    btn.configure(image=self._icons.get(icon_key), compound="left")
                else:
                    btn.configure(image="", compound="none")
            except Exception:
                pass

        set_btn(getattr(self, "_btn_add_folder", None), "folder_add")
        set_btn(getattr(self, "_btn_remove_folder", None), "folder_remove")
        set_btn(getattr(self, "_btn_scan", None), "scan")
        set_btn(getattr(self, "_btn_stop_scan", None), "stop")

        set_btn(getattr(self, "_btn_refresh_list", None), "refresh")
        set_btn(getattr(self, "_btn_open_source", None), "link")
        set_btn(getattr(self, "_btn_download_url", None), "download")
        set_btn(getattr(self, "_btn_audit", None), "audit")
        set_btn(getattr(self, "_btn_stop_download", None), "stop")

        set_btn(getattr(self, "_btn_download", None), "download")
        set_btn(getattr(self, "_btn_copy_url", None), "copy")
        set_btn(getattr(self, "_btn_validate", None), "check")

        set_btn(getattr(self, "_btn_mount_open", None), "mount")
        set_btn(getattr(self, "_btn_extract", None), "extract")
        set_btn(getattr(self, "_btn_copy", None), "copy")

    def _ensure_icons(self) -> None:
        if self._icons:
            return

        colors = getattr(self, "_colors", {})
        icon_bg = colors.get("panel", "#ffffff")
        accent = colors.get("accent", "#2563eb")
        danger = colors.get("danger", "#dc2626")
        muted = colors.get("muted", "#666666")
        is_graphical = self._theme_var.get() == "Graphical"

        def icon(name: str, draw: Callable[[tk.PhotoImage], None]) -> None:
            img = tk.PhotoImage(width=16, height=16)
            img.put(icon_bg, to=(0, 0, 15, 15))
            # simple 1px outline to feel more "icon-like" on light themes
            img.put(muted, to=(0, 0, 15, 0))
            img.put(muted, to=(0, 15, 15, 15))
            img.put(muted, to=(0, 0, 0, 15))
            img.put(muted, to=(15, 0, 15, 15))
            draw(img)
            if is_graphical:
                img = img.zoom(2, 2)
            self._icons[name] = img

        def fill(img: tk.PhotoImage, x0: int, y0: int, x1: int, y1: int, color: str) -> None:
            img.put(color, to=(x0, y0, x1, y1))

        def plus(img: tk.PhotoImage, color: str) -> None:
            fill(img, 7, 3, 8, 12, color)
            fill(img, 3, 7, 12, 8, color)

        def minus(img: tk.PhotoImage, color: str) -> None:
            fill(img, 3, 7, 12, 8, color)

        def triangle_play(img: tk.PhotoImage, color: str) -> None:
            for i in range(0, 9):
                fill(img, 4 + i, 4 + i // 2, 4 + i, 11 - i // 2, color)

        def square_stop(img: tk.PhotoImage, color: str) -> None:
            fill(img, 4, 4, 11, 11, color)

        def arrow_down(img: tk.PhotoImage, color: str) -> None:
            fill(img, 7, 3, 8, 9, color)
            fill(img, 5, 9, 10, 10, color)
            fill(img, 6, 10, 9, 11, color)
            fill(img, 7, 11, 8, 12, color)

        def refresh(img: tk.PhotoImage, color: str) -> None:
            fill(img, 4, 4, 11, 5, color)
            fill(img, 11, 4, 12, 9, color)
            fill(img, 4, 10, 11, 11, color)
            fill(img, 3, 5, 4, 10, color)
            fill(img, 10, 2, 12, 3, color)
            fill(img, 12, 3, 13, 5, color)

        def link(img: tk.PhotoImage, color: str) -> None:
            fill(img, 4, 6, 7, 9, color)
            fill(img, 9, 6, 12, 9, color)
            fill(img, 7, 7, 9, 8, color)

        def doc(img: tk.PhotoImage, color: str) -> None:
            fill(img, 4, 3, 11, 12, color)
            fill(img, 6, 5, 9, 5, "#ffffff")
            fill(img, 6, 7, 10, 7, "#ffffff")
            fill(img, 6, 9, 10, 9, "#ffffff")

        def box(img: tk.PhotoImage, color: str) -> None:
            fill(img, 4, 6, 11, 11, color)
            fill(img, 4, 5, 11, 6, muted)
            fill(img, 4, 11, 11, 12, muted)

        def shield(img: tk.PhotoImage, color: str) -> None:
            fill(img, 6, 3, 9, 3, color)
            fill(img, 5, 4, 10, 8, color)
            fill(img, 6, 8, 9, 12, color)

        def drive(img: tk.PhotoImage, color: str) -> None:
            fill(img, 3, 9, 12, 11, muted)
            fill(img, 4, 5, 11, 9, color)
            fill(img, 10, 10, 11, 10, "#00aa00")

        icon("folder_add", lambda im: (fill(im, 3, 6, 12, 11, muted), plus(im, accent)))
        icon("folder_remove", lambda im: (fill(im, 3, 6, 12, 11, muted), minus(im, danger)))
        icon("scan", lambda im: triangle_play(im, accent))
        icon("stop", lambda im: square_stop(im, danger))
        icon("download", lambda im: arrow_down(im, accent))
        icon("refresh", lambda im: refresh(im, accent))
        icon("link", lambda im: link(im, accent))
        icon("copy", lambda im: doc(im, accent))
        icon("check", lambda im: shield(im, "#16a34a"))
        icon("extract", lambda im: box(im, accent))
        icon("audit", lambda im: shield(im, accent))
        icon("mount", lambda im: drive(im, accent))

    def _apply_style(self, theme: str):
        try:
            available = set(self._style.theme_names())
            if theme == "Windows XP":
                if "xpnative" in available:
                    self._style.theme_use("xpnative")
                else:
                    for t in ("clam", "alt", "vista"):
                        if t in available:
                            self._style.theme_use(t)
                            break
            elif theme == "Graphical":
                for t in ("clam", "alt", "vista", "xpnative"):
                    if t in available:
                        self._style.theme_use(t)
                        break
            elif theme == "Default":
                for t in ("vista", "xpnative", "clam", "alt"):
                    if t in available:
                        self._style.theme_use(t)
                        break
            else:
                for t in ("clam", "alt", "vista", "xpnative"):
                    if t in available:
                        self._style.theme_use(t)
                        break
        except Exception:
            pass

        try:
            if theme == "Windows XP":
                font_family = "Tahoma" if is_windows() else "Sans"
                bg = "#d4d0c8"
                panel = "#ece9d8"
                text = "#000000"
                muted = "#404040"
                accent = "#d4d0c8"
                accent_active = "#c9c6bf"
                danger = "#d4d0c8"
                danger_active = "#c9c6bf"
                border = "#808080"
                selection = "#316ac5"
                tree_bg = "#ffffff"
                tree_heading_bg = "#d4d0c8"
                tab_bg = "#ece9d8"
                tab_selected_bg = "#ffffff"
            elif theme == "Graphical":
                font_family = "Segoe UI" if is_windows() else "Sans"
                bg = "#e0f2fe"
                panel = "#ffffff"
                text = "#0f172a"
                muted = "#334155"
                accent = "#0ea5e9"
                accent_active = "#0284c7"
                danger = "#ef4444"
                danger_active = "#dc2626"
                border = "#94a3b8"
                selection = "#0ea5e9"
                tree_bg = "#ffffff"
                tree_heading_bg = "#bae6fd"
                tab_bg = "#c7d2fe"
                tab_selected_bg = "#ffffff"
            elif theme == "Default":
                font_family = "Segoe UI" if is_windows() else "Sans"
                bg = "#f3f4f6"
                panel = "#ffffff"
                text = "#111827"
                muted = "#4b5563"
                accent = "#2563eb"
                accent_active = "#1d4ed8"
                danger = "#dc2626"
                danger_active = "#b91c1c"
                border = "#d1d5db"
                selection = "#2563eb"
                tree_bg = "#ffffff"
                tree_heading_bg = "#f3f4f6"
                tab_bg = "#e5e7eb"
                tab_selected_bg = "#ffffff"
            else:
                font_family = "Segoe UI" if is_windows() else "Sans"
                bg = "#0f172a"
                panel = "#111827"
                text = "#e5e7eb"
                muted = "#9ca3af"
                accent = "#2563eb"
                accent_active = "#1d4ed8"
                danger = "#dc2626"
                danger_active = "#b91c1c"
                border = "#1f2937"
                selection = "#1f3a8a"
                tree_bg = panel
                tree_heading_bg = bg
                tab_bg = panel
                tab_selected_bg = bg

            # -- theme packs (ipm_themes) ---------------------------------
            # A user-installed pack paints with its own palette. Anything the
            # pack leaves unset keeps the value from the base theme above, so a
            # one-colour pack still behaves. ``_pack`` is None for built-ins.
            _pack = _ipm_pack_kwargs(theme)
            if _pack:
                font_family = _pack.get("font_family") or font_family
                bg = _pack["bg"]
                panel = _pack["panel"]
                text = _pack["text"]
                muted = _pack["muted"]
                accent = _pack["accent"]
                accent_active = _pack["accent_active"]
                danger = _pack["danger"]
                danger_active = _pack["danger_active"]
                border = _pack["border"]
                selection = _pack["selection"]
                tree_bg = _pack.get("tree_bg") or panel
                tree_heading_bg = _pack.get("tree_heading_bg") or bg
                tab_bg = _pack.get("tab_bg") or panel
                tab_selected_bg = _pack.get("tab_selected_bg") or bg
            # -------------------------------------------------------------

            # Text that sits *on* accent / selection, and the table header text.
            # Packs may set them; the built-ins keep the old hard-coded values.
            accent_text = (_pack.get("accent_text") if _pack else None) or "#ffffff"
            selection_fg = (_pack.get("selection_fg") if _pack else None) or "#ffffff"
            heading_fg = (_pack.get("tree_heading_fg") if _pack else None) or text

            default_font = (font_family, 10)
            ui_font = (font_family, 10)
            heading_font = (font_family, 10, "bold")
            title_font = (font_family, 14, "bold")

            self._colors = {
                "bg": bg,
                "panel": panel,
                "text": text,
                "muted": muted,
                "accent": accent,
                "danger": danger,
                "border": border,
                "selection": selection,
                "accent_text": accent_text,
                "selection_fg": selection_fg,
                "tree_bg": tree_bg,
                "row_alt": _mix(tree_bg, text, 0.05),
                "accent_active": accent_active,
                "ok": _mix(text, "#22c55e", 0.72),
                "panel": panel,
                "muted": muted,
            }

            self.configure(background=bg)

            self._style.configure("TFrame", background=bg)
            self._style.configure("Panel.TFrame", background=panel)
            self._style.configure("TLabel", font=ui_font, background=bg, foreground=text)
            self._style.configure("Muted.TLabel", font=ui_font, background=bg, foreground=muted)
            self._style.configure("Title.TLabel", font=title_font, background=bg, foreground=text)
            self._style.configure("TSeparator", background=border)
            self._style.configure("Status.TLabel", font=ui_font, background=panel,
                                  foreground=muted, padding=(10, 6))

            # Labels sitting *on* a card need the card's background, otherwise
            # every caption looks like a little box of the window colour.
            self._style.configure("Panel.TLabel", font=ui_font, background=panel, foreground=text)
            self._style.configure("PanelMuted.TLabel", font=ui_font, background=panel, foreground=muted)
            self._style.configure("PanelTitle.TLabel", font=title_font, background=panel, foreground=text)
            self._style.configure("Section.TLabel", font=heading_font, background=panel, foreground=text)
            self._style.configure("Hint.TLabel", font=(font_family, 9), background=panel, foreground=muted)
            self._style.configure("Value.TLabel", font=heading_font, background=panel, foreground=accent)
            self._style.configure("Card.TFrame", background=panel, bordercolor=border,
                                  lightcolor=border, darkcolor=border,
                                  relief="solid", borderwidth=1)

            self._style.configure("TEntry", padding=(8, 6))
            self._style.configure("TCombobox", padding=(8, 6))

            self._style.configure(
                "TButton",
                font=ui_font,
                padding=(10, 7),
            )
            if theme not in ("Graphical", "Windows XP"):
                # Plain buttons used to keep the ttk default grey, which looked
                # wrong on every palette. They now get their own surface - a
                # shade away from the panel so the button is visible - with a
                # hover tint mixed towards the accent.
                surface = _mix(panel, text, 0.10)
                hover = _mix(panel, accent, 0.30)
                pressed = _mix(panel, accent, 0.45)
                self._style.configure(
                    "TButton",
                    background=surface,
                    foreground=text,
                    bordercolor=_mix(border, text, 0.12),
                    lightcolor=surface,
                    darkcolor=surface,
                    focuscolor=accent,
                    focusthickness=1,
                    borderwidth=1,
                    relief="solid",
                )
                self._style.map(
                    "TButton",
                    background=[("disabled", _mix(panel, bg, 0.6)), ("pressed", pressed), ("active", hover)],
                    foreground=[("disabled", _mix(muted, bg, 0.35))],
                    bordercolor=[("focus", accent), ("active", accent), ("disabled", border)],
                    lightcolor=[("pressed", pressed), ("active", hover)],
                    darkcolor=[("pressed", pressed), ("active", hover)],
                )
            if theme == "Graphical":
                self._style.configure(
                    "TButton",
                    background=panel,
                    foreground=text,
                    bordercolor=border,
                    focusthickness=1,
                    focuscolor=accent,
                    relief="raised",
                )
                self._style.map(
                    "TButton",
                    background=[("active", "#f0f9ff"), ("pressed", "#e0f2fe"), ("disabled", "#e5e7eb")],
                    foreground=[("disabled", muted)],
                    relief=[("pressed", "sunken"), ("active", "raised")],
                )
            if theme == "Windows XP":
                self._style.configure(
                    "TButton",
                    background=panel,
                    foreground=text,
                    bordercolor=border,
                    focusthickness=1,
                    focuscolor=border,
                    relief="raised",
                )
                self._style.map(
                    "TButton",
                    background=[("active", "#f6f5f1"), ("pressed", "#c9c6bf"), ("disabled", "#d4d0c8")],
                    foreground=[("disabled", muted)],
                    relief=[("pressed", "sunken"), ("active", "raised")],
                )
            if theme == "Windows XP":
                self._style.configure(
                    "Accent.TButton",
                    font=ui_font,
                    padding=(12, 8),
                    background=panel,
                    foreground=text,
                    bordercolor=border,
                    relief="raised",
                    focusthickness=1,
                    focuscolor=border,
                )
                self._style.map(
                    "Accent.TButton",
                    background=[("pressed", "#bdbab3"), ("active", accent_active), ("disabled", "#d4d0c8")],
                    foreground=[("disabled", muted)],
                    relief=[("pressed", "sunken"), ("active", "raised")],
                )
                self._style.configure(
                    "Danger.TButton",
                    font=ui_font,
                    padding=(12, 8),
                    background=panel,
                    foreground=text,
                    bordercolor=border,
                    relief="raised",
                    focusthickness=1,
                    focuscolor=border,
                )
                self._style.map(
                    "Danger.TButton",
                    background=[("pressed", "#bdbab3"), ("active", danger_active), ("disabled", "#d4d0c8")],
                    foreground=[("disabled", muted)],
                    relief=[("pressed", "sunken"), ("active", "raised")],
                )
            else:
                self._style.configure(
                    "Accent.TButton",
                    font=ui_font,
                    padding=(12, 8),
                    background=accent,
                    foreground=accent_text,
                    bordercolor=accent,
                    focusthickness=2,
                    focuscolor=border,
                    relief="raised" if theme == "Graphical" else "flat",
                )
                self._style.map(
                    "Accent.TButton",
                    background=[("active", accent_active), ("pressed", accent_active), ("disabled", "#e5e7eb" if theme == "Graphical" else "#374151")],
                    foreground=[("disabled", muted if theme == "Graphical" else "#9ca3af")],
                    relief=[("pressed", "sunken"), ("active", "raised")] if theme == "Graphical" else [],
                )
                self._style.configure(
                    "Danger.TButton",
                    font=ui_font,
                    padding=(12, 8),
                    background=danger,
                    foreground="#ffffff",
                    bordercolor=danger,
                    focusthickness=2,
                    focuscolor=border,
                    relief="raised" if theme == "Graphical" else "flat",
                )
                self._style.map(
                    "Danger.TButton",
                    background=[("active", danger_active), ("pressed", danger_active), ("disabled", "#e5e7eb" if theme == "Graphical" else "#374151")],
                    foreground=[("disabled", muted if theme == "Graphical" else "#9ca3af")],
                    relief=[("pressed", "sunken"), ("active", "raised")] if theme == "Graphical" else [],
                )

            self._style.configure(
                "Treeview",
                font=default_font,
                rowheight=28,
                background=tree_bg,
                fieldbackground=tree_bg,
                foreground=text,
                bordercolor=border,
                lightcolor=border,
                darkcolor=border,
            )
            self._style.configure(
                "Treeview.Heading",
                font=heading_font,
                background=_mix(tree_heading_bg, text, 0.06),
                foreground=heading_fg,
                bordercolor=border,
                relief="raised" if theme == "Windows XP" else "flat",
                padding=(10, 11),
            )
            self._style.map(
                "Treeview.Heading",
                background=[("active", _mix(tree_heading_bg, accent, 0.25))],
                foreground=[("active", text)],
            )
            self._style.map(
                "Treeview",
                background=[("selected", selection)],
                foreground=[("selected", selection_fg)],
            )

            self._style.configure(
                "TNotebook",
                background=bg,
                bordercolor=border,
                lightcolor=bg,
                darkcolor=bg,
                tabmargins=(10, 6, 10, 0),
            )
            self._style.configure(
                "TNotebook.Tab",
                font=heading_font,
                padding=(14, 10),
                background=tab_bg,
                foreground=muted,
                bordercolor=border,
                lightcolor=tab_bg,
                darkcolor=tab_bg,
                relief="raised" if theme == "Windows XP" else "flat",
            )
            _use_dark_title_bar(self, _is_dark(bg))
            self._menu_colors = {
                "background": panel,
                "foreground": text,
                "activebackground": accent,
                "activeforeground": accent_text,
                "selectcolor": accent,
                "borderwidth": 0,
                "relief": "flat",
            }
            _try_menu = getattr(self, "_paint_menus", None)
            if callable(_try_menu):
                _try_menu()
            self._style.map(
                "TNotebook.Tab",
                background=[("selected", tab_selected_bg), ("active", _mix(tab_bg, accent, 0.18))],
                foreground=[("selected", text), ("active", text)],
                lightcolor=[("selected", accent)],
                bordercolor=[("selected", accent)],
                relief=[("selected", "raised"), ("active", "raised")] if theme == "Windows XP" else [],
            )

            # -- the rest of the widgets follow the palette too ---------------
            # Every block is separate: a ttk engine that does not know an option
            # (vista/xpnative are picky) must not stop the ones after it.
            field_bg = tree_bg if theme != "Windows XP" else panel

            def _try(func):
                try:
                    func()
                except Exception:
                    pass

            _try(lambda: self._style.configure(
                "TEntry", fieldbackground=field_bg, foreground=text, insertcolor=text,
                bordercolor=border, lightcolor=border, darkcolor=border,
            ))
            _try(lambda: self._style.map(
                "TEntry",
                bordercolor=[("focus", accent)], lightcolor=[("focus", accent)],
                fieldbackground=[("disabled", panel)], foreground=[("disabled", muted)],
            ))
            _try(lambda: self._style.configure(
                "TCombobox", fieldbackground=field_bg, background=panel, foreground=text,
                arrowcolor=accent, bordercolor=border, lightcolor=border, darkcolor=border,
            ))
            _try(lambda: self._style.map(
                "TCombobox",
                fieldbackground=[("readonly", field_bg), ("disabled", panel)],
                foreground=[("disabled", muted)],
                bordercolor=[("focus", accent)], lightcolor=[("focus", accent)],
                selectbackground=[("readonly", selection)], selectforeground=[("readonly", selection_fg)],
            ))
            _try(lambda: self._style.configure(
                "TSpinbox", fieldbackground=field_bg, foreground=text, arrowcolor=accent,
                bordercolor=border, lightcolor=border, darkcolor=border,
            ))
            for _name in ("TCheckbutton", "TRadiobutton"):
                _try(lambda name=_name: self._style.configure(
                    name, font=ui_font, background=bg, foreground=text,
                    focuscolor=accent, indicatorcolor=field_bg, bordercolor=border,
                ))
                _try(lambda name=_name: self._style.map(
                    name,
                    foreground=[("disabled", muted)],
                    indicatorcolor=[("selected", accent), ("pressed", accent_active)],
                    background=[("active", bg)],
                ))
            for _name in ("Vertical.TScrollbar", "Horizontal.TScrollbar"):
                _try(lambda name=_name: self._style.configure(
                    name, background=panel, troughcolor=bg, bordercolor=border,
                    arrowcolor=muted, lightcolor=panel, darkcolor=panel,
                ))
                _try(lambda name=_name: self._style.map(
                    name,
                    background=[("active", accent), ("pressed", accent_active)],
                    arrowcolor=[("active", accent_text)],
                ))
            _try(lambda: self._style.configure(
                "Horizontal.TProgressbar", background=accent, troughcolor=panel,
                bordercolor=border, lightcolor=accent, darkcolor=accent,
            ))
            _try(lambda: self._style.configure(
                "TLabelframe", background=bg, bordercolor=border, lightcolor=border, darkcolor=border,
            ))
            _try(lambda: self._style.configure(
                "TLabelframe.Label", background=bg, foreground=accent, font=heading_font,
            ))
            _try(lambda: self._style.configure("TPanedwindow", background=bg))
            _try(lambda: self._style.configure("Sash", background=border))
        except Exception:
            pass

    def _build_ui(self):
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        self._notebook = ttk.Notebook(self)
        self._notebook.grid(row=0, column=0, rowspan=3, sticky="nsew")

        self._tab_local = ttk.Frame(self._notebook)
        self._tab_internet = ttk.Frame(self._notebook)
        self._tab_settings = ttk.Frame(self._notebook)
        self._tab_logs = ttk.Frame(self._notebook)
        self._notebook.add(self._tab_local, text=self._t("Local"))
        self._notebook.add(self._tab_internet, text=self._t("Internet"))
        self._notebook.add(self._tab_settings, text=self._t("Settings"))
        self._notebook.add(self._tab_logs, text=self._t("Logs"))

        self._build_local_tab(self._tab_local)
        self._build_internet_tab(self._tab_internet)
        self._build_settings_tab(self._tab_settings)
        self._build_logs_tab(self._tab_logs)

        try:
            self._normalize_panel_backgrounds()
        except Exception:
            pass

    def _log(self, msg: str) -> None:
        try:
            ts = datetime.now().strftime("%H:%M:%S")
        except Exception:
            ts = ""
        line = f"[{ts}] {msg}" if ts else str(msg)

        try:
            if hasattr(self, "_log_list") and self._log_list.winfo_exists():
                self._log_list.insert(tk.END, line)
                self._log_list.yview_moveto(1.0)
        except Exception:
            pass

    def _build_logs_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(1, weight=1)

        header = ttk.Frame(parent, padding=(12, 12, 12, 0))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        self._lbl_logs_header = ttk.Label(header, text=self._t("Logs"), style="Title.TLabel")
        self._lbl_logs_header.grid(row=0, column=0, sticky="w")

        content = ttk.Frame(parent, padding=10)
        content.grid(row=1, column=0, sticky="nsew")
        content.columnconfigure(0, weight=1)
        content.rowconfigure(0, weight=1)

        panel = ttk.Frame(content, padding=(10, 10, 10, 10), style="Panel.TFrame")
        panel.grid(row=0, column=0, sticky="nsew")
        panel.columnconfigure(0, weight=1)
        panel.rowconfigure(0, weight=1)

        colors = getattr(self, "_colors", {})
        lb_bg = colors.get("panel", "#111827")
        lb_fg = colors.get("text", "#e5e7eb")
        lb_sel = "#1f3a8a"
        self._log_list = tk.Listbox(
            panel,
            height=12,
            selectmode=tk.BROWSE,
            background=lb_bg,
            foreground=lb_fg,
            selectbackground=lb_sel,
            selectforeground="#ffffff",
            highlightthickness=1,
            relief="flat",
        )
        self._log_list.grid(row=0, column=0, sticky="nsew")

        scroll = ttk.Scrollbar(panel, orient="vertical", command=self._log_list.yview)
        scroll.grid(row=0, column=1, sticky="ns")
        self._log_list.configure(yscrollcommand=scroll.set)

        btn_row = ttk.Frame(content)
        btn_row.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        btn_row.columnconfigure(0, weight=1)

        def clear_logs():
            try:
                self._log_list.delete(0, tk.END)
            except Exception:
                pass

        def save_logs():
            try:
                path = filedialog.asksaveasfilename(
                    title="Save log",
                    defaultextension=".txt",
                    filetypes=[("Text", "*.txt"), ("All files", "*.*")],
                )
                if not path:
                    return
                lines = list(self._log_list.get(0, tk.END))
                Path(path).write_text("\n".join(lines), encoding="utf-8")
            except Exception as e:
                messagebox.showerror("Error", f"Could not save log: {e}")

        self._btn_logs_clear = ttk.Button(btn_row, text="Clear", command=clear_logs)
        self._btn_logs_clear.grid(row=0, column=0, sticky="w", padx=(0, 8))
        self._btn_logs_save = ttk.Button(btn_row, text="Save", command=save_logs)
        self._btn_logs_save.grid(row=0, column=1, sticky="w")

        self._log("Logs ready")

    def _load_settings_file(self) -> None:
        try:
            if self._settings_path.exists():
                self._settings = json.loads(self._settings_path.read_text(encoding="utf-8"))
        except Exception:
            self._settings = {}

    def _save_settings_file(self) -> None:
        try:
            self._settings_path.write_text(json.dumps(self._settings, indent=2, ensure_ascii=False), encoding="utf-8")
        except Exception as e:
            messagebox.showerror("Error", f"Could not save settings: {e}")

    def _apply_loaded_settings_to_ui_vars(self) -> None:
        try:
            if isinstance(self._settings.get("theme"), str):
                self._theme_var.set(str(self._settings.get("theme")))
            if isinstance(self._settings.get("language"), str):
                self._lang_var.set(str(self._settings.get("language")))
        except Exception:
            pass

        def _set_if_exists(attr: str, key: str) -> None:
            v = getattr(self, attr, None)
            if v is None:
                return
            try:
                if isinstance(self._settings.get(key), str):
                    v.set(str(self._settings.get(key)))
            except Exception:
                pass

        _set_if_exists("_web_provider_var", "web_provider")
        _set_if_exists("_searx_url_var", "searxng_url")
        _set_if_exists("_google_key_var", "google_key")
        _set_if_exists("_google_cx_var", "google_cx")
        _set_if_exists("_remote_page_size_var", "remote_page_size")
        _set_if_exists("_deterministic_first_var", "deterministic_first")
        _set_if_exists("_web_archive_level_var", "web_archive_level")
        _set_if_exists("_auto_verify_var", "auto_verify")

        try:
            if isinstance(self._settings.get("custom_mirrors"), str):
                os.environ["IPM_CUSTOM_MIRRORS"] = str(self._settings.get("custom_mirrors"))
        except Exception:
            pass

        try:
            startup = self._settings.get("startup_tab")
            if isinstance(startup, str) and startup and hasattr(self, "_notebook"):
                if startup == "Local":
                    self._notebook.select(self._tab_local)
                elif startup == "Internet":
                    self._notebook.select(self._tab_internet)
                elif startup == "Settings":
                    self._notebook.select(self._tab_settings)
        except Exception:
            pass

        for attr, key in (
            ("_settings_theme_var", "theme"),
            ("_settings_lang_var", "language"),
            ("_settings_startup_tab_var", "startup_tab"),
            ("_settings_provider_var", "web_provider"),
            ("_settings_searx_url_var", "searxng_url"),
            ("_settings_google_key_var", "google_key"),
            ("_settings_google_cx_var", "google_cx"),
            ("_settings_custom_mirrors_var", "custom_mirrors"),
            ("_settings_remote_page_size_var", "remote_page_size"),
        ):
            _set_if_exists(attr, key)

        try:
            if hasattr(self, "_settings_folders_list") and self._settings_folders_list.winfo_exists():
                self._settings_folders_list.delete(0, tk.END)
                for p in self._folders_list.get(0, tk.END):
                    self._settings_folders_list.insert(tk.END, str(p))
        except Exception:
            pass

    def _collect_settings_from_ui(self) -> None:
        try:
            theme = (getattr(self, "_settings_theme_var", None).get() if hasattr(self, "_settings_theme_var") else "")
            lang = (getattr(self, "_settings_lang_var", None).get() if hasattr(self, "_settings_lang_var") else "")
            if theme:
                self._theme_var.set(theme)
            if lang:
                self._lang_var.set(lang)
            self._settings["theme"] = self._theme_var.get()
            self._settings["language"] = self._lang_var.get()
        except Exception:
            pass

        def _get_str(attr: str) -> str:
            try:
                v = getattr(self, attr, None)
                if v is None:
                    return ""
                return str(v.get() or "").strip()
            except Exception:
                return ""

        provider = _get_str("_settings_provider_var") or _get_str("_web_provider_var")
        searx = _get_str("_settings_searx_url_var") or _get_str("_searx_url_var")
        gkey = _get_str("_settings_google_key_var") or _get_str("_google_key_var")
        gcx = _get_str("_settings_google_cx_var") or _get_str("_google_cx_var")
        mirrors = _get_str("_settings_custom_mirrors_var")
        page_size = _get_str("_settings_remote_page_size_var") or _get_str("_remote_page_size_var")
        startup_tab = _get_str("_settings_startup_tab_var")
        det_first = _get_str("_deterministic_first_var")
        arch_lvl = _get_str("_web_archive_level_var")
        auto_verify = _get_str("_auto_verify_var")

        if provider:
            self._settings["web_provider"] = provider
        self._settings["searxng_url"] = searx
        self._settings["google_key"] = gkey
        self._settings["google_cx"] = gcx
        self._settings["custom_mirrors"] = mirrors
        if page_size:
            self._settings["remote_page_size"] = page_size
        if startup_tab:
            self._settings["startup_tab"] = startup_tab
        if det_first:
            self._settings["deterministic_first"] = det_first
        if arch_lvl:
            self._settings["web_archive_level"] = arch_lvl
        if auto_verify:
            self._settings["auto_verify"] = auto_verify

        try:
            # A maximized/zoomed window reports the full screen size; keep the last
            # normal size instead so the app does not re-open full screen every time.
            state = str(self.state())
        except Exception:
            state = "normal"
        try:
            if state == "normal":
                self._settings["geometry"] = str(self.geometry())
        except Exception:
            pass

        try:
            if hasattr(self, "_notebook"):
                cur = self._notebook.select()
                if cur == str(self._tab_local):
                    self._settings["startup_tab"] = "Local"
                elif cur == str(self._tab_internet):
                    self._settings["startup_tab"] = "Internet"
                elif hasattr(self, "_tab_settings") and cur == str(self._tab_settings):
                    self._settings["startup_tab"] = "Settings"
        except Exception:
            pass

        try:
            if hasattr(self, "_settings_folders_list") and self._settings_folders_list.winfo_exists():
                folders = list(self._settings_folders_list.get(0, tk.END))
                self._folders_list.delete(0, tk.END)
                for p in folders:
                    self._folders_list.insert(tk.END, str(p))
                self._settings["folders"] = folders
            else:
                self._settings["folders"] = list(self._folders_list.get(0, tk.END))
        except Exception:
            pass

        try:
            if mirrors:
                os.environ["IPM_CUSTOM_MIRRORS"] = mirrors
            else:
                os.environ.pop("IPM_CUSTOM_MIRRORS", None)
        except Exception:
            pass

        try:
            if hasattr(self, "_web_provider_var") and provider:
                self._web_provider_var.set(provider)
            if hasattr(self, "_searx_url_var"):
                self._searx_url_var.set(searx)
            if hasattr(self, "_google_key_var"):
                self._google_key_var.set(gkey)
            if hasattr(self, "_google_cx_var"):
                self._google_cx_var.set(gcx)
            if hasattr(self, "_remote_page_size_var") and page_size:
                self._remote_page_size_var.set(page_size)
        except Exception:
            pass

    def _save_settings_clicked(self) -> None:
        self._collect_settings_from_ui()
        self._save_settings_file()
        try:
            self._apply_style(self._theme_var.get())
        except Exception:
            pass
        self._build_menu()
        self._apply_language()
        self._retheme_widgets()
        try:
            self._set_net_status("Ready")
        except Exception:
            pass
        try:
            stamp = datetime.now().strftime("%H:%M:%S")
            self._settings_saved_var.set(f"{self._t('Saved')} {stamp}")
        except Exception:
            pass

    def _card(self, parent, title: str, *, padding=(14, 12, 14, 14)):
        """A titled card. Returns (card, body) - put the content in ``body``."""
        card = ttk.Frame(parent, padding=padding, style="Card.TFrame")
        card.columnconfigure(0, weight=1)
        head = ttk.Label(card, text=self._t(title), style="Section.TLabel")
        head.grid(row=0, column=0, sticky="w")
        ttk.Separator(card, orient="horizontal").grid(row=1, column=0, sticky="ew", pady=(6, 10))
        body = ttk.Frame(card, style="Panel.TFrame")
        body.grid(row=2, column=0, sticky="nsew")
        body.columnconfigure(1, weight=1)
        card.rowconfigure(2, weight=1)
        return card, body, head

    @staticmethod
    def _field(body, row: int, label, widget, hint_widget=None) -> int:
        """One label + control line inside a card body."""
        label.grid(row=row, column=0, sticky="w", padx=(0, 14), pady=(0, 10))
        widget.grid(row=row, column=1, sticky="ew", pady=(0, 10))
        if hint_widget is not None:
            hint_widget.grid(row=row + 1, column=1, sticky="w", pady=(0, 10))
            return row + 2
        return row + 1

    def _build_settings_tab(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(1, weight=1)

        header = ttk.Frame(parent, padding=(12, 12, 12, 0))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        self._lbl_settings_header = ttk.Label(header, text=self._t("Settings"), style="Title.TLabel")
        self._lbl_settings_header.grid(row=0, column=0, sticky="w")
        self._lbl_settings_sub = ttk.Label(
            header,
            text=self._t("Preferences are stored in your user profile and applied right away."),
            style="Muted.TLabel",
        )
        self._lbl_settings_sub.grid(row=1, column=0, sticky="w", pady=(2, 0))

        content = ttk.Frame(parent, padding=10)
        content.grid(row=1, column=0, sticky="nsew")
        content.columnconfigure(0, weight=1, uniform="settings")
        content.columnconfigure(1, weight=1, uniform="settings")
        content.rowconfigure(2, weight=1)

        self._settings_theme_var = tk.StringVar(value=self._theme_var.get())
        self._settings_lang_var = tk.StringVar(value=self._lang_var.get())
        self._settings_startup_tab_var = tk.StringVar(value="Internet")

        self._settings_provider_var = tk.StringVar(value="DuckDuckGo")
        self._settings_searx_url_var = tk.StringVar(value="")
        self._settings_google_key_var = tk.StringVar(value="")
        self._settings_google_cx_var = tk.StringVar(value="")
        self._settings_custom_mirrors_var = tk.StringVar(value="")
        self._settings_remote_page_size_var = tk.StringVar(value="200")

        # ---- Appearance ---------------------------------------------------
        look_card, look, self._lbl_settings_look = self._card(content, "Appearance")
        look_card.grid(row=0, column=0, sticky="nsew", padx=(0, 6), pady=(0, 10))

        row = 0
        self._lbl_settings_theme = ttk.Label(look, text=self._t("Theme"))
        self._settings_theme_combo = ttk.Combobox(
            look,
            textvariable=self._settings_theme_var,
            state="readonly",
            values=_ipm_theme_labels(),
        )
        self._lbl_settings_theme_hint = ttk.Label(
            look, text=self._t("Community packs appear here once installed."), style="Hint.TLabel",
        )
        row = self._field(look, row, self._lbl_settings_theme,
                          self._settings_theme_combo, self._lbl_settings_theme_hint)

        self._lbl_settings_lang = ttk.Label(look, text=self._t("Language"))
        self._settings_lang_combo = ttk.Combobox(
            look,
            textvariable=self._settings_lang_var,
            state="readonly",
            values=["English", "German", "Spanish", "French", "Russian", "Portuguese", "Chinese (Simplified)"],
        )
        row = self._field(look, row, self._lbl_settings_lang, self._settings_lang_combo)

        self._lbl_settings_startup = ttk.Label(look, text=self._t("Startup tab"))
        self._settings_startup_combo = ttk.Combobox(
            look,
            textvariable=self._settings_startup_tab_var,
            state="readonly",
            values=["Local", "Internet", "Settings"],
        )
        row = self._field(look, row, self._lbl_settings_startup, self._settings_startup_combo)

        self._lbl_settings_page_size = ttk.Label(look, text=self._t("Page size:"))
        self._settings_page_size_combo = ttk.Combobox(
            look,
            textvariable=self._settings_remote_page_size_var,
            values=["50", "100", "200", "500", "1000"],
            state="readonly",
        )
        self._lbl_settings_page_size_hint = ttk.Label(
            look, text=self._t("How many results one search page returns."), style="Hint.TLabel",
        )
        row = self._field(look, row, self._lbl_settings_page_size,
                          self._settings_page_size_combo, self._lbl_settings_page_size_hint)

        # ---- Web search ---------------------------------------------------
        web_card, web, self._lbl_settings_web = self._card(content, "Web search")
        web_card.grid(row=0, column=1, sticky="nsew", padx=(6, 0), pady=(0, 10))

        row = 0
        self._lbl_settings_provider = ttk.Label(web, text=self._t("Provider:"))
        self._settings_provider_combo = ttk.Combobox(
            web,
            textvariable=self._settings_provider_var,
            state="readonly",
            values=["DuckDuckGo", "SearxNG", "Google API"],
        )
        self._lbl_settings_provider_hint = ttk.Label(
            web, text=self._t("DuckDuckGo needs no keys. The other two do."), style="Hint.TLabel",
        )
        row = self._field(web, row, self._lbl_settings_provider,
                          self._settings_provider_combo, self._lbl_settings_provider_hint)

        self._lbl_settings_searx = ttk.Label(web, text=self._t("SearxNG URL:"))
        self._settings_searx_entry = ttk.Entry(web, textvariable=self._settings_searx_url_var)
        row = self._field(web, row, self._lbl_settings_searx, self._settings_searx_entry)

        self._lbl_settings_gkey = ttk.Label(web, text=self._t("Google API key:"))
        self._settings_gkey_entry = ttk.Entry(web, textvariable=self._settings_google_key_var, show="•")
        row = self._field(web, row, self._lbl_settings_gkey, self._settings_gkey_entry)

        self._lbl_settings_gcx = ttk.Label(web, text=self._t("Google CSE ID (cx):"))
        self._settings_gcx_entry = ttk.Entry(web, textvariable=self._settings_google_cx_var)
        row = self._field(web, row, self._lbl_settings_gcx, self._settings_gcx_entry)

        self._lbl_settings_custom_mirrors = ttk.Label(web, text=self._t("Custom mirrors (optional):"))
        self._settings_custom_mirrors_entry = ttk.Entry(web, textvariable=self._settings_custom_mirrors_var)
        self._lbl_settings_mirrors_hint = ttk.Label(
            web, text=self._t("Comma separated base URLs, tried before the built-in list."), style="Hint.TLabel",
        )
        row = self._field(web, row, self._lbl_settings_custom_mirrors,
                          self._settings_custom_mirrors_entry, self._lbl_settings_mirrors_hint)

        # ---- Folders ------------------------------------------------------
        folders_card, folders_body, self._lbl_settings_folders = self._card(content, "Folders")
        folders_card.grid(row=1, column=0, columnspan=2, sticky="new")
        folders_body.columnconfigure(0, weight=1)
        folders_body.rowconfigure(0, weight=1)

        folders_frame = ttk.Frame(folders_body, style="Panel.TFrame")
        folders_frame.grid(row=0, column=0, sticky="nsew")
        folders_frame.columnconfigure(0, weight=1)
        folders_frame.rowconfigure(0, weight=1)

        colors = getattr(self, "_colors", {})
        lb_bg = colors.get("tree_bg", colors.get("panel", "#111827"))
        lb_fg = colors.get("text", "#e5e7eb")
        lb_sel = colors.get("selection", "#1f3a8a")
        self._settings_folders_list = tk.Listbox(
            folders_frame,
            height=6,
            selectmode=tk.EXTENDED,
            background=lb_bg,
            foreground=lb_fg,
            selectbackground=lb_sel,
            selectforeground=colors.get("selection_fg", "#ffffff"),
            highlightthickness=1,
            relief="flat",
            activestyle="none",
        )
        self._settings_folders_list.grid(row=0, column=0, sticky="nsew")
        folders_scroll = ttk.Scrollbar(folders_frame, orient="vertical", command=self._settings_folders_list.yview)
        folders_scroll.grid(row=0, column=1, sticky="ns")
        self._settings_folders_list.configure(yscrollcommand=folders_scroll.set)

        try:
            for p in self._folders_list.get(0, tk.END):
                self._settings_folders_list.insert(tk.END, str(p))
        except Exception:
            pass

        def add_folder_settings():
            d = filedialog.askdirectory(title="Add folder")
            if not d:
                return
            existing = set(self._settings_folders_list.get(0, tk.END))
            if d in existing:
                return
            self._settings_folders_list.insert(tk.END, d)

        def remove_folder_settings():
            sel = list(self._settings_folders_list.curselection())
            for i in reversed(sel):
                try:
                    self._settings_folders_list.delete(i)
                except Exception:
                    pass

        side = ttk.Frame(folders_body, style="Panel.TFrame")
        side.grid(row=0, column=1, sticky="n", padx=(10, 0))
        self._btn_settings_add_folder = ttk.Button(side, text=self._t("Add Folder"), command=add_folder_settings)
        self._btn_settings_add_folder.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        self._btn_settings_rm_folder = ttk.Button(side, text=self._t("Remove Selected"), command=remove_folder_settings)
        self._btn_settings_rm_folder.grid(row=1, column=0, sticky="ew")
        self._lbl_settings_folders_hint = ttk.Label(
            side, text=self._t("These folders are scanned on the Local tab."), style="Hint.TLabel", wraplength=170,
        )
        self._lbl_settings_folders_hint.grid(row=2, column=0, sticky="w", pady=(10, 0))

        # ---- Footer -------------------------------------------------------
        footer = ttk.Frame(parent, padding=(10, 0, 10, 12))
        footer.grid(row=2, column=0, sticky="ew")
        footer.columnconfigure(0, weight=1)

        self._settings_saved_var = tk.StringVar(value="")
        self._lbl_settings_saved = ttk.Label(footer, textvariable=self._settings_saved_var, style="Muted.TLabel")
        self._lbl_settings_saved.grid(row=0, column=0, sticky="w")

        self._btn_save_settings = ttk.Button(
            footer, text=self._t("Save Settings"), command=self._save_settings_clicked, style="Accent.TButton",
        )
        self._btn_save_settings.grid(row=0, column=1, sticky="e")

    def _build_local_tab(self, parent: ttk.Frame):
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(1, weight=1)

        header = ttk.Frame(parent, padding=(12, 12, 12, 0))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        self._lbl_local_header = ttk.Label(header, text=self._t("Local ISOs"), style="Title.TLabel")
        self._lbl_local_header.grid(row=0, column=0, sticky="w")
        self._local_count_var = tk.StringVar(value="")
        self._lbl_local_count = ttk.Label(header, textvariable=self._local_count_var, style="Muted.TLabel")
        self._lbl_local_count.grid(row=0, column=1, sticky="e")
        header.columnconfigure(1, weight=0)

        content = ttk.Frame(parent, padding=10)
        content.grid(row=1, column=0, sticky="nsew")
        content.columnconfigure(0, weight=1)
        content.rowconfigure(0, weight=1)

        paned = ttk.Panedwindow(content, orient="horizontal")
        paned.grid(row=0, column=0, sticky="nsew")

        sidebar = ttk.Frame(paned, padding=(12, 12, 12, 12), style="Card.TFrame", width=240)
        main = ttk.Frame(paned, padding=(12, 12, 12, 12), style="Card.TFrame")
        paned.add(sidebar, weight=0)
        paned.add(main, weight=1)

        sidebar.columnconfigure(0, weight=1)
        self._lbl_folders = ttk.Label(sidebar, text=self._t("Folders"), style="Section.TLabel")
        self._lbl_folders.grid(row=0, column=0, sticky="w")

        folders_frame = ttk.Frame(sidebar, style="Panel.TFrame")
        folders_frame.grid(row=1, column=0, sticky="nsew", pady=(8, 10))
        folders_frame.columnconfigure(0, weight=1)
        folders_frame.rowconfigure(0, weight=1)
        sidebar.rowconfigure(1, weight=1)

        colors = getattr(self, "_colors", {})
        lb_bg = colors.get("panel", "#111827")
        lb_fg = colors.get("text", "#e5e7eb")
        lb_sel = "#1f3a8a"

        self._folders_list = tk.Listbox(
            folders_frame,
            height=8,
            selectmode=tk.EXTENDED,
            background=lb_bg,
            foreground=lb_fg,
            selectbackground=lb_sel,
            selectforeground="#ffffff",
            highlightthickness=1,
            relief="flat",
        )
        self._folders_list.grid(row=0, column=0, sticky="nsew")

        folders_scroll = ttk.Scrollbar(folders_frame, orient="vertical", command=self._folders_list.yview)
        folders_scroll.grid(row=0, column=1, sticky="ns")
        self._folders_list.configure(yscrollcommand=folders_scroll.set)

        self._btn_add_folder = ttk.Button(sidebar, text=self._t("Add Folder"), command=self._add_folder)
        self._btn_add_folder.grid(row=2, column=0, sticky="ew", pady=(0, 6))
        self._btn_remove_folder = ttk.Button(sidebar, text=self._t("Remove Selected"), command=self._remove_selected_folders)
        self._btn_remove_folder.grid(row=3, column=0, sticky="ew", pady=(0, 12))

        ttk.Separator(sidebar, orient="horizontal").grid(row=4, column=0, sticky="ew", pady=(0, 12))
        self._btn_scan = ttk.Button(sidebar, text=self._t("Scan"), command=self._start_scan, style="Accent.TButton")
        self._btn_scan.grid(row=5, column=0, sticky="ew", pady=(0, 6))
        self._btn_stop_scan = ttk.Button(sidebar, text=self._t("Stop"), command=self._stop_scan_clicked, style="Danger.TButton")
        self._btn_stop_scan.grid(row=6, column=0, sticky="ew")
        self._set_scan_running(False)

        main.columnconfigure(0, weight=1)
        main.rowconfigure(1, weight=1)

        filter_row = ttk.Frame(main, style="Panel.TFrame")
        filter_row.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        filter_row.columnconfigure(3, weight=1)

        self._lbl_filter_distro = ttk.Label(filter_row, text=self._t("Distro:"))
        self._lbl_filter_distro.grid(row=0, column=0, sticky="w")
        self._distro_filter_var = tk.StringVar(value="All")
        self._distro_filter_combo = ttk.Combobox(
            filter_row,
            textvariable=self._distro_filter_var,
            state="readonly",
            values=["All", "Ubuntu", "Debian", "Fedora", "Arch", "Kali", "Alpine", "openSUSE", "Pop!_OS", "Manjaro", "Linux Mint", "elementary OS", "Zorin OS", "Deepin", "MX Linux", "FreeBSD", "OpenBSD", "NetBSD"],
            width=16,
        )
        self._distro_filter_combo.grid(row=0, column=1, sticky="w", padx=(8, 16))
        self._distro_filter_combo.bind("<<ComboboxSelected>>", lambda _e: self._apply_filter())

        self._lbl_filter_search = ttk.Label(filter_row, text=self._t("Search:"))
        self._lbl_filter_search.grid(row=0, column=2, sticky="w")
        self._filter_var = tk.StringVar(value="")
        ent = ttk.Entry(filter_row, textvariable=self._filter_var)
        ent.grid(row=0, column=3, sticky="ew", padx=(8, 8))
        ent.bind("<KeyRelease>", lambda _e: self._apply_filter())
        self._btn_clear_filter = ttk.Button(filter_row, text=self._t("Clear"), command=self._clear_filter)
        self._btn_clear_filter.grid(row=0, column=4, sticky="e")

        mid = ttk.Frame(main, style="Panel.TFrame")
        mid.grid(row=1, column=0, sticky="nsew")
        mid.columnconfigure(0, weight=1)
        mid.rowconfigure(0, weight=1)

        cols = ("name", "info", "path", "size", "modified")
        self._tree = ttk.Treeview(mid, columns=cols, show="headings", selectmode="browse")
        self._tree.heading("name", text=self._t("Name"))
        self._tree.heading("info", text=self._t("Info"))
        self._tree.heading("path", text=self._t("Path"))
        self._tree.heading("size", text=self._t("Size"))
        self._tree.heading("modified", text=self._t("Modified"))

        self._tree.column("name", width=270, minwidth=160, anchor="w", stretch=True)
        self._tree.column("info", width=190, minwidth=120, anchor="w", stretch=False)
        self._tree.column("path", width=330, minwidth=180, anchor="w", stretch=True)
        self._tree.column("size", width=95, minwidth=80, anchor="e", stretch=False)
        self._tree.column("modified", width=140, minwidth=110, anchor="w", stretch=False)

        self._tree.grid(row=0, column=0, sticky="nsew")
        self._tree.bind("<<TreeviewSelect>>", lambda _e: self._on_selection_changed())
        self._tree.bind("<Double-1>", lambda _e: self._mount_and_open_clicked())

        tree_scroll = ttk.Scrollbar(mid, orient="vertical", command=self._tree.yview)
        tree_scroll.grid(row=0, column=1, sticky="ns")
        self._tree.configure(yscrollcommand=tree_scroll.set)

        bottom = ttk.Frame(main, style="Panel.TFrame")
        bottom.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        bottom.columnconfigure(1, weight=1)

        self._status_var = tk.StringVar(value=self._t("Ready"))
        ttk.Label(bottom, textvariable=self._status_var, style="Muted.TLabel").grid(
            row=0, column=0, sticky="w")

        self._progress = ttk.Progressbar(bottom, mode="determinate", maximum=100)
        self._progress.grid(row=0, column=1, columnspan=2, sticky="ew", padx=(12, 0))

        actions = ttk.Frame(bottom, style="Panel.TFrame")
        actions.grid(row=1, column=0, columnspan=3, sticky="e", pady=(10, 0))

        self._btn_mount_open = ttk.Button(actions, text=self._t("Mount + Open"), command=self._mount_and_open_clicked)
        self._btn_details = ttk.Button(actions, text=self._t("Show Details"), command=self._details_clicked)
        self._btn_dupes = ttk.Button(actions, text=self._t("Find Duplicates"), command=self._find_duplicates_clicked)
        self._btn_extract = ttk.Button(actions, text=self._t("Extract"), command=self._extract_clicked)
        self._btn_copy = ttk.Button(actions, text=self._t("Copy Path"), command=self._copy_path_clicked)
        self._btn_eject = ttk.Button(actions, text=self._t("Eject ISO"), command=self._eject_clicked)

        self._local_actions_frame = actions
        self._local_action_buttons = [
            self._btn_mount_open, self._btn_details, self._btn_dupes,
            self._btn_extract, self._btn_copy, self._btn_eject,
        ]
        self._local_action_columns = 0
        self._layout_local_actions()
        bottom.bind("<Configure>", self._layout_local_actions)

        self._set_actions_enabled(False)

    def _layout_local_actions(self, _event=None) -> None:
        """Keep the action buttons on one row, or wrap them when the window is narrow."""
        buttons = getattr(self, "_local_action_buttons", None)
        frame = getattr(self, "_local_actions_frame", None)
        if not buttons or frame is None:
            return
        try:
            available = frame.master.winfo_width()
            needed = sum(b.winfo_reqwidth() + 6 for b in buttons)
            columns = len(buttons) if available <= 1 or needed <= available else 3
            if columns == getattr(self, "_local_action_columns", 0):
                return
            self._local_action_columns = columns
            for index, button in enumerate(buttons):
                button.grid_forget()
                button.grid(
                    row=index // columns,
                    column=index % columns,
                    sticky="ew",
                    padx=(0, 6),
                    pady=(0, 6) if index // columns == 0 and columns < len(buttons) else 0,
                )
            for column in range(max(columns, len(buttons))):
                frame.columnconfigure(column, weight=0, uniform="")
        except Exception:
            pass

    def _restore_geometry(self, saved: str) -> str | None:
        """Apply a saved "WxH+X+Y" string, clamped so it stays usable on screen.

        Returns the geometry actually applied (None when the string is unusable).
        """
        text = str(saved or "").strip()
        match = re.match(r"^(\d+)x(\d+)(?:([+-]\d+)([+-]\d+))?$", text)
        if not match:
            return None
        try:
            screen_w = self.winfo_screenwidth()
            screen_h = self.winfo_screenheight()
        except Exception:
            screen_w, screen_h = 1920, 1080
        width = max(680, min(int(match.group(1)), max(680, screen_w - 40)))
        height = max(460, min(int(match.group(2)), max(460, screen_h - 80)))
        geometry = f"{width}x{height}"
        if match.group(3) and match.group(4):
            x = max(-width + 120, min(int(match.group(3)), max(0, screen_w - 120)))
            y = max(0, min(int(match.group(4)), max(0, screen_h - 60)))
            geometry += f"{x:+d}{y:+d}"
        self.geometry(geometry)
        return geometry

    @staticmethod
    def _wheel_units(event) -> int:
        try:
            delta = int(getattr(event, "delta", 0) or 0)
        except Exception:
            delta = 0
        if delta:
            return -1 if delta > 0 else 1
        try:
            num = int(getattr(event, "num", 0) or 0)
        except Exception:
            num = 0
        if num == 4:
            return -1
        if num == 5:
            return 1
        return 0

    def _bind_wheel(self, canvas, *extra):
        """Scroll ``canvas`` with the mouse wheel while the pointer is over it.

        ``bind_all`` is only armed on <Enter> and disarmed on <Leave> so the
        wheel keeps working over child widgets and never hijacks other tabs.
        """
        def _on_wheel(event):
            units = self._wheel_units(event)
            if not units:
                return None
            try:
                canvas.yview_scroll(units * 3, "units")
            except Exception:
                return None
            return "break"

        def _inside() -> bool:
            try:
                widget = canvas.winfo_containing(*canvas.winfo_pointerxy())
            except Exception:
                return False
            depth = 0
            while widget is not None and depth < 64:
                if widget is canvas:
                    return True
                widget = getattr(widget, "master", None)
                depth += 1
            return False

        def _on_enter(_event=None):
            canvas.bind_all("<MouseWheel>", _on_wheel)
            canvas.bind_all("<Button-4>", _on_wheel)
            canvas.bind_all("<Button-5>", _on_wheel)

        def _on_leave(_event=None):
            if _inside():
                return
            canvas.unbind_all("<MouseWheel>")
            canvas.unbind_all("<Button-4>")
            canvas.unbind_all("<Button-5>")

        for widget in (canvas,) + tuple(extra):
            widget.bind("<Enter>", _on_enter, add="+")
            widget.bind("<Leave>", _on_leave, add="+")

    def _build_internet_tab(self, parent: ttk.Frame):
        parent.columnconfigure(0, weight=1)
        parent.rowconfigure(1, weight=1)

        header = ttk.Frame(parent, padding=(12, 12, 12, 0))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(0, weight=1)
        self._lbl_internet_header = ttk.Label(header, text=self._t("Internet Sources"), style="Title.TLabel")
        self._lbl_internet_header.grid(row=0, column=0, sticky="w")

        content = ttk.Frame(parent, padding=10)
        content.grid(row=1, column=0, sticky="nsew")
        content.columnconfigure(0, weight=1)
        content.rowconfigure(0, weight=1)

        paned = ttk.Panedwindow(content, orient="horizontal")
        paned.grid(row=0, column=0, sticky="nsew")

        sidebar_holder = ttk.Frame(paned, style="Panel.TFrame")
        main = ttk.Frame(paned, padding=(10, 10, 10, 10), style="Panel.TFrame")
        paned.add(sidebar_holder, weight=0)
        paned.add(main, weight=1)

        try:
            side_bg = self._style.lookup("Panel.TFrame", "background") or "#111827"
        except Exception:
            side_bg = "#111827"
        side_canvas = tk.Canvas(
            sidebar_holder, width=250, highlightthickness=0, borderwidth=0, background=side_bg
        )
        side_scroll = ttk.Scrollbar(sidebar_holder, orient="vertical", command=side_canvas.yview)
        side_canvas.configure(yscrollcommand=side_scroll.set)
        side_canvas.pack(side="left", fill="both", expand=True)
        side_scroll.pack(side="right", fill="y")

        sidebar = ttk.Frame(side_canvas, padding=(10, 10, 10, 10), style="Panel.TFrame")
        side_window = side_canvas.create_window((0, 0), window=sidebar, anchor="nw")

        def _net_sidebar_sync(_event=None):
            try:
                side_canvas.configure(scrollregion=side_canvas.bbox("all"))
                side_canvas.itemconfigure(side_window, width=max(1, side_canvas.winfo_width()))
            except Exception:
                pass

        sidebar.bind("<Configure>", _net_sidebar_sync)
        side_canvas.bind("<Configure>", _net_sidebar_sync)
        self._bind_wheel(side_canvas, sidebar)
        self._net_sidebar_canvas = side_canvas

        sidebar.columnconfigure(0, weight=1)
        self._lbl_net_source = ttk.Label(sidebar, text=self._t("Source"), style="Muted.TLabel")
        self._lbl_net_source.grid(row=0, column=0, sticky="w")

        self._lbl_net_category = ttk.Label(sidebar, text=self._t("Category:"))
        self._lbl_net_category.grid(row=1, column=0, sticky="w", pady=(10, 0))
        self._cat_var = tk.StringVar(value="All")
        self._cat_combo = ttk.Combobox(
            sidebar,
            textvariable=self._cat_var,
            state="readonly",
            values=["All", "Windows", "Windows Server", "Archive.org", "Mainstream", "Security/Pentest", "Lightweight", "Specialty", "BSD/Other"],
        )
        self._cat_combo.grid(row=2, column=0, sticky="ew", pady=(6, 0))
        self._cat_combo.bind("<<ComboboxSelected>>", lambda _e: self._on_category_changed())

        self._lbl_net_source2 = ttk.Label(sidebar, text=self._t("Source:"))
        self._lbl_net_source2.grid(row=3, column=0, sticky="w", pady=(10, 0))
        self._source_var = tk.StringVar(value="Ubuntu")
        self._source_combo = ttk.Combobox(
            sidebar,
            textvariable=self._source_var,
            state="readonly",
            values=self._sources_for_category("All"),
        )
        self._source_combo.grid(row=4, column=0, sticky="ew", pady=(6, 0))
        self._source_combo.bind("<<ComboboxSelected>>", lambda _e: self._on_source_changed())

        self._custom_row = ttk.Frame(sidebar, style="Panel.TFrame")
        self._custom_row.grid(row=5, column=0, sticky="ew", pady=(10, 0))
        self._custom_row.columnconfigure(0, weight=1)

        self._lbl_net_iso_dir = ttk.Label(self._custom_row, text=self._t("ISO directory URL:"))
        self._lbl_net_iso_dir.grid(row=0, column=0, sticky="w")
        self._custom_base_var = tk.StringVar(value="")
        self._custom_base_entry = ttk.Entry(self._custom_row, textvariable=self._custom_base_var)
        self._custom_base_entry.grid(row=1, column=0, sticky="ew", pady=(6, 0))

        self._lbl_net_checksum = ttk.Label(self._custom_row, text=self._t("Checksum URL (optional):"))
        self._lbl_net_checksum.grid(row=2, column=0, sticky="w", pady=(10, 0))
        self._custom_sum_var = tk.StringVar(value="")
        self._custom_sum_entry = ttk.Entry(self._custom_row, textvariable=self._custom_sum_var)
        self._custom_sum_entry.grid(row=3, column=0, sticky="ew", pady=(6, 0))

        self._lbl_net_web_search = ttk.Label(sidebar, text=self._t("Web Search:"))
        self._lbl_net_web_search.grid(row=6, column=0, sticky="w", pady=(14, 0))
        self._web_query_var = tk.StringVar(value="")
        self._web_query_entry = ttk.Entry(sidebar, textvariable=self._web_query_var)
        self._web_query_entry.grid(row=7, column=0, sticky="ew", pady=(6, 0))
        self._web_query_entry.bind("<Return>", lambda _e: self._web_search_clicked())
        self._web_query_entry.bind("<Button-3>", self._show_web_search_menu)

        self._lbl_net_mode = ttk.Label(sidebar, text=self._t("Mode:"))
        self._lbl_net_mode.grid(row=8, column=0, sticky="w", pady=(10, 0))
        self._web_mode_var = tk.StringVar(value="Any Website")
        self._web_mode_combo = ttk.Combobox(
            sidebar,
            textvariable=self._web_mode_var,
            state="readonly",
            values=["Any Website", "ISO-focused"],
        )
        self._web_mode_combo.grid(row=9, column=0, sticky="ew", pady=(6, 0))

        self._lbl_net_provider = ttk.Label(sidebar, text=self._t("Provider:"))
        self._lbl_net_provider.grid(row=10, column=0, sticky="w", pady=(10, 0))
        self._web_provider_var = tk.StringVar(value="DuckDuckGo")
        self._web_provider_combo = ttk.Combobox(
            sidebar,
            textvariable=self._web_provider_var,
            state="readonly",
            values=["DuckDuckGo", "SearxNG", "Google API"],
        )
        self._web_provider_combo.grid(row=11, column=0, sticky="ew", pady=(6, 0))

        self._deterministic_first_var = tk.StringVar(value="")
        det_cb = ttk.Checkbutton(
            sidebar,
            text="Deterministic first",
            variable=self._deterministic_first_var,
            onvalue="1",
            offvalue="",
        )
        det_cb.grid(row=12, column=0, sticky="w", pady=(8, 0))

        self._web_archive_level_var = tk.StringVar(value="0")
        lvl_row = ttk.Frame(sidebar, style="Panel.TFrame")
        lvl_row.grid(row=13, column=0, sticky="ew", pady=(8, 0))
        lvl_row.columnconfigure(1, weight=1)
        ttk.Label(lvl_row, text="Archive level:").grid(row=0, column=0, sticky="w")
        lvl = ttk.Combobox(lvl_row, textvariable=self._web_archive_level_var, values=["0", "1", "2", "3"], width=5, state="readonly")
        lvl.grid(row=0, column=1, sticky="e")

        self._provider_row = ttk.Frame(sidebar, style="Panel.TFrame")
        self._provider_row.grid(row=14, column=0, sticky="ew", pady=(6, 0))
        self._provider_row.columnconfigure(0, weight=1)

        self._lbl_net_searx = ttk.Label(self._provider_row, text=self._t("SearxNG URL:"))
        self._lbl_net_searx.grid(row=0, column=0, sticky="w")
        self._searx_url_var = tk.StringVar(value="")
        self._searx_url_entry = ttk.Entry(self._provider_row, textvariable=self._searx_url_var)
        self._searx_url_entry.grid(row=1, column=0, sticky="ew", pady=(6, 0))

        self._lbl_net_gkey = ttk.Label(self._provider_row, text=self._t("Google API key:"))
        self._lbl_net_gkey.grid(row=2, column=0, sticky="w", pady=(10, 0))
        self._google_key_var = tk.StringVar(value="")
        self._google_key_entry = ttk.Entry(self._provider_row, textvariable=self._google_key_var, show="•")
        self._google_key_entry.grid(row=3, column=0, sticky="ew", pady=(6, 0))

        self._lbl_net_gcx = ttk.Label(self._provider_row, text=self._t("Google CSE ID (cx):"))
        self._lbl_net_gcx.grid(row=4, column=0, sticky="w", pady=(10, 0))
        self._google_cx_var = tk.StringVar(value="")
        self._google_cx_entry = ttk.Entry(self._provider_row, textvariable=self._google_cx_var)
        self._google_cx_entry.grid(row=5, column=0, sticky="ew", pady=(6, 0))

        def on_provider_changed(*_args):
            provider = (self._web_provider_var.get() or "DuckDuckGo").strip()
            if provider == "SearxNG":
                self._provider_row.grid()
                self._searx_url_entry.grid()
                self._google_key_entry.grid_remove()
                self._google_cx_entry.grid_remove()
            elif provider == "Google API":
                self._provider_row.grid()
                self._searx_url_entry.grid_remove()
                self._google_key_entry.grid()
                self._google_cx_entry.grid()
            else:
                self._provider_row.grid_remove()

        self._web_provider_var.trace_add("write", on_provider_changed)
        on_provider_changed()

        prof_row = ttk.Frame(sidebar, style="Panel.TFrame")
        prof_row.grid(row=15, column=0, sticky="ew", pady=(10, 0))
        self._btn_save_profile = ttk.Button(prof_row, text="Save Profile", command=self._save_provider_profile)
        self._btn_apply_profile = ttk.Button(prof_row, text="Apply Profile", command=self._apply_provider_profile)
        self._btn_save_profile.grid(row=0, column=0, sticky="ew", padx=(0, 6))
        self._btn_apply_profile.grid(row=0, column=1, sticky="ew")

        self._btn_web_search = ttk.Button(sidebar, text=self._t("Search Web"), command=self._web_search_clicked)
        self._btn_web_search.grid(row=16, column=0, sticky="ew", pady=(10, 0))

        ttk.Separator(sidebar, orient="horizontal").grid(row=17, column=0, sticky="ew", pady=(14, 10))

        self._btn_refresh_list = ttk.Button(sidebar, text=self._t("Refresh List"), command=self._refresh_remote_clicked, style="Accent.TButton")
        self._btn_refresh_list.grid(row=18, column=0, sticky="ew", pady=(0, 6))

        self._btn_open_source = ttk.Button(sidebar, text=self._t("Open Source Page"), command=self._open_source_page_clicked)
        self._btn_open_source.grid(row=19, column=0, sticky="ew", pady=(0, 12))

        self._btn_download_url = ttk.Button(sidebar, text=self._t("Download URL"), command=self._download_from_url_clicked)
        self._btn_download_url.grid(row=20, column=0, sticky="ew", pady=(0, 6))

        self._auto_verify_var = tk.StringVar(value="1")
        self._chk_auto_verify = ttk.Checkbutton(
            sidebar,
            text="Auto-verify (SHA-256)",
            variable=self._auto_verify_var,
            onvalue="1",
            offvalue="",
        )
        self._chk_auto_verify.grid(row=21, column=0, sticky="w", pady=(0, 10))

        self._btn_audit = ttk.Button(sidebar, text=self._t("Audit Sources"), command=self._audit_sources_clicked)
        self._btn_audit.grid(row=22, column=0, sticky="ew", pady=(0, 6))

        self._btn_stop_download = ttk.Button(sidebar, text=self._t("Stop"), command=self._stop_download_clicked, style="Danger.TButton")
        self._btn_stop_download.grid(row=23, column=0, sticky="ew")

        sidebar.rowconfigure(24, weight=1)

        main.columnconfigure(0, weight=1)
        main.rowconfigure(1, weight=1)

        filter_row = ttk.Frame(main, style="Panel.TFrame")
        filter_row.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        filter_row.columnconfigure(1, weight=1)

        self._lbl_net_search = ttk.Label(filter_row, text=self._t("Search:"))
        self._lbl_net_search.grid(row=0, column=0, sticky="w")
        self._net_query_var = tk.StringVar(value="")
        ent = ttk.Entry(filter_row, textvariable=self._net_query_var)
        ent.grid(row=0, column=1, sticky="ew", padx=(8, 8))
        ent.bind("<KeyRelease>", lambda _e: self._apply_remote_filter())

        self._remote_page_size_var = tk.StringVar(value="200")
        page = ttk.Combobox(filter_row, textvariable=self._remote_page_size_var, values=["50", "100", "200", "500", "1000"], width=7, state="readonly")
        page.grid(row=0, column=2, sticky="e")

        def _on_page_size_changed(*_args):
            try:
                step = int((self._remote_page_size_var.get() or "200").strip())
            except Exception:
                step = 200
            if step <= 0:
                step = 200
            self._remote_display_limit_step = step
            # Reset the current display limit to the selected page size.
            self._remote_display_limit = step
            self._apply_remote_filter()

        self._remote_page_size_var.trace_add("write", _on_page_size_changed)

        self._remote_display_limit_step = 200
        self._remote_display_limit = self._remote_display_limit_step

        mid = ttk.Frame(main, style="Panel.TFrame")
        mid.grid(row=1, column=0, sticky="nsew")
        mid.columnconfigure(0, weight=1)
        mid.rowconfigure(0, weight=1)

        cols = ("name", "url", "sha256")
        self._remote_tree = ttk.Treeview(mid, columns=cols, show="headings", selectmode="extended")
        self._remote_tree.heading("name", text=self._t("Name"))
        self._remote_tree.heading("url", text=self._t("URL"))
        self._remote_tree.heading("sha256", text=self._t("SHA-256"))
        self._remote_tree.column("name", width=260, anchor="w")
        self._remote_tree.column("url", width=560, anchor="w")
        self._remote_tree.column("sha256", width=140, anchor="w")
        self._remote_tree.grid(row=0, column=0, sticky="nsew")
        self._remote_tree.bind("<<TreeviewSelect>>", lambda _e: self._on_remote_selection_changed())
        self._remote_tree.bind("<Double-1>", lambda _e: self._download_clicked())

        tree_scroll = ttk.Scrollbar(mid, orient="vertical", command=self._remote_tree.yview)
        tree_scroll.grid(row=0, column=1, sticky="ns")
        self._remote_tree.configure(yscrollcommand=tree_scroll.set)

        bottom = ttk.Frame(main, style="Panel.TFrame")
        bottom.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        bottom.columnconfigure(1, weight=1)

        self._net_status_var = tk.StringVar(value=self._t("Pick a source and click Refresh"))
        ttk.Label(bottom, textvariable=self._net_status_var).grid(row=0, column=0, sticky="w")

        self._net_progress = ttk.Progressbar(bottom, mode="determinate", maximum=100)
        self._net_progress.grid(row=0, column=1, sticky="ew", padx=(10, 10))

        actions = ttk.Frame(bottom)
        actions.grid(row=0, column=2, sticky="e")

        self._btn_load_more_remote = ttk.Button(actions, text=self._t("Load more"), command=self._load_more_remote_clicked)
        self._btn_download = ttk.Button(actions, text=self._t("Download"), command=self._download_clicked, style="Accent.TButton")
        self._btn_copy_url = ttk.Button(actions, text=self._t("Copy URL"), command=self._copy_url_clicked)
        self._btn_validate = ttk.Button(actions, text=self._t("Validate"), command=self._validate_remote_clicked)
        self._btn_load_more_remote.grid(row=0, column=0, padx=(0, 6))
        self._btn_download.grid(row=0, column=1, padx=(0, 6))
        self._btn_copy_url.grid(row=0, column=2, padx=(0, 6))
        self._btn_validate.grid(row=0, column=3, padx=(0, 6))

        downloads_panel = ttk.Frame(main, style="Panel.TFrame")
        downloads_panel.grid(row=3, column=0, sticky="nsew", pady=(10, 0))
        downloads_panel.columnconfigure(0, weight=1)
        downloads_panel.rowconfigure(1, weight=1)

        jobs_head = ttk.Frame(downloads_panel, style="Panel.TFrame")
        jobs_head.grid(row=0, column=0, sticky="ew", pady=(0, 6))
        jobs_head.columnconfigure(1, weight=1)
        ttk.Label(jobs_head, text="Downloads:").grid(row=0, column=0, sticky="w")
        self._jobs_summary_var = tk.StringVar(value=self._t("No downloads yet"))
        self._jobs_summary_label = ttk.Label(jobs_head, textvariable=self._jobs_summary_var, style="Status.TLabel")
        self._jobs_summary_label.grid(row=0, column=1, sticky="e")

        jobs_mid = ttk.Frame(downloads_panel, style="Panel.TFrame")
        jobs_mid.grid(row=1, column=0, sticky="nsew")
        jobs_mid.columnconfigure(0, weight=1)
        jobs_mid.rowconfigure(0, weight=1)

        job_cols = ("job", "status", "progress", "size", "speed", "eta")
        self._jobs_tree = ttk.Treeview(jobs_mid, columns=job_cols, show="headings", selectmode="extended", height=6)
        for key, title in (("job", "Job"), ("status", "Status"), ("progress", "Progress"),
                           ("size", "Size"), ("speed", "Speed"), ("eta", "Left")):
            self._jobs_tree.heading(key, text=self._t(title))
        self._jobs_tree.column("job", width=240, minwidth=150, anchor="w", stretch=True)
        self._jobs_tree.column("status", width=110, minwidth=90, anchor="w", stretch=False)
        self._jobs_tree.column("progress", width=205, minwidth=180, anchor="w", stretch=False)
        self._jobs_tree.column("size", width=160, minwidth=135, anchor="e", stretch=False)
        self._jobs_tree.column("speed", width=95, minwidth=80, anchor="e", stretch=False)
        self._jobs_tree.column("eta", width=80, minwidth=70, anchor="e", stretch=False)
        self._jobs_tree.grid(row=0, column=0, sticky="nsew")
        self._jobs_tree.bind("<<TreeviewSelect>>", lambda _e: self._on_job_selection_changed())
        self._jobs_tree.bind("<Double-1>", lambda _e: self._open_job_folder_clicked())
        self._jobs_tree.bind("<Delete>", lambda _e: self._remove_job_clicked())
        self._jobs_tree.bind("<Button-3>", self._show_job_menu)
        self._jobs_tree.bind("<Button-2>", self._show_job_menu)

        jobs_scroll = ttk.Scrollbar(jobs_mid, orient="vertical", command=self._jobs_tree.yview)
        jobs_scroll.grid(row=0, column=1, sticky="ns")
        self._jobs_tree.configure(yscrollcommand=jobs_scroll.set)

        jobs_foot = ttk.Frame(downloads_panel, style="Panel.TFrame")
        jobs_foot.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        jobs_foot.columnconfigure(0, weight=1)

        self._jobs_detail_var = tk.StringVar(value="")
        self._jobs_detail_label = ttk.Label(
            jobs_foot, textvariable=self._jobs_detail_var, style="Status.TLabel", anchor="w",
        )
        self._jobs_detail_label.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(6, 0))

        job_actions = ttk.Frame(jobs_foot)
        job_actions.grid(row=0, column=1, sticky="e")
        self._btn_job_pause = ttk.Button(job_actions, text=self._t("Pause"), command=self._pause_job_clicked)
        self._btn_job_resume = ttk.Button(job_actions, text=self._t("Resume"), command=self._resume_job_clicked)
        self._btn_job_retry = ttk.Button(job_actions, text=self._t("Retry"), command=self._retry_job_clicked)
        self._btn_job_open = ttk.Button(job_actions, text=self._t("Open folder"), command=self._open_job_folder_clicked)
        self._btn_job_remove = ttk.Button(job_actions, text=self._t("Remove"), command=self._remove_job_clicked)
        self._btn_job_clear = ttk.Button(job_actions, text=self._t("Clear finished"), command=self._clear_finished_jobs_clicked)
        for index, button in enumerate((self._btn_job_pause, self._btn_job_resume, self._btn_job_retry,
                                        self._btn_job_open, self._btn_job_remove, self._btn_job_clear)):
            button.grid(row=0, column=index, padx=(0, 6) if index < 5 else 0)
        self._set_job_actions_enabled(False)

        self._set_remote_actions_enabled(False)
        self._on_source_changed()

        try:
            main.rowconfigure(3, weight=0)
        except Exception:
            pass

    # ---- download list helpers -------------------------------------------
    @staticmethod
    def _progress_bar(frac: float, width: int = 12) -> str:
        """A text progress bar - Treeview cannot host a real widget."""
        frac = max(0.0, min(1.0, float(frac or 0.0)))
        filled = int(round(frac * width))
        return "█" * filled + "░" * (width - filled) + f" {frac * 100:5.1f}%"

    @staticmethod
    def _format_speed(bytes_per_second: float | None) -> str:
        if not bytes_per_second or bytes_per_second <= 0:
            return ""
        return f"{human_bytes(int(bytes_per_second))}/s"

    @staticmethod
    def _format_eta(seconds: float | None) -> str:
        if seconds is None or seconds < 0 or seconds > 86400 * 2:
            return ""
        seconds = int(seconds)
        if seconds < 60:
            return f"{seconds}s"
        if seconds < 3600:
            return f"{seconds // 60}m {seconds % 60:02d}s"
        return f"{seconds // 3600}h {(seconds % 3600) // 60:02d}m"

    def _job_row_values(self, job: dict) -> tuple:
        frac = float(job.get("progress") or 0.0)
        done = job.get("done_bytes")
        total = job.get("total_bytes")
        if done is not None and total:
            size = f"{human_bytes(int(done))} / {human_bytes(int(total))}"
        elif done is not None:
            size = human_bytes(int(done))
        else:
            size = ""
        status = str(job.get("status") or "")
        speed = self._format_speed(job.get("speed")) if status == "Downloading" else ""
        eta = self._format_eta(job.get("eta")) if status == "Downloading" else ""
        return (
            str(job.get("label") or job.get("job_id") or ""),
            status,
            self._progress_bar(frac),
            size,
            speed,
            eta,
        )

    _JOB_TAGS = {
        "Queued": "job_queued", "Downloading": "job_running", "Paused": "job_paused",
        "Done": "job_done", "Failed": "job_failed", "Cancelled": "job_failed",
    }

    def _refresh_job_row(self, job_id: str) -> None:
        job = self._download_jobs.get(job_id)
        tree = getattr(self, "_jobs_tree", None)
        if job is None or tree is None:
            return
        try:
            if not tree.winfo_exists():
                return
            values = self._job_row_values(job)
            tag = self._JOB_TAGS.get(str(job.get("status") or ""), "")
            if job_id in tree.get_children():
                tree.item(job_id, values=values, tags=(tag,) if tag else ())
            else:
                tree.insert("", tk.END, iid=job_id, values=values, tags=(tag,) if tag else ())
        except Exception:
            pass
        self._on_job_selection_changed()

    def _paint_job_tags(self) -> None:
        """Colour the job rows from the current palette."""
        tree = getattr(self, "_jobs_tree", None)
        colors = getattr(self, "_colors", {})
        if tree is None or not colors:
            return
        try:
            if not tree.winfo_exists():
                return
            tree.tag_configure("job_queued", foreground=colors.get("muted"))
            tree.tag_configure("job_running", foreground=colors.get("accent"))
            tree.tag_configure("job_paused", foreground=colors.get("muted"))
            tree.tag_configure("job_done", foreground=colors.get("ok", colors.get("accent")))
            tree.tag_configure("job_failed", foreground=colors.get("danger"))
        except Exception:
            pass

    def _show_job_menu(self, event) -> None:
        """Right-click menu with the same actions as the buttons."""
        tree = getattr(self, "_jobs_tree", None)
        if tree is None:
            return
        try:
            row = tree.identify_row(event.y)
            if row and row not in tree.selection():
                tree.selection_set(row)
            self._on_job_selection_changed()
        except Exception:
            pass

        menu = tk.Menu(self, tearoff=0)
        try:
            colors = getattr(self, "_menu_colors", None)
            if colors:
                menu.configure(**colors)
        except Exception:
            pass

        def add(label: str, command, button_name: str) -> None:
            button = getattr(self, button_name, None)
            state = "normal"
            try:
                if button is not None and str(button.cget("state")) == "disabled":
                    state = "disabled"
            except Exception:
                pass
            menu.add_command(label=self._t(label), command=command, state=state)

        add("Pause", self._pause_job_clicked, "_btn_job_pause")
        add("Resume", self._resume_job_clicked, "_btn_job_resume")
        add("Retry", self._retry_job_clicked, "_btn_job_retry")
        menu.add_separator()
        add("Copy URL", self._copy_job_url_clicked, "_btn_job_open")
        add("Open folder", self._open_job_folder_clicked, "_btn_job_open")
        menu.add_separator()
        add("Remove", self._remove_job_clicked, "_btn_job_remove")
        add("Clear finished", self._clear_finished_jobs_clicked, "_btn_job_clear")
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            try:
                menu.grab_release()
            except Exception:
                pass

    def _copy_job_url_clicked(self) -> None:
        urls = []
        for job_id in self._selected_job_ids():
            job = self._download_jobs.get(job_id) or {}
            url = str(job.get("url") or "")
            if url:
                urls.append(url)
        if not urls:
            return
        try:
            self.clipboard_clear()
            self.clipboard_append("\n".join(urls))
            self._set_net_status(self._t("URL copied"))
        except Exception:
            pass

    def _update_jobs_summary(self) -> None:
        """One line above the list: how many jobs are in which state."""
        var = getattr(self, "_jobs_summary_var", None)
        if var is None:
            return
        jobs = list(getattr(self, "_download_jobs", {}).values())
        if not jobs:
            var.set(self._t("No downloads yet"))
            return

        counts: dict[str, int] = {}
        total_speed = 0.0
        remaining = 0
        for job in jobs:
            status = str(job.get("status") or "Queued")
            counts[status] = counts.get(status, 0) + 1
            if status == "Downloading":
                try:
                    total_speed += float(job.get("speed") or 0.0)
                except Exception:
                    pass
                try:
                    total = job.get("total_bytes")
                    done = job.get("done_bytes") or 0
                    if total:
                        remaining += max(0, int(total) - int(done))
                except Exception:
                    pass

        order = ("Downloading", "Queued", "Paused", "Done", "Failed", "Cancelled")
        parts = [f"{counts[s]} {self._t(s.lower())}" for s in order if counts.get(s)]
        line = " · ".join(parts)
        if total_speed > 0:
            line += f" · {self._format_speed(total_speed)}"
            if remaining:
                line += f" · {self._format_eta(remaining / total_speed)} {self._t('left')}"
        var.set(line)

    # ---- remembering downloads across restarts ---------------------------
    _DOWNLOAD_STATE_VERSION = 1
    _SAVED_JOB_KEYS = (
        "job_id", "url", "target", "sha256", "label", "status", "progress",
        "done_bytes", "total_bytes", "verify", "note",
    )

    @property
    def _downloads_state_path(self) -> Path:
        return Path.home() / ".ipm_downloads.json"

    def _job_to_dict(self, job: dict) -> dict:
        out = {}
        for key in self._SAVED_JOB_KEYS:
            value = job.get(key)
            if isinstance(value, (str, int, float, bool)) or value is None:
                out[key] = value
        # A job that was running when the app closed comes back paused, so the
        # user decides when to hit the network again.
        if str(out.get("status")) in ("Downloading", "Queued"):
            out["status"] = "Paused"
        return out

    def _save_download_jobs(self, *, force: bool = False) -> None:
        """Write the job list to disk so a restart does not lose progress."""
        now = time.monotonic()
        if not force and now - float(getattr(self, "_downloads_saved_at", 0.0)) < 2.0:
            return
        self._downloads_saved_at = now

        jobs = []
        for job in getattr(self, "_download_jobs", {}).values():
            try:
                jobs.append(self._job_to_dict(job))
            except Exception:
                continue
        data = {"version": self._DOWNLOAD_STATE_VERSION, "jobs": jobs}
        try:
            self._downloads_state_path.write_text(
                json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8",
            )
        except Exception:
            pass

    def _load_download_jobs(self) -> None:
        """Bring back the jobs from the last run, paused and ready to resume."""
        path = self._downloads_state_path
        try:
            if not path.exists():
                return
            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            return
        if not isinstance(data, dict):
            return

        restored = 0
        unfinished = 0
        highest = 0
        for raw in (data.get("jobs") or []):
            if not isinstance(raw, dict):
                continue
            url = str(raw.get("url") or "")
            target = str(raw.get("target") or "")
            if not url.lower().startswith(("http://", "https://")) or not target:
                continue

            job_id = str(raw.get("job_id") or "")
            if not job_id or job_id in self._download_jobs:
                continue
            try:
                highest = max(highest, int(str(job_id).lstrip("dl") or 0))
            except Exception:
                pass

            status = str(raw.get("status") or "Paused")
            if status not in ("Paused", "Done", "Failed", "Cancelled"):
                status = "Paused"

            # Trust the file on disk over the saved counter.
            done_bytes = None
            try:
                partial = Path(target)
                if partial.exists():
                    done_bytes = int(partial.stat().st_size)
                elif status == "Done":
                    continue  # the finished file is gone, no point listing it
            except Exception:
                done_bytes = None

            total_bytes = raw.get("total_bytes")
            try:
                total_bytes = int(total_bytes) if total_bytes else None
            except Exception:
                total_bytes = None

            if done_bytes is not None and total_bytes:
                progress = max(0.0, min(1.0, done_bytes / total_bytes))
            else:
                try:
                    progress = float(raw.get("progress") or 0.0)
                except Exception:
                    progress = 0.0

            job = {
                "job_id": job_id,
                "url": url,
                "target": target,
                "sha256": (str(raw.get("sha256")).lower() if raw.get("sha256") else None),
                "label": str(raw.get("label") or Path(target).name or job_id),
                "retries_left": 2,
                "pause_event": threading.Event(),
                "status": status,
                "progress": progress,
                "done_bytes": done_bytes,
                "total_bytes": total_bytes,
                "verify": bool(raw.get("verify")),
                "note": (str(raw.get("note")) if raw.get("note") else None),
                "in_queue": False,
                "restored": True,
            }
            if status == "Paused":
                job["pause_event"].set()
                unfinished += 1

            self._download_jobs[job_id] = job
            self._refresh_job_row(job_id)
            restored += 1

        if restored:
            try:
                self._download_job_seq = max(int(self._download_job_seq or 0), highest)
            except Exception:
                pass
            self._update_jobs_summary()
            if unfinished:
                self._log(f"Restored {unfinished} unfinished download(s) - select one and press Resume")
                try:
                    self._jobs_detail_var.set(
                        self._t("Unfinished downloads from last time were restored - press Resume to continue.")
                    )
                except Exception:
                    pass
            else:
                self._log(f"Restored {restored} download(s) from the last session")

    def _requeue_job(self, job: dict) -> bool:
        """Hand a job back to the download worker."""
        try:
            self._download_queue.put(job)
            with self._download_queue_lock:
                self._download_queue_count += 1
            job["in_queue"] = True
            return True
        except Exception:
            return False

    def _selected_job_ids(self) -> list[str]:
        try:
            return [str(item) for item in self._jobs_tree.selection()]
        except Exception:
            return []

    def _retry_job_clicked(self) -> None:
        for job_id in self._selected_job_ids():
            job = self._download_jobs.get(job_id)
            if not job or str(job.get("status")) not in ("Failed", "Cancelled"):
                continue
            job["retries_left"] = 2
            job["status"] = "Queued"
            job["progress"] = 0.0
            job["cancelled"] = False
            try:
                event = job.get("pause_event")
                if event is not None:
                    event.clear()
                self._requeue_job(job)
                self._log(f"Retrying download: {job.get('label')}")
            except Exception:
                continue
            self._refresh_job_row(job_id)
        self._save_download_jobs(force=True)

    def _open_job_folder_clicked(self) -> None:
        for job_id in self._selected_job_ids()[:1]:
            job = self._download_jobs.get(job_id) or {}
            target = str(job.get("target") or "")
            if not target:
                continue
            folder = Path(target).parent
            try:
                open_explorer(folder)
            except Exception as exc:
                self._log(f"Could not open {folder}: {exc}")

    def _remove_job_clicked(self) -> None:
        for job_id in self._selected_job_ids():
            job = self._download_jobs.get(job_id)
            if job is None:
                continue
            if str(job.get("status")) == "Downloading":
                messagebox.showinfo(
                    self._t("Downloads"),
                    self._t("Pause or stop the download before removing it from the list."),
                    parent=self,
                )
                continue
            job["cancelled"] = True
            self._download_jobs.pop(job_id, None)
            try:
                self._jobs_tree.delete(job_id)
            except Exception:
                pass
        self._on_job_selection_changed()
        self._update_jobs_summary()
        self._save_download_jobs(force=True)

    def _clear_finished_jobs_clicked(self) -> None:
        for job_id, job in list(self._download_jobs.items()):
            if str(job.get("status")) in ("Done", "Failed", "Cancelled"):
                self._download_jobs.pop(job_id, None)
                try:
                    self._jobs_tree.delete(job_id)
                except Exception:
                    pass
        self._on_job_selection_changed()
        self._update_jobs_summary()
        self._save_download_jobs(force=True)

    def _selected_job_id(self) -> str | None:
        try:
            sel = list(self._jobs_tree.selection())
        except Exception:
            sel = []
        if not sel:
            return None
        return str(sel[0])

    def _set_job_actions_enabled(self, enabled: bool) -> None:
        state = "normal" if enabled else "disabled"
        for b in (getattr(self, "_btn_job_pause", None), getattr(self, "_btn_job_resume", None),
                  getattr(self, "_btn_job_retry", None), getattr(self, "_btn_job_open", None),
                  getattr(self, "_btn_job_remove", None)):
            if not b:
                continue
            try:
                b.configure(state=state)
            except Exception:
                pass

    def _on_job_selection_changed(self) -> None:
        """Only offer the buttons that make sense for the current selection."""
        selected = [self._download_jobs.get(jid) for jid in self._selected_job_ids()]
        selected = [job for job in selected if job]
        statuses = set()
        for job in selected:
            statuses.add(str(job.get("status") or ""))
            try:
                event = job.get("pause_event")
                if event is not None and event.is_set():
                    statuses.add("Paused")
            except Exception:
                pass

        def enable(name: str, on: bool) -> None:
            button = getattr(self, name, None)
            if not button:
                return
            try:
                button.configure(state="normal" if on else "disabled")
            except Exception:
                pass

        enable("_btn_job_pause", bool(statuses & {"Downloading", "Queued"}))
        enable("_btn_job_resume", bool(statuses & {"Paused"}))
        enable("_btn_job_retry", bool(statuses & {"Failed", "Cancelled"}))
        enable("_btn_job_open", len(selected) == 1)
        enable("_btn_job_remove", bool(selected) and "Downloading" not in statuses)
        enable("_btn_job_clear", any(
            str(job.get("status") or "") in ("Done", "Failed", "Cancelled")
            for job in getattr(self, "_download_jobs", {}).values()
        ))
        self._update_job_detail(selected)

    def _update_job_detail(self, selected: list) -> None:
        """The line under the list: full path / error for the selected job."""
        var = getattr(self, "_jobs_detail_var", None)
        if var is None:
            return
        if len(selected) != 1:
            var.set(f"{len(selected)} " + self._t("selected") if selected else "")
            return
        job = selected[0]
        note = str(job.get("note") or "")
        target = str(job.get("target") or "")
        text = target
        if note:
            text = f"{target}  -  {note}" if target else note
        if len(text) > 160:
            text = text[:157] + "..."
        var.set(text)

    def _pause_job_clicked(self) -> None:
        for jid in self._selected_job_ids():
            job = self._download_jobs.get(jid)
            if not job or str(job.get("status")) not in ("Downloading", "Queued"):
                continue
            try:
                ev = job.get("pause_event")
                if ev is not None:
                    ev.set()
                else:
                    continue
                job["status"] = "Paused"
                job["speed"] = None
                job["eta"] = None
                self._work_q.put(("download_job_update", {
                    "job_id": jid, "status": "Paused", "speed": None, "eta": None,
                }, None))
            except Exception:
                pass

    def _resume_job_clicked(self) -> None:
        for jid in self._selected_job_ids():
            job = self._download_jobs.get(jid)
            if not job:
                continue
            ev = job.get("pause_event")
            # The event is the truth: a late progress message may have relabelled
            # the row, but the worker is still waiting on it.
            if str(job.get("status")) != "Paused" and not (ev is not None and ev.is_set()):
                continue
            try:
                if ev is not None:
                    ev.clear()
                # A job restored from disk is not in the worker queue any more.
                if not job.get("in_queue"):
                    self._requeue_job(job)
                # the worker sets "Downloading" once it picks the job up again
                job["status"] = "Queued"
                self._work_q.put(("download_job_update", {"job_id": jid, "status": "Queued"}, None))
            except Exception:
                pass
        self._save_download_jobs(force=True)

    def _sources_for_category(self, cat: str) -> list[str]:
        linux_mainstream = [
            "Ubuntu",
            "Ubuntu Server",
            "Ubuntu (Kubuntu)",
            "Ubuntu (Xubuntu)",
            "Ubuntu (Lubuntu)",
            "Ubuntu (Ubuntu MATE)",
            "Ubuntu (Ubuntu Budgie)",
            "Ubuntu (Ubuntu Studio)",
            "Ubuntu (Ubuntu Kylin)",
            "Debian",
            "Debian Live",
            "Fedora",
            "openSUSE Leap",
            "openSUSE Tumbleweed",
            "Pop!_OS",
            "Manjaro",
            "Linux Mint",
            "MX Linux",
            "Deepin",
            "Arch Linux",
            "Mageia",
            "Gentoo",
            "Slackware",
            "Rocky Linux",
            "AlmaLinux",
            "CentOS Stream",
        ]
        linux_security = [
            "Kali Linux",
            "Parrot OS",
            "SystemRescue",
            "Rescuezilla",
            "GParted Live",
            "Clonezilla",
        ]
        linux_lightweight = [
            "Alpine Linux",
            "Void Linux",
            "Tiny Core Linux",
        ]
        linux_specialty = [
            "EndeavourOS",
            "Garuda Linux",
            "KDE neon",
            "NixOS",
            "Puppy Linux",
            "Tails",
            "elementary OS",
            "Zorin OS",
        ]
        bsd_other = [
            "FreeBSD",
            "OpenBSD",
            "NetBSD",
            "Linux Kernel (kernel.org)",
            "Windows 11 (Microsoft)",
            "Windows 10 (Microsoft)",
            "Custom Source",
        ]
        windows_sources = list(WINDOWS_SOURCES)
        server_sources = list(SERVER_SOURCES)
        archive_sources = [s for s in IA_SOURCES if s not in windows_sources]

        def _sorted_sources(src: list[str]) -> list[str]:
            custom = [s for s in src if s == "Custom Source"]
            rest = [s for s in src if s != "Custom Source"]
            rest.sort(key=lambda s: s.lower())
            return rest + custom

        if cat == "Mainstream":
            return _sorted_sources(linux_mainstream + ["Custom Source"])
        if cat == "Security/Pentest":
            return _sorted_sources(linux_security + ["Custom Source"])
        if cat == "Lightweight":
            return _sorted_sources(linux_lightweight + ["Custom Source"])
        if cat == "Specialty":
            return _sorted_sources(linux_specialty + ["Custom Source"])
        if cat == "BSD/Other":
            # Keep as curated order, but ensure Custom Source remains last.
            return _sorted_sources(bsd_other)
        if cat == "Windows Server":
            # Server releases only, newest first; Custom Source stays last.
            return server_sources + ["Custom Source"]

        if cat == "Windows":
            # Keep release order (newest first); Custom Source stays last.
            return windows_sources + ["Custom Source"]

        if cat == "Archive.org":
            # Curated catalogue order (grouped by OS family), Custom Source last.
            return list(archive_sources) + ["Custom Source"]

        # All
        merged = (
            linux_mainstream
            + linux_security
            + linux_lightweight
            + linux_specialty
            + bsd_other
            + windows_sources
            + archive_sources
        )
        seen: set[str] = set()
        unique: list[str] = []
        for name in merged:
            if name not in seen:
                seen.add(name)
                unique.append(name)
        return _sorted_sources(unique)

    def _on_category_changed(self):
        cat = self._cat_var.get()
        values = self._sources_for_category(cat)
        self._source_combo.configure(values=values)
        if self._source_var.get() not in values:
            self._source_var.set(values[0] if values else "")
        self._on_source_changed()

    def _on_source_changed(self):
        is_custom = self._source_var.get() == "Custom Source"
        if is_custom:
            self._custom_row.grid()
        else:
            self._custom_row.grid_remove()
        try:
            if hasattr(self, "_net_status_var") and not is_custom and is_ia_source(self._source_var.get()):
                self._set_net_status(LICENSE_NOTE)
        except Exception:
            pass

    def _source_page_url(self, source: str) -> str | None:
        if source in ("Windows 11 (Microsoft)", "Windows 11"):
            return "https://www.microsoft.com/software-download/windows11"
        if source in ("Windows 10 (Microsoft)", "Windows 10"):
            return "https://www.microsoft.com/software-download/windows10"
        # LTSC editions are not on Microsoft's consumer download pages; the
        # preserved media lives in the Internet Archive catalogue.
        if source in ("Windows 11 Enterprise LTSC", "Windows 11 IoT Enterprise LTSC"):
            return "https://archive.org/search?query=windows+11+ltsc+iso"
        if source in ("Windows 10 Enterprise LTSC", "Windows 10 IoT Enterprise LTSC"):
            return "https://archive.org/search?query=windows+10+ltsc+iso"
        if source in ("Windows 8.1", "Windows 8"):
            return "https://www.microsoft.com/software-download/windows8"
        if source == "Windows 7":
            return "https://archive.org/search?query=windows+7+iso"
        if source == "Windows Vista":
            return "https://archive.org/search?query=windows+vista+iso"
        if source == "Windows XP":
            return "https://archive.org/search?query=windows+xp+iso"
        # Windows Server is published as an evaluation build on the Eval Center
        # (2025 downwards to 2012 R2); the retired releases are not offered by
        # Microsoft any more and fall back to the preserved Archive items.
        if source == "Windows Server 2025":
            return ("https://www.microsoft.com/en-us/evalcenter/"
                    "download-windows-server-2025")
        if source in ("Windows Server 2022", "Windows Server 2019",
                      "Windows Server 2016"):
            return ("https://www.microsoft.com/en-us/evalcenter/"
                    "download-windows-server-%s" % source.rsplit(" ", 1)[-1])
        if source == "Windows Server 2012 R2":
            return ("https://www.microsoft.com/en-us/evalcenter/"
                    "download-windows-server-2012-r2")
        if source == "Windows Server 2012":
            return "https://archive.org/search?query=windows+server+2012+iso"
        if source == "Windows Server 2008 R2":
            return "https://archive.org/search?query=windows+server+2008+r2+iso"
        if source == "Windows Server (all versions)":
            return "https://archive.org/search?query=windows+server+iso"
        if source == "Windows (all versions)":
            return "https://archive.org/search?query=microsoft+windows+iso"
        if source == "Linux Kernel (kernel.org)":
            return "https://www.kernel.org/"
        if source == "Ubuntu Server":
            return "https://ubuntu.com/download/server"
        if source == "Debian Live":
            return "https://www.debian.org/CD/live/"
        if source == "Fedora":
            return "https://fedoraproject.org/workstation/download"
        if source == "Pop!_OS":
            return "https://pop.system76.com/"
        if source == "openSUSE Leap":
            return "https://get.opensuse.org/leap/"
        if source == "openSUSE Tumbleweed":
            return "https://get.opensuse.org/tumbleweed/"
        if source == "Mageia":
            return "https://www.mageia.org/en/downloads/"
        if source == "AlmaLinux":
            return "https://almalinux.org/get-almalinux/"
        if source == "MX Linux":
            return "https://mxlinux.org/download-links/"
        if source == "Manjaro":
            return "https://manjaro.org/download/"
        if source == "CentOS Stream":
            return "https://www.centos.org/download/"
        if source == "Slackware":
            return "https://www.slackware.com/getslack/"
        if source == "Tiny Core Linux":
            return "https://tinycorelinux.net/downloads.html"
        if source == "SystemRescue":
            return "https://www.system-rescue.org/Download/"
        if source == "FreeBSD":
            return "https://www.freebsd.org/where/"
        if source == "Zorin OS":
            return "https://zorinos.com/download/"
        if source == "elementary OS":
            return "https://github.com/elementary/os/releases/"
        if source == "NixOS":
            return "https://nixos.org/download.html"
        if source == "Puppy Linux":
            return "https://puppylinux-woof-ce.github.io/"
        if source == "Tails":
            return "https://tails.net/install/"
        if source == "EndeavourOS":
            return "https://endeavouros.com/"
        if source == "Garuda Linux":
            return "https://garudalinux.org/downloads"
        if source == "KDE neon":
            return "https://neon.kde.org/download"
        if source == "Rescuezilla":
            return "https://rescuezilla.com/download"
        if source == "GParted Live":
            return "https://gparted.org/livecd.php"
        if source == "Clonezilla":
            return "https://clonezilla.org/downloads.php"
        if source == "Deepin":
            return "https://www.deepin.org/en/download/"
        if source == "FreeBSD":
            return "https://www.freebsd.org/where/"
        if source == "OpenBSD":
            return "https://www.openbsd.org/faq/faq4.html#Download"
        if source == "NetBSD":
            return "https://www.netbsd.org/releases/"
        if is_ia_source(source):
            return "https://archive.org/search?query=" + urllib.parse.quote_plus(source)
        return None

    def _open_source_page_clicked(self):
        source = self._source_var.get()
        url = self._source_page_url(source)
        if not url:
            messagebox.showinfo("No page", "This source does not have a single official download page in the app.")
            return
        try:
            ipc_path = os.path.join(tempfile.gettempdir(), f"ipm_webview_{int(time.time() * 1000)}.txt")
            try:
                with open(ipc_path, "w", encoding="utf-8") as f:
                    f.write("")
            except Exception:
                ipc_path = ""

            opened_in_app = open_url_in_app(url, title=source, ipc_path=ipc_path or None)
            if not opened_in_app:
                open_url_default(url)
                messagebox.showinfo("Tip", "For in-app browsing, install pywebview (pip install pywebview).")
                return

            if ipc_path:
                def watcher():
                    deadline = time.time() + 60 * 60
                    while time.time() < deadline:
                        try:
                            with open(ipc_path, "r", encoding="utf-8") as f:
                                s = (f.read() or "").strip()
                            if s and is_plausible_iso_download_url(s):
                                try:
                                    os.remove(ipc_path)
                                except Exception:
                                    pass
                                self.after(0, lambda u=s: self._download_detected_url(u))
                                return
                        except Exception:
                            pass
                        time.sleep(0.5)

                threading.Thread(target=watcher, daemon=True).start()

            open_external = messagebox.askyesno(
                "Downloads",
                "The in-app browser may not support file downloads on some sites.\n\nOpen this page in your default browser to download?",
            )
            if open_external:
                open_url_default(url)
        except Exception as e:
            messagebox.showerror("Error", f"Could not open browser: {e}")

    def _download_detected_url(self, url: str):
        parsed = urllib.parse.urlparse(url)
        name = os.path.basename(parsed.path) or "download.iso"
        if not name.lower().endswith(".iso"):
            name += ".iso"

        target = filedialog.asksaveasfilename(
            title="Save ISO as",
            defaultextension=".iso",
            initialfile=name,
            filetypes=[("ISO files", "*.iso"), ("All files", "*.*")],
        )
        if not target:
            return

        self._stop_download.clear()
        self._set_net_progress(0.0)
        self._set_net_status("Queued")
        self._set_remote_actions_enabled(False)
        self._enqueue_download(url=url, target=Path(target), expected_sha256=None, label=name)

    def _set_net_status(self, text: str):
        self._net_status_var.set(text)

    def _set_net_progress(self, frac: float | None):
        if frac is None:
            self._net_progress.configure(mode="indeterminate")
            self._net_progress.start(10)
            return
        self._net_progress.stop()
        self._net_progress.configure(mode="determinate")
        self._net_progress["value"] = max(0.0, min(100.0, frac * 100.0))

    def _set_remote_actions_enabled(self, enabled: bool):
        state = "normal" if enabled else "disabled"
        for b in (self._btn_download, self._btn_copy_url):
            b.configure(state=state)
        # Validate does not require a selection; it depends on having a list.
        if getattr(self, "_remote_items", None):
            self._btn_validate.configure(state="normal")
        else:
            self._btn_validate.configure(state="disabled")
        # Audit is always available.
        if getattr(self, "_audit_thread", None) is not None and self._audit_thread.is_alive():
            self._btn_audit.configure(state="disabled")
        else:
            self._btn_audit.configure(state="normal")
        # Download URL is always available unless a download is running.
        if self._download_active.is_set():
            self._btn_download_url.configure(state="disabled")
            self._btn_refresh_list.configure(state="disabled")
            self._btn_validate.configure(state="disabled")
            self._btn_audit.configure(state="disabled")
        else:
            self._btn_download_url.configure(state="normal")
            self._btn_refresh_list.configure(state="normal")
            if getattr(self, "_remote_items", None):
                self._btn_validate.configure(state="normal")
            else:
                self._btn_validate.configure(state="disabled")
            if getattr(self, "_audit_thread", None) is not None and self._audit_thread.is_alive():
                self._btn_audit.configure(state="disabled")
            else:
                self._btn_audit.configure(state="normal")

    def _download_from_url_clicked(self):
        url = simpledialog.askstring("Download URL", "Paste a direct ISO URL:")
        if not url:
            return
        url = url.strip()
        if not (url.lower().startswith("http://") or url.lower().startswith("https://")):
            messagebox.showerror("Invalid URL", "URL must start with http:// or https://")
            return

        sha = simpledialog.askstring("SHA-256 (optional)", "Paste expected SHA-256 (optional):")
        sha = (sha or "").strip().lower() or None
        if sha is not None and not re.fullmatch(r"[a-f0-9]{64}", sha):
            messagebox.showerror("Invalid SHA-256", "SHA-256 must be 64 hex characters.")
            return

        parsed = urllib.parse.urlparse(url)
        name = os.path.basename(parsed.path) or "download.iso"
        if not name.lower().endswith(".iso"):
            name += ".iso"

        target = filedialog.asksaveasfilename(
            title="Save ISO as",
            defaultextension=".iso",
            initialfile=name,
            filetypes=[("ISO files", "*.iso"), ("All files", "*.*")],
        )
        if not target:
            return

        self._stop_download.clear()
        self._set_net_progress(0.0)
        self._set_net_status("Queued")
        self._set_remote_actions_enabled(False)
        self._enqueue_download(url=url, target=Path(target), expected_sha256=sha, label=os.path.basename(Path(target).name))

    def _on_remote_selection_changed(self):
        self._set_remote_actions_enabled(bool(self._selected_remotes()))

    def _selected_remotes(self) -> list[RemoteIsoItem]:
        sel = list(self._remote_tree.selection())
        if not sel:
            return []
        by_url = {it.url: it for it in self._remote_items}
        out: list[RemoteIsoItem] = []
        for iid in sel:
            it = by_url.get(iid)
            if it is not None:
                out.append(it)
        # Keep the display order if possible
        out.sort(key=lambda x: (x.name.lower(), x.url))
        return out

    def _selected_remote(self) -> RemoteIsoItem | None:
        items = self._selected_remotes()
        return items[0] if items else None

    def _copy_url_clicked(self):
        items = self._selected_remotes()
        if not items:
            return
        self.clipboard_clear()
        self.clipboard_append("\n".join([it.url for it in items]))
        self._set_net_status("Copied URL(s) to clipboard")

    def _apply_remote_filter(self):
        term = (self._net_query_var.get() or "").strip().lower()
        self._remote_tree.delete(*self._remote_tree.get_children())

        visible = []
        for item in self._remote_items:
            if not term or term in item.name.lower() or term in item.url.lower():
                visible.append(item)

        # Treeview item IDs must be unique. We use the URL as iid, so ensure we only
        # show each URL once even if a source/search returns duplicates.
        deduped: list[RemoteIsoItem] = []
        seen_urls: set[str] = set()
        for it in visible:
            if it.url in seen_urls:
                continue
            seen_urls.add(it.url)
            deduped.append(it)
        visible = deduped

        def _version_key(it: RemoteIsoItem) -> tuple[int, tuple[int, int, int, int], str, str]:
            s = f"{it.name} {it.url}"
            v = infer_version_from_iso_name(s) or ""
            if not v:
                return (0, (0, 0, 0, 0), it.name.lower(), it.url)
            parts = [0, 0, 0, 0]
            nums = [p for p in re.split(r"[^0-9]+", v) if p]
            for i, p in enumerate(nums[:4]):
                try:
                    parts[i] = int(p)
                except Exception:
                    parts[i] = 0
            return (1, (parts[0], parts[1], parts[2], parts[3]), it.name.lower(), it.url)

        visible.sort(key=_version_key, reverse=True)

        shown = visible
        if getattr(self, "_remote_display_limit", None) is not None and self._remote_display_limit > 0:
            shown = visible[: self._remote_display_limit]

        for it in shown:
            self._remote_tree.insert(
                "",
                tk.END,
                iid=it.url,
                values=(it.name, it.url, it.sha256[:12] + "..." if it.sha256 else ""),
            )
        self._stripe_rows(self._remote_tree)
        self._set_net_status(
            f"Showing {len(shown)} of {len(visible)} ISO(s) (filtered from {len(self._remote_items)})"
        )
        self._set_remote_actions_enabled(False)
        if getattr(self, "_btn_load_more_remote", None) is not None:
            can_show_more = len(shown) < len(visible)
            can_fetch_more = bool(getattr(self, "_last_web_search", None))
            # If this list came from Web Search, allow Load more to fetch older versions even
            # when all currently-fetched items are already shown.
            self._btn_load_more_remote.configure(state="normal" if (can_show_more or can_fetch_more) else "disabled")

    def _load_more_remote_clicked(self):
        if not getattr(self, "_remote_items", None):
            return
        # First: if more items are already fetched but hidden by the page size, show more rows.
        step = getattr(self, "_remote_display_limit_step", 200)
        cur = getattr(self, "_remote_display_limit", step)
        if cur > 0 and len(self._remote_items) > cur:
            self._remote_display_limit = cur + step
            self._apply_remote_filter()
            return

        # If this is a Web Search result, Load more means: fetch older versions (deeper archives).
        last = getattr(self, "_last_web_search", None)
        if isinstance(last, dict) and last.get("query"):
            if self._download_active.is_set():
                return
            if getattr(self, "_web_more_thread", None) is not None and self._web_more_thread.is_alive():
                return

            self._web_archive_level = int(getattr(self, "_web_archive_level", 0)) + 1
            self._set_net_status("Loading more (older versions)...")
            self._set_net_progress(None)
            self._set_remote_actions_enabled(False)

            def worker_more():
                try:
                    if last.get("archive_only"):
                        items = archive_search_all(last.get("query", ""), self._web_archive_level)
                        self._work_q.put(("remote_items_more", items, None))
                        return
                    items = web_search_iso_urls(
                        last.get("query", ""),
                        last.get("provider", "DuckDuckGo"),
                        searxng_url=last.get("searxng_url", ""),
                        google_key=last.get("google_key", ""),
                        google_cx=last.get("google_cx", ""),
                        archive_level=self._web_archive_level,
                    )
                    self._work_q.put(("remote_items_more", items, None))
                except Exception as e:
                    self._work_q.put(("error_net", f"Load more failed: {e}", None))

            self._web_more_thread = threading.Thread(target=worker_more, daemon=True)
            self._web_more_thread.start()
            return

        # Otherwise, Load more just shows more already-fetched items.
        self._remote_display_limit = cur + step
        self._apply_remote_filter()

    def _refresh_remote_clicked(self):
        if self._download_active.is_set():
            return

        source = self._source_var.get()

        page_only_on_refresh = {
            "Windows 11 (Microsoft)",
            "Windows 10 (Microsoft)",
            "Linux Kernel (kernel.org)",
            "Pop!_OS",
            "Garuda Linux",
            "KDE neon",
            "NixOS",
            "Puppy Linux",
            "Tails",
            "Rescuezilla",
            "GParted Live",
            "Clonezilla",
            "Zorin OS",
            "Deepin",
        }

        url = self._source_page_url(source)
        if url and source in page_only_on_refresh:
            self._remote_items = []
            self._apply_remote_filter()
            self._set_net_progress(0.0)
            self._set_net_status("Opening official source page...")
            try:
                opened_in_app = open_url_in_app(url, title=source)
                if not opened_in_app:
                    open_url_default(url)
                    messagebox.showinfo("Tip", "For in-app browsing, install pywebview (pip install pywebview).")
                else:
                    open_external = messagebox.askyesno(
                        "Downloads",
                        "The in-app browser may not support file downloads on some sites.\n\nOpen this page in your default browser to download?",
                    )
                    if open_external:
                        open_url_default(url)
            except Exception as e:
                messagebox.showerror("Error", f"Could not open browser: {e}")

            self._set_net_progress(0.0)
            self._set_net_status("Ready")
            return

        self._set_net_status("Refreshing...")
        self._set_net_progress(None)
        self._set_remote_actions_enabled(False)
        # Let "Load more" fetch older releases of this source from its archive.
        self._web_archive_level = 0
        self._last_web_search = {"query": source, "archive_only": True} if has_archive_support(source) else None

        def worker():
            try:
                custom_base = (self._custom_base_var.get() or "").strip()
                custom_sum = (self._custom_sum_var.get() or "").strip()
                items = self._fetch_remote_items(source, custom_base=custom_base, custom_sum=custom_sum)
                self._work_q.put(("remote_items", (source, items), None))
            except Exception as e:
                self._work_q.put(("error_net", f"{source}: Refresh failed: {e}", None))

        threading.Thread(target=worker, daemon=True).start()

    def _web_search_clicked(self):
        if self._download_active.is_set():
            messagebox.showinfo("Busy", "A download is in progress.")
            return

        q = (getattr(self, "_web_query_var", None).get() if getattr(self, "_web_query_var", None) is not None else "").strip()
        if not q:
            messagebox.showinfo("Web Search", "Type something to search (e.g. 'ubuntu iso').")
            return

        try:
            self._add_recent_web_search(q)
            self._save_settings_file()
        except Exception:
            pass

        mode = (getattr(self, "_web_mode_var", None).get() if getattr(self, "_web_mode_var", None) is not None else "Any Website").strip()
        provider = (getattr(self, "_web_provider_var", None).get() if getattr(self, "_web_provider_var", None) is not None else "DuckDuckGo").strip()
        det_first = bool((getattr(self, "_deterministic_first_var", None).get() if getattr(self, "_deterministic_first_var", None) is not None else "").strip())
        try:
            base_level = int((getattr(self, "_web_archive_level_var", None).get() if getattr(self, "_web_archive_level_var", None) is not None else "0").strip())
        except Exception:
            base_level = 0

        # Remember web-search context so "Load more" can fetch older versions.
        base_url = (getattr(self, "_searx_url_var", None).get() if getattr(self, "_searx_url_var", None) is not None else "").strip()
        key = (getattr(self, "_google_key_var", None).get() if getattr(self, "_google_key_var", None) is not None else "").strip()
        cx = (getattr(self, "_google_cx_var", None).get() if getattr(self, "_google_cx_var", None) is not None else "").strip()
        self._last_web_search = {
            "query": q,
            "provider": provider,
            "searxng_url": base_url,
            "google_key": key,
            "google_cx": cx,
        }
        self._web_archive_level = base_level

        self._set_net_status("Searching web...")
        self._set_net_progress(None)
        self._set_remote_actions_enabled(False)

        def worker(raw_query: str, shown_query: str):
            try:
                # Use shared helper: handles provider selection + archive fallbacks.
                items = web_search_iso_urls(
                    raw_query,
                    provider,
                    searxng_url=base_url,
                    google_key=key,
                    google_cx=cx,
                    archive_level=base_level,
                    deterministic_first=det_first,
                )
                if not items:
                    # Web engines (especially DuckDuckGo) often block scraping and return nothing.
                    # If the query names a distro we already support, use the built-in source instead.
                    src_name = self._match_builtin_source(raw_query)
                    if src_name:
                        try:
                            items = self._fetch_remote_items(src_name)
                        except Exception:
                            items = []
                        if items:
                            shown_query = f"{shown_query} -> {src_name} (built-in source)"
                if not items:
                    hint = (
                        "No direct .iso links found.\n\n"
                        "Tip: DuckDuckGo often doesn't expose direct download links. If you have a SearxNG instance, set Provider to SearxNG.\n\n"
                        "Try more specific queries like:\n"
                        "- ubuntu 16.04 iso archive\n"
                        "- debian 10 iso\n"
                        "- archlinux 2020.01.01 iso\n"
                        "- systemrescue iso\n\n"
                        "Tip: Some pages hide the download links behind scripts, so scraping may fail.\n\n"
                        "Tip: You can filter the found list using the Search box above the table."
                    )
                    self._work_q.put(("error_net", f"Web Search: {shown_query}: 0 ISO(s) found\n\n{hint}", None))
                    return
                self._work_q.put(("remote_items", (f"Web Search: {shown_query}", items), None))
            except Exception as e:
                self._work_q.put(("error_net", f"Web search failed: {e}", None))

        shown = q if mode == "Any Website" else f"{q} (iso-focused)"
        if provider == "SearxNG":
            shown = shown + " [SearxNG]"
        elif provider == "Google API":
            shown = shown + " [Google API]"
        # web_search_iso_urls already applies an ISO-focused query internally; for "Any Website"
        # we still pass the raw query and let the helper decide.
        threading.Thread(target=worker, args=(q, shown), daemon=True).start()

    def _match_builtin_source(self, query: str) -> str | None:
        """Map a free-text web query (e.g. 'ubuntu', 'mx linux iso') to a built-in source name."""

        def norm(s: str) -> str:
            return re.sub(r"[^a-z0-9]+", " ", (s or "").lower()).strip()

        q = f" {norm(query)} "
        q_compact = q.replace(" ", "")
        try:
            sources = self._sources_for_category("All")
        except Exception:
            return None

        best: str | None = None
        best_len = 0
        for s in sources:
            if s in ("Custom Source",):
                continue
            inner = re.findall(r"\((.*?)\)", s)
            base = norm(re.sub(r"\(.*?\)", "", s))
            cands = {base, base.replace(" ", "")}
            cands.update(norm(x) for x in inner if "." not in x)  # "(Kubuntu)" yes, "(kernel.org)" no
            for suffix in (" linux", " os", " live"):
                if base.endswith(suffix):
                    cands.add(base[: -len(suffix)])
            if base.startswith("linux "):
                cands.add(base[len("linux "):])
            for c in cands:
                if not c:
                    continue
                hit = f" {c} " in q or (len(c) >= 5 and c.replace(" ", "") in q_compact)
                if hit and len(c) > best_len:
                    best, best_len = s, len(c)
        return best

    def _get_provider_profiles(self) -> dict[str, dict[str, str]]:
        try:
            v = self._settings.get("provider_profiles")
            if isinstance(v, dict):
                out: dict[str, dict[str, str]] = {}
                for k, vv in v.items():
                    if not isinstance(vv, dict):
                        continue
                    out[str(k)] = {str(kk): str(vvv) for kk, vvv in vv.items()}
                return out
        except Exception:
            pass
        return {}

    def _save_provider_profile(self) -> None:
        provider = (self._web_provider_var.get() or "DuckDuckGo").strip()
        profiles = self._get_provider_profiles()
        profiles[provider] = {
            "searxng_url": (self._searx_url_var.get() or "").strip(),
            "google_key": (self._google_key_var.get() or "").strip(),
            "google_cx": (self._google_cx_var.get() or "").strip(),
            "deterministic_first": (self._deterministic_first_var.get() or "").strip(),
            "archive_level": (self._web_archive_level_var.get() or "0").strip(),
        }
        self._settings["provider_profiles"] = profiles
        self._save_settings_file()
        self._set_net_status(f"Saved profile: {provider}")

    def _apply_provider_profile(self) -> None:
        provider = (self._web_provider_var.get() or "DuckDuckGo").strip()
        profiles = self._get_provider_profiles()
        prof = profiles.get(provider)
        if not prof:
            messagebox.showinfo("Profile", f"No saved profile for {provider}.")
            return
        try:
            self._searx_url_var.set((prof.get("searxng_url") or "").strip())
            self._google_key_var.set((prof.get("google_key") or "").strip())
            self._google_cx_var.set((prof.get("google_cx") or "").strip())
            self._deterministic_first_var.set((prof.get("deterministic_first") or "").strip())
            self._web_archive_level_var.set((prof.get("archive_level") or "0").strip())
        except Exception:
            pass
        self._set_net_status(f"Applied profile: {provider}")

    def _get_recent_web_searches(self) -> list[str]:
        try:
            v = self._settings.get("recent_web_searches")
            if isinstance(v, list):
                out: list[str] = []
                for s in v:
                    ss = str(s).strip()
                    if ss:
                        out.append(ss)
                return out
        except Exception:
            pass
        return []

    def _get_favorite_web_searches(self) -> list[str]:
        try:
            v = self._settings.get("favorite_web_searches")
            if isinstance(v, list):
                out: list[str] = []
                for s in v:
                    ss = str(s).strip()
                    if ss:
                        out.append(ss)
                return out
        except Exception:
            pass
        return []

    def _add_recent_web_search(self, q: str) -> None:
        s = (q or "").strip()
        if not s:
            return
        cur = self._get_recent_web_searches()
        cur = [x for x in cur if x.lower() != s.lower()]
        cur.insert(0, s)
        self._settings["recent_web_searches"] = cur[:25]

    def _toggle_favorite_web_search(self, q: str) -> None:
        s = (q or "").strip()
        if not s:
            return
        fav = self._get_favorite_web_searches()
        existing = None
        for x in fav:
            if x.lower() == s.lower():
                existing = x
                break
        if existing is not None:
            fav = [x for x in fav if x.lower() != s.lower()]
        else:
            fav.insert(0, s)
        self._settings["favorite_web_searches"] = fav[:50]

    def _clear_recent_web_searches(self) -> None:
        try:
            self._settings["recent_web_searches"] = []
        except Exception:
            pass

    def _show_web_search_menu(self, event=None) -> None:
        try:
            menu = tk.Menu(self, tearoff=0)
            q = (self._web_query_var.get() or "").strip()

            def run_query(s: str) -> None:
                try:
                    self._web_query_var.set(s)
                except Exception:
                    return
                self._web_search_clicked()

            fav = self._get_favorite_web_searches()
            recent = self._get_recent_web_searches()

            if q:
                is_fav = any(x.lower() == q.lower() for x in fav)
                menu.add_command(
                    label=("Remove from favorites" if is_fav else "Add to favorites"),
                    command=lambda s=q: (self._toggle_favorite_web_search(s), self._save_settings_file()),
                )
                menu.add_separator()

            if fav:
                for s in fav[:15]:
                    menu.add_command(label=f"★ {s}", command=lambda ss=s: run_query(ss))
                menu.add_separator()

            if recent:
                for s in recent[:15]:
                    menu.add_command(label=s, command=lambda ss=s: run_query(ss))
                menu.add_separator()

            menu.add_command(label="Clear history", command=lambda: (self._clear_recent_web_searches(), self._save_settings_file()))

            try:
                menu.tk_popup(int(event.x_root), int(event.y_root))
            finally:
                menu.grab_release()
        except Exception:
            pass

    def _url_check(self, url: str) -> tuple[bool, str]:
        # Best-effort check without downloading the file.
        # 1) HEAD
        # 2) fallback to Range GET (first byte)
        ua = {
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/120.0 Safari/537.36"
            ),
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "Accept-Language": "en-US,en;q=0.9",
        }
        try:
            req = urllib.request.Request(url, headers=ua, method="HEAD")
            with urllib.request.urlopen(req, timeout=20.0, context=ssl_context_for_https()) as resp:
                code = getattr(resp, "status", 200)
                if 200 <= int(code) < 400:
                    return True, f"HTTP {code}"
                return False, f"HTTP {code}"
        except Exception:
            pass

        try:
            req = urllib.request.Request(url, headers={**ua, "Range": "bytes=0-0"})
            with urllib.request.urlopen(req, timeout=20.0, context=ssl_context_for_https()) as resp:
                code = getattr(resp, "status", 200)
                if 200 <= int(code) < 400:
                    return True, f"HTTP {code}"
                return False, f"HTTP {code}"
        except Exception as e:
            return False, str(e)

    def _validate_remote_clicked(self):
        if self._download_active.is_set():
            return
        if getattr(self, "_validate_thread", None) is not None and self._validate_thread.is_alive():
            return
        if not self._remote_items:
            messagebox.showinfo("Nothing to validate", "Refresh a source first.")
            return

        self._stop_validate.clear()
        self._set_net_status("Validating links...")
        self._set_net_progress(0.0)
        self._btn_validate.configure(state="disabled")

        def worker(items: list[RemoteIsoItem]):
            try:
                bad: list[tuple[str, str]] = []
                total = max(1, len(items))
                for i, it in enumerate(items):
                    if self._stop_validate.is_set():
                        raise RuntimeError("Validation cancelled")
                    ok, info = self._url_check(it.url)
                    if not ok:
                        bad.append((it.url, info))
                    self._work_q.put(("validate_progress", (i + 1) / total, None))
                self._work_q.put(("validate_done", bad, len(items)))
            except Exception as e:
                self._work_q.put(("error_net", f"Validate failed: {e}", None))

        self._validate_thread = threading.Thread(target=worker, args=(list(self._remote_items),), daemon=True)
        self._validate_thread.start()

    def _audit_sources_clicked(self):
        if self._download_active.is_set():
            return
        if getattr(self, "_audit_thread", None) is not None and self._audit_thread.is_alive():
            return

        self._stop_audit.clear()
        self._set_net_status("Auditing all sources...")
        self._set_net_progress(0.0)
        self._set_remote_actions_enabled(self._selected_remote() is not None)

        sources = [s for s in self._sources_for_category("All") if s != "Custom Source"]

        def worker(all_sources: list[str]):
            try:
                results: list[tuple[str, bool, str]] = []
                total = max(1, len(all_sources))
                for i, src in enumerate(all_sources):
                    if self._stop_audit.is_set():
                        raise RuntimeError("Audit cancelled")

                    # If source has an official page URL and is page-only, check the page URL.
                    page = self._source_page_url(src)
                    if page is not None and src in (
                        "Windows 11 (Microsoft)",
                        "Windows 10 (Microsoft)",
                        "Linux Kernel (kernel.org)",
                        "Garuda Linux",
                        "KDE neon",
                        "NixOS",
                        "Puppy Linux",
                        "Tails",
                        "Rescuezilla",
                        "GParted Live",
                        "Clonezilla",
                        "Zorin OS",
                    ):
                        ok, info = self._url_check(page)
                        results.append((src, ok, info))
                    else:
                        try:
                            items = self._fetch_remote_items(src)
                            results.append((src, bool(items), f"{len(items)} item(s)" if items else "No items"))
                        except Exception as e:
                            # Some sources fail due to network policy (403), DNS issues, or local SSL trust store.
                            # In that case, fall back to checking the official source page, if we have one.
                            msg = str(e)
                            page2 = self._source_page_url(src)
                            netish = (
                                "HTTP Error 403" in msg
                                or "HTTP Error 404" in msg
                                or "CERTIFICATE_VERIFY_FAILED" in msg
                                or "SSLCertVerificationError" in msg
                                or "getaddrinfo failed" in msg
                                or "timed out" in msg.lower()
                                or "Use 'Open Source Page'" in msg
                                or "Use \"Open Source Page\"" in msg
                                or "No items" in msg
                                or "No " in msg and " found" in msg
                            )
                            if page2 and netish:
                                ok, info = self._url_check(page2)
                                if ok:
                                    results.append((src, True, f"Source page reachable; fetch failed: {msg}"))
                                else:
                                    results.append((src, False, msg))
                            else:
                                results.append((src, False, msg))

                    self._work_q.put(("audit_progress", (i + 1) / total, None))

                self._work_q.put(("audit_done", results, None))
            except Exception as e:
                self._work_q.put(("error_net", f"Audit failed: {e}", None))

        self._audit_thread = threading.Thread(target=worker, args=(sources,), daemon=True)
        self._audit_thread.start()

    def _fetch_remote_items(self, source: str, custom_base: str = "", custom_sum: str = "") -> list[RemoteIsoItem]:
        if source == "Ubuntu":
            base = "https://releases.ubuntu.com/"
            index = http_get_text(base)
            links = parse_apache_listing_for_links(index)
            versions = []
            for href in links:
                m = re.match(r"^(\d+\.\d+(?:\.\d+)?)/$", href)
                if m:
                    versions.append(m.group(1))
            if not versions:
                raise RuntimeError("No Ubuntu versions found")
            versions.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
            ver = versions[0]
            page_url = join_url(base, ver + "/")
            page = http_get_text(page_url)
            page_links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in page_links if l.lower().endswith(".iso")})

            sums: dict[str, str] = {}
            if "SHA256SUMS" in page_links:
                sums_text = http_get_text(join_url(page_url, "SHA256SUMS"))
                sums = parse_checksum_lines(sums_text)

            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(page_url, name), sha256=sums.get(name)))
            return items

        if source == "Ubuntu Server":
            base = "https://releases.ubuntu.com/"
            index = http_get_text(base)
            links = parse_apache_listing_for_links(index)
            versions: list[str] = []
            for href in links:
                m = re.match(r"^(\d+\.\d+(?:\.\d+)?)/$", href)
                if m:
                    versions.append(m.group(1))
            if not versions:
                raise RuntimeError("No Ubuntu versions found")
            versions.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)

            last_err: Exception | None = None
            for ver in versions[:5]:
                try:
                    page_url = join_url(base, ver + "/")
                    page = http_get_text(page_url)
                    page_links = parse_apache_listing_for_links(page)
                    iso_names = sorted({l for l in page_links if l.lower().endswith(".iso") and "server" in l.lower()})
                    if not iso_names:
                        continue

                    sums: dict[str, str] = {}
                    if "SHA256SUMS" in page_links:
                        try:
                            sums_text = http_get_text(join_url(page_url, "SHA256SUMS"))
                            sums = parse_checksum_lines(sums_text)
                        except Exception:
                            pass

                    return [RemoteIsoItem(name=n, url=join_url(page_url, n), sha256=sums.get(n)) for n in iso_names]
                except Exception as e:
                    last_err = e
                    continue

            if last_err is not None:
                raise RuntimeError(f"Ubuntu Server listing failed: {last_err}")
            raise RuntimeError("Ubuntu Server listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source in (
            "Ubuntu (Kubuntu)",
            "Ubuntu (Xubuntu)",
            "Ubuntu (Lubuntu)",
            "Ubuntu (Ubuntu MATE)",
            "Ubuntu (Ubuntu Budgie)",
            "Ubuntu (Ubuntu Studio)",
            "Ubuntu (Ubuntu Kylin)",
        ):
            flavor = {
                "Ubuntu (Kubuntu)": "kubuntu",
                "Ubuntu (Xubuntu)": "xubuntu",
                "Ubuntu (Lubuntu)": "lubuntu",
                "Ubuntu (Ubuntu MATE)": "ubuntu-mate",
                "Ubuntu (Ubuntu Budgie)": "ubuntu-budgie",
                "Ubuntu (Ubuntu Studio)": "ubuntustudio",
                "Ubuntu (Ubuntu Kylin)": "ubuntukylin",
            }[source]
            base = f"https://cdimage.ubuntu.com/{flavor}/releases/"
            index = http_get_text(base)
            links = parse_apache_listing_for_links(index)
            versions = []
            for href in links:
                m = re.match(r"^(\d+\.\d+(?:\.\d+)?)/$", href)
                if m:
                    versions.append(m.group(1))
            if not versions:
                versions = sorted(set(re.findall(r"href=[\"'](\d+\.\d+(?:\.\d+)?)/[\"']", index)))
            if not versions:
                # Some ubuntu flavors provide a 'current/' folder
                if "current/" in links or "current/" in index:
                    versions = ["current"]
            if not versions:
                raise RuntimeError(f"{flavor} listing failed. Use 'Open Source Page' or 'Custom Source'.")
            if versions == ["current"]:
                ver = "current"
            else:
                versions.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
                ver = versions[0]

            ver_candidates: list[str] = []
            if ver == "current":
                ver_candidates = ["current"]
            else:
                # Try a few recent versions; some flavors lag behind the main Ubuntu release.
                ver_candidates = versions[:3]

            # Try common subfolders for release artifacts
            for v in ver_candidates:
                candidates: list[str] = []
                if v == "current":
                    candidates.extend(["current/", "current/release/", "current/live/"])
                else:
                    candidates.extend([
                        f"{v}/release/",
                        f"{v}/",
                        f"{v}/current/",
                        f"{v}/live/",
                    ])

                for rel in candidates:
                    page_url = join_url(base, rel)
                    try:
                        page = http_get_text(page_url)
                    except Exception:
                        continue
                    page_links = parse_apache_listing_for_links(page)
                    iso_names = sorted({l for l in page_links if l.lower().endswith(".iso")})
                    if not iso_names:
                        iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
                        if not iso_names:
                            continue

                    sums: dict[str, str] = {}
                    if "SHA256SUMS" in page_links:
                        try:
                            sums = parse_checksum_lines(http_get_text(join_url(page_url, "SHA256SUMS")))
                        except Exception:
                            pass

                    items = []
                    for name in iso_names:
                        items.append(RemoteIsoItem(name=name, url=join_url(page_url, name), sha256=sums.get(name)))
                    if items:
                        return items
            raise RuntimeError(f"{flavor} listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "FreeBSD":
            # FreeBSD ISO images are published under ISO-IMAGES/<version>/amd64/amd64/
            root = "https://download.freebsd.org/ftp/releases/ISO-IMAGES/"
            idx = http_get_text(root)
            links = parse_apache_listing_for_links(idx)
            vers: list[str] = []
            for href in links:
                m = re.match(r"^(\d+\.\d+)/$", href)
                if m:
                    vers.append(m.group(1))
            if not vers:
                vers = sorted(set(re.findall(r"href=[\"'](\d+\.\d+)/[\"']", idx)))
            if not vers:
                raise RuntimeError("No FreeBSD ISO-IMAGES versions found")
            vers.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
            ver = vers[0]

            base = join_url(root, f"{ver}/amd64/amd64/")
            page = http_get_text(base)
            rel_links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in rel_links if l.lower().endswith(".iso")})
            if not iso_names:
                iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))

            sums: dict[str, str] = {}
            for candidate in ("CHECKSUM.SHA256", "CHECKSUM.SHA256.asc", "SHA256"):
                if candidate in rel_links:
                    try:
                        txt = http_get_text(join_url(base, candidate))
                        sums = parse_checksum_lines(txt)
                        if sums:
                            break
                    except Exception:
                        pass

            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            if not items:
                raise RuntimeError("No FreeBSD ISO images found for latest release")
            return items

        if source == "NetBSD":
            # NetBSD release layout can vary; best-effort attempt for current release ISO images.
            base = "https://cdn.netbsd.org/pub/NetBSD/NetBSD-"
            # Instead of scraping versions broadly, use releases page as a source page and attempt a common URL.
            # If it fails, user can click Open Source Page.
            try:
                releases_page = http_get_text("https://cdn.netbsd.org/pub/NetBSD/")
                links = parse_apache_listing_for_links(releases_page)
                vers = []
                for href in links:
                    m = re.match(r"^NetBSD-(\d+\.\d+(?:\.\d+)?)/$", href)
                    if m:
                        vers.append(m.group(1))
                if not vers:
                    raise RuntimeError("No NetBSD versions found")
                vers.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
                ver = vers[0]
                rel_base = f"https://cdn.netbsd.org/pub/NetBSD/NetBSD-{ver}/images/"
                page = http_get_text(rel_base)
                rel_links = parse_apache_listing_for_links(page)
                iso_names = sorted({l for l in rel_links if l.lower().endswith(".iso")})

                sums: dict[str, str] = {}
                for candidate in ("SHA256", "SHA256SUMS", "sha256"):
                    if candidate in rel_links:
                        try:
                            sums = parse_checksum_lines(http_get_text(join_url(rel_base, candidate)))
                            break
                        except Exception:
                            pass

                items = []
                for name in iso_names:
                    items.append(RemoteIsoItem(name=name, url=join_url(rel_base, name), sha256=sums.get(name)))
                if items:
                    return items
            except Exception:
                pass

            raise RuntimeError("NetBSD listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Linux Mint":
            candidates = [
                "https://mirrors.kernel.org/linuxmint/stable/",
                "https://mirror.csclub.uwaterloo.ca/linuxmint/stable/",
                "https://ftp.heanet.ie/mirrors/linuxmint.com/stable/",
            ]

            last_err: Exception | None = None
            for base in candidates:
                try:
                    page = http_get_text(base)
                except Exception as e:
                    last_err = e
                    continue

                # Prefer parsed links, but also fall back to raw HTML scan
                links = parse_apache_listing_for_links(page)
                versions: list[str] = []
                for href in links:
                    m = re.match(r"^(\d+(?:\.\d+)?)/$", href)
                    if m:
                        versions.append(m.group(1))
                if not versions:
                    versions = sorted(set(re.findall(r'href="(\d+(?:\.\d+)?)/"', page)))

                if not versions:
                    continue

                versions.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
                ver = versions[0]

                # Linux Mint ISOs live under version/ (sometimes iso/)
                for sub in ("", "iso/", "images/"):
                    ver_base = join_url(base, f"{ver}/{sub}")
                    try:
                        ver_page = http_get_text(ver_base)
                    except Exception as e:
                        last_err = e
                        continue
                    ver_links = parse_apache_listing_for_links(ver_page)
                    iso_names = sorted({l for l in ver_links if l.lower().endswith(".iso")})
                    if not iso_names:
                        iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", ver_page)))
                        if not iso_names:
                            continue

                    sums: dict[str, str] = {}
                    sha_files = [
                        l
                        for l in ver_links
                        if l.lower().endswith("sha256.txt")
                        or l.lower().endswith("sha256sum.txt")
                        or l.lower().endswith("sha256sums")
                    ]
                    for sha in sha_files:
                        try:
                            sums.update(parse_checksum_lines(http_get_text(join_url(ver_base, sha))))
                        except Exception:
                            pass

                    items = []
                    for name in iso_names:
                        items.append(RemoteIsoItem(name=name, url=join_url(ver_base, name), sha256=sums.get(name)))
                    if items:
                        return items

            # Fallback: release metadata JSON is often more stable than scraping.
            try:
                rel_json_txt = http_get_text("https://fedoraproject.org/releases.json")
                data = json.loads(rel_json_txt)
                rels: list[int] = []
                for it in data.get("releases", []):
                    if it.get("state") == "current" and isinstance(it.get("version"), str):
                        v = it["version"].strip()
                        if v.isdigit():
                            rels.append(int(v))
                if rels:
                    rel = str(max(rels))
                    ws_base = f"https://dl.fedoraproject.org/pub/fedora/linux/releases/{rel}/Workstation/x86_64/iso/"
                    page = http_get_text(ws_base)
                    ws_links = parse_apache_listing_for_links(page)
                    iso_names = sorted({l for l in ws_links if l.lower().endswith(".iso")})
                    if not iso_names:
                        iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
                    if iso_names:
                        items = []
                        for name in iso_names:
                            items.append(RemoteIsoItem(name=name, url=join_url(ws_base, name), sha256=None))
                        if items:
                            return items
            except Exception as e:
                last_err = e

            if last_err is not None:
                raise RuntimeError(f"Linux Mint listing failed: {last_err}")
            raise RuntimeError("Linux Mint listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "elementary OS":
            base = "https://github.com/elementary/os/releases/"
            page = http_get_text(base)
            # Parse HTML for release links; fallback to opening page if parsing fails
            iso_links = re.findall(r'href="(/elementary/os/download/[^"]+)"', page)
            if not iso_links:
                raise RuntimeError("elementary OS downloads are best obtained via their official page. Use 'Open Source Page' or 'Custom Source'.")
            items = []
            for rel in iso_links:
                url = "https://github.com" + rel
                name = rel.split("/")[-1]
                items.append(RemoteIsoItem(name=name, url=url, sha256=None))
            return items

        if source == "Zorin OS":
            raise RuntimeError("Zorin OS downloads are best obtained via their official page. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Deepin":
            raise RuntimeError("Deepin downloads are best obtained via their official page. Use 'Open Source Page'.")

        if source == "MX Linux":
            # mirror.mxrepo.com no longer resolves. ISOs live in Final/<Desktop>/ subfolders.
            candidates = [
                "https://ftp.fau.de/mxlinux-iso/MX/Final/",
                "https://mirror.math.princeton.edu/pub/mxlinux-iso/MX/Final/",
                "https://mirrors.evowise.com/mxlinux-iso/MX/Final/",
                "https://ftp.halifax.rwth-aachen.de/mxlinux-iso/MX/Final/",
            ]
            last_err: Exception | None = None
            for base in candidates:
                try:
                    page = http_get_text(base)
                except Exception as e:
                    last_err = e
                    continue
                links = parse_apache_listing_for_links(page)
                scan: list[str] = [base]
                for href in links:
                    h = (href or "").strip()
                    if h.endswith("/") and not h.startswith(("?", "/", "..", "http")) and h.count("/") == 1:
                        scan.append(join_url(base, h))
                items = []
                seen_urls: set[str] = set()
                for b in scan:
                    try:
                        sub = page if b == base else http_get_text(b)
                    except Exception as e:
                        last_err = e
                        continue
                    sub_links = parse_apache_listing_for_links(sub)
                    sums: dict[str, str] = {}
                    for l in sub_links:
                        if l.lower().endswith((".sha256", "sha256.txt", "sha256sum.txt")):
                            try:
                                txt = http_get_text(join_url(b, l))
                                m = re.match(r"^\s*([a-fA-F0-9]{64})", txt)
                                if m and l.lower().endswith(".sha256"):
                                    sums[l[: -len(".sha256")].rsplit("/", 1)[-1]] = m.group(1).lower()
                                sums.update(parse_checksum_lines(txt))
                            except Exception:
                                pass
                    for l in sub_links:
                        if not l.lower().endswith(".iso"):
                            continue
                        url = join_url(b, l)
                        if url in seen_urls:
                            continue
                        seen_urls.add(url)
                        name = l.rsplit("/", 1)[-1]
                        items.append(RemoteIsoItem(name=name, url=url, sha256=sums.get(name)))
                if items:
                    return items

            # Last resort: SourceForge file browser (download links redirect to a mirror).
            try:
                sf_items = []
                seen_sf: set[str] = set()
                for folder in ("Xfce", "KDE", "Fluxbox"):
                    sf_page = http_get_text(f"https://sourceforge.net/projects/mx-linux/files/Final/{folder}/")
                    for path in re.findall(r'href="(/projects/mx-linux/files/Final/[^"]+?\.iso)/download"', sf_page):
                        if path in seen_sf:
                            continue
                        seen_sf.add(path)
                        sf_items.append(
                            RemoteIsoItem(name=path.rsplit("/", 1)[-1], url=f"https://sourceforge.net{path}/download", sha256=None)
                        )
                if sf_items:
                    return sf_items
            except Exception as e:
                last_err = e

            if last_err is not None:
                raise RuntimeError(f"MX Linux listing failed: {last_err}")
            raise RuntimeError("MX Linux listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Debian Live":
            base = "https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            if not iso_names:
                iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))

            sums: dict[str, str] = {}
            for candidate in ("SHA256SUMS", "SHA256SUMS.sign"):
                if candidate in links:
                    try:
                        sums = parse_checksum_lines(http_get_text(join_url(base, "SHA256SUMS")))
                        break
                    except Exception:
                        pass

            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            if not items:
                raise RuntimeError("No Debian Live ISOs found")
            return items

        if source == "openSUSE Tumbleweed":
            base = "https://download.opensuse.org/tumbleweed/iso/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            if not iso_names:
                iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=None))
            if not items:
                raise RuntimeError("No openSUSE Tumbleweed ISOs found")
            return items

        if source == "openSUSE Leap":
            root = "https://download.opensuse.org/distribution/leap/"
            idx = http_get_text(root)
            links = parse_apache_listing_for_links(idx)
            vers: list[str] = []
            for href in links:
                m = re.match(r"^(\d+\.\d+)/$", href)
                if m:
                    vers.append(m.group(1))
            if not vers:
                vers = sorted(set(re.findall(r"href=[\"'](\d+\.\d+)/[\"']", idx)))
            if not vers:
                raise RuntimeError("No openSUSE Leap versions found")
            vers.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
            ver = vers[0]

            base = join_url(root, f"{ver}/iso/")
            page = http_get_text(base)
            rel_links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in rel_links if l.lower().endswith(".iso")})
            if not iso_names:
                iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=None))
            if not items:
                raise RuntimeError("No openSUSE Leap ISOs found")
            return items

        if source == "Mageia":
            root = "https://mirrors.kernel.org/mageia/iso/"
            idx = http_get_text(root)
            links = parse_apache_listing_for_links(idx)
            vers: list[str] = []
            for href in links:
                m = re.match(r"^(\d+)/$", href)
                if m:
                    vers.append(m.group(1))
            if not vers:
                vers = sorted(set(re.findall(r"href=[\"'](\d+)/[\"']", idx)))
            if not vers:
                raise RuntimeError("No Mageia ISO versions found")
            vers.sort(key=lambda s: int(s), reverse=True)
            ver = vers[0]

            base = join_url(root, f"{ver}/")
            page = http_get_text(base)
            rel_links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in rel_links if l.lower().endswith(".iso")})
            if not iso_names:
                iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=None))
            if not items:
                raise RuntimeError("No Mageia ISOs found")
            return items

        if source == "Parrot OS":
            base = "https://deb.parrot.sh/parrot/iso/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            # Prefer rolling/home editions if present
            preferred_dirs = [d for d in links if d.endswith("/") and d.lower() in ("parrot-home/", "parrot-security/", "rolling/")]
            candidates = preferred_dirs + [d for d in links if d.endswith("/")]
            last_err: Exception | None = None
            for d in candidates[:8]:
                try:
                    rel_base = join_url(base, d)
                    rel_page = http_get_text(rel_base)
                    rel_links = parse_apache_listing_for_links(rel_page)
                    iso_names = sorted({l for l in rel_links if l.lower().endswith(".iso")})
                    if not iso_names:
                        iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", rel_page)))
                        if not iso_names:
                            continue
                    sums: dict[str, str] = {}
                    for candidate in ("sha256sums.txt", "SHA256SUMS", "SHA256SUMS.txt"):
                        if candidate in rel_links:
                            try:
                                sums = parse_checksum_lines(http_get_text(join_url(rel_base, candidate)))
                                break
                            except Exception:
                                pass
                    items = []
                    for name in iso_names:
                        items.append(RemoteIsoItem(name=name, url=join_url(rel_base, name), sha256=sums.get(name)))
                    if items:
                        return items
                except Exception as e:
                    last_err = e
            if last_err is not None:
                raise RuntimeError(f"Parrot OS listing failed: {last_err}")
            raise RuntimeError("Parrot OS listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "OpenBSD":
            raise RuntimeError("OpenBSD is best downloaded via official mirrors. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Debian":
            base = "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            sums: dict[str, str] = {}
            if "SHA256SUMS" in links:
                sums_text = http_get_text(join_url(base, "SHA256SUMS"))
                sums = parse_checksum_lines(sums_text)
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "Fedora":
            last_err: Exception | None = None

            # 1) Official release metadata: direct ISO links + sha256, no directory listing needed.
            try:
                data = json.loads(http_get_text("https://fedoraproject.org/releases.json"))
                entries = data if isinstance(data, list) else data.get("releases", [])
                ws = [
                    e for e in entries
                    if isinstance(e, dict)
                    and str(e.get("version", "")).strip().isdigit()
                    and e.get("arch") == "x86_64"
                    and str(e.get("link", "")).lower().endswith(".iso")
                ]
                if ws:
                    newest = max(int(str(e["version"]).strip()) for e in ws)
                    items = []
                    seen_links: set[str] = set()
                    for e in ws:
                        if int(str(e["version"]).strip()) != newest:
                            continue
                        link = str(e["link"]).strip()
                        if link in seen_links:
                            continue
                        seen_links.add(link)
                        name = link.rsplit("/", 1)[-1]
                        sha = str(e.get("sha256") or "").strip().lower() or None
                        items.append(RemoteIsoItem(name=name, url=link, sha256=sha))
                    # Workstation first, then the rest alphabetically.
                    items.sort(key=lambda it: (0 if "workstation" in it.name.lower() else 1, it.name))
                    if items:
                        return items
            except Exception as e:
                last_err = e

            # 2) Fallback: scrape the release directory listing.
            bases = [
                "https://dl.fedoraproject.org/pub/fedora/linux/releases/",
                "https://download.fedoraproject.org/pub/fedora/linux/releases/",
                "https://mirrors.kernel.org/fedora/releases/",
            ]
            for base in bases:
                try:
                    index = http_get_text(base)
                except Exception as e:
                    last_err = e
                    continue

                # Accept relative ("44/"), dotted ("./44/") and absolute (".../releases/44/") hrefs.
                rels = sorted(
                    {int(m) for m in re.findall(r"href=[\"'](?:[^\"']*/)?(\d{2,3})/[\"']", index)},
                    reverse=True,
                )
                for rel in rels[:3]:
                    ws_base = f"{base}{rel}/Workstation/x86_64/iso/"
                    try:
                        page = http_get_text(ws_base)
                    except Exception as e:
                        last_err = e
                        continue
                    iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)(?=[\"'<])", page)))
                    if not iso_names:
                        continue
                    sums: dict[str, str] = {}
                    for chk in set(re.findall(r"([A-Za-z0-9._-]+CHECKSUM)(?=[\"'<])", page)):
                        try:
                            for line in http_get_text(join_url(ws_base, chk)).splitlines():
                                m = re.match(r"^SHA256 \((.+)\) = ([a-fA-F0-9]{64})$", line.strip())
                                if m:
                                    sums[m.group(1).strip()] = m.group(2).lower()
                        except Exception:
                            pass
                    return [
                        RemoteIsoItem(name=n, url=join_url(ws_base, n), sha256=sums.get(n)) for n in iso_names
                    ]

            if last_err is not None:
                raise RuntimeError(f"Fedora listing failed: {last_err}")
            raise RuntimeError("Fedora listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "SystemRescue":
            base = "https://download.system-rescue.org/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            if "latest/" in links:
                base = join_url(base, "latest/")
                page = http_get_text(base)
                links = parse_apache_listing_for_links(page)

            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            sums: dict[str, str] = {}
            sha_files = [l for l in links if l.lower().endswith(".sha256")]
            for sha in sha_files:
                txt = http_get_text(join_url(base, sha))
                sums.update(parse_checksum_lines(txt))

            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "Arch Linux":
            base = "https://geo.mirror.pkgbuild.com/iso/latest/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            sums: dict[str, str] = {}
            sha_candidates = [l for l in links if l.lower().endswith("sha256") or l.lower().endswith("sha256sums.txt")]
            if sha_candidates:
                txt = http_get_text(join_url(base, sha_candidates[0]))
                sums = parse_checksum_lines(txt)
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "Kali Linux":
            base = "https://cdimage.kali.org/current/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            sums: dict[str, str] = {}
            if "SHA256SUMS" in links:
                sums_text = http_get_text(join_url(base, "SHA256SUMS"))
                sums = parse_checksum_lines(sums_text)
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "Alpine Linux":
            base = "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            sums: dict[str, str] = {}
            sha_files = [l for l in links if l.lower().endswith(".sha256") or l.lower().endswith("sha256sums")]
            if sha_files:
                sums_text = http_get_text(join_url(base, sha_files[0]))
                sums = parse_checksum_lines(sums_text)
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "Void Linux":
            base = "https://repo-default.voidlinux.org/live/current/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            if not iso_names:
                raise RuntimeError("No Void Linux ISOs found")

            sums: dict[str, str] = {}
            for candidate in ("sha256sum.txt", "SHA256SUMS", "sha256sums.txt"):
                if candidate in links:
                    try:
                        sums = parse_checksum_lines(http_get_text(join_url(base, candidate)))
                        break
                    except Exception:
                        pass

            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "openSUSE Leap":
            # Discover latest 15.x folder instead of hardcoding.
            roots = [
                "https://download.opensuse.org/distribution/leap/",
                "https://downloadcontent.opensuse.org/distribution/leap/",
            ]

            root = roots[0]
            idx = None
            last_root_err: Exception | None = None
            for r in roots:
                try:
                    idx = http_get_text(r)
                    root = r
                    break
                except Exception as e:
                    last_root_err = e
            if idx is None:
                if last_root_err is not None:
                    raise RuntimeError(f"openSUSE Leap listing failed: {last_root_err}")
                raise RuntimeError("openSUSE Leap listing failed")

            root_links = parse_apache_listing_for_links(idx)
            vers = []
            for href in root_links:
                m = re.match(r"^(15\.\d+)/$", href)
                if m:
                    vers.append(m.group(1))
            if not vers:
                vers = sorted(set(re.findall(r"(15\.\d+)/", idx)))
            if not vers:
                vers = []
            if vers:
                vers.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
                ver_candidates = vers[:2]
            else:
                # Fallback known versions
                ver_candidates = ["15.6", "15.5"]

            last_err: Exception | None = None
            for ver in ver_candidates:
                for rel in ("iso/", "live/"):
                    base = join_url(root, f"{ver}/{rel}")
                    try:
                        page = http_get_text(base)
                    except Exception as e:
                        last_err = e
                        continue

                    links = parse_apache_listing_for_links(page)
                    iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
                    if not iso_names:
                        # Catch .iso links that include query strings
                        iso_hrefs = re.findall(r"href=[\"']([^\"']+\.iso[^\"']*)[\"']", page, flags=re.IGNORECASE)
                        iso_names = []
                        for href in iso_hrefs:
                            name = href.split("?")[0].split("/")[-1]
                            if name.lower().endswith(".iso"):
                                iso_names.append(name)
                        iso_names = sorted(set(iso_names))
                    if not iso_names:
                        continue

                    sums: dict[str, str] = {}
                    for name in iso_names:
                        sha_url = join_url(base, name + ".sha256")
                        try:
                            sha_txt = http_get_text(sha_url)
                            sums.update(parse_checksum_lines(sha_txt))
                        except Exception:
                            pass
                    items = []
                    for name in iso_names:
                        items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
                    if items:
                        return items

            if last_err is not None:
                raise RuntimeError(f"openSUSE Leap listing failed: {last_err}")
            raise RuntimeError("No items")

        if source == "openSUSE Tumbleweed":
            bases = [
                "https://download.opensuse.org/tumbleweed/iso/",
                "https://downloadcontent.opensuse.org/tumbleweed/iso/",
            ]

            last_err: Exception | None = None
            for base in bases:
                try:
                    page = http_get_text(base)
                except Exception as e:
                    last_err = e
                    continue

                links = parse_apache_listing_for_links(page)
                iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
                if not iso_names:
                    iso_hrefs = re.findall(r"href=[\"']([^\"']+\.iso[^\"']*)[\"']", page, flags=re.IGNORECASE)
                    iso_names = []
                    for href in iso_hrefs:
                        name = href.split("?")[0].split("/")[-1]
                        if name.lower().endswith(".iso"):
                            iso_names.append(name)
                    iso_names = sorted(set(iso_names))
                if not iso_names:
                    continue

                sums: dict[str, str] = {}
                for name in iso_names:
                    sha_url = join_url(base, name + ".sha256")
                    try:
                        sha_txt = http_get_text(sha_url)
                        sums.update(parse_checksum_lines(sha_txt))
                    except Exception:
                        pass
                items = []
                for name in iso_names:
                    items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
                if items:
                    return items

            if last_err is not None:
                raise RuntimeError(f"openSUSE Tumbleweed listing failed: {last_err}")
            raise RuntimeError("No openSUSE Tumbleweed ISOs found")
            sums: dict[str, str] = {}
            for name in iso_names:
                sha_url = join_url(base, name + ".sha256")
                try:
                    sha_txt = http_get_text(sha_url)
                    sums.update(parse_checksum_lines(sha_txt))
                except Exception:
                    pass
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "Pop!_OS":
            base = "https://iso.pop-os.org/"
            try:
                index = http_get_text(base)
            except Exception as e:
                # Some networks get 403 from this host.
                raise RuntimeError(f"{e}. Use 'Open Source Page'.")
            links = parse_apache_listing_for_links(index)

            # Pop!_OS layout often uses version/arch/vendor subfolders.
            vers = []
            for href in links:
                m = re.match(r"^(\d+\.\d+)/$", href)
                if m:
                    vers.append(m.group(1))
            if not vers:
                # fallback: some listings might not have trailing slash in our parser
                vers = sorted(set(re.findall(r"(\d+\.\d+)/", index)))
            if not vers:
                raise RuntimeError("No Pop!_OS version folders found")

            vers.sort(key=lambda s: [int(p) for p in s.split(".")], reverse=True)
            ver = vers[0]

            dir_candidates = [
                f"{ver}/amd64/intel/",
                f"{ver}/amd64/nvidia/",
                f"{ver}/amd64/",
                f"{ver}/intel/",
                f"{ver}/nvidia/",
                f"{ver}/",
            ]

            last_err: Exception | None = None
            all_items: list[RemoteIsoItem] = []
            for rel in dir_candidates:
                folder = join_url(base, rel)
                try:
                    page = http_get_text(folder)
                except Exception as e:
                    last_err = e
                    continue
                f_links = parse_apache_listing_for_links(page)
                iso_names = sorted({l for l in f_links if l.lower().endswith(".iso")})
                if not iso_names:
                    iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
                    if not iso_names:
                        continue

                sums: dict[str, str] = {}
                sha_files = [l for l in f_links if l.lower().endswith(".sha256sum") or l.lower().endswith(".sha256")]
                for sha in sha_files:
                    try:
                        sums.update(parse_checksum_lines(http_get_text(join_url(folder, sha))))
                    except Exception:
                        pass

                for name in iso_names:
                    label = name
                    low = folder.lower()
                    if "nvidia" in low:
                        label = "nvidia-" + name
                    elif "intel" in low:
                        label = "intel-" + name
                    all_items.append(RemoteIsoItem(name=label, url=join_url(folder, name), sha256=sums.get(name)))

            if all_items:
                all_items.sort(key=lambda x: x.name.lower())
                return all_items

            if last_err is not None:
                raise RuntimeError(f"Pop!_OS listing failed: {last_err}")
            raise RuntimeError("Pop!_OS listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Manjaro":
            # Manjaro offers editions in subfolders; we list from the top-level download pages
            # that are typically directory listings.
            candidates = [
                "https://download.manjaro.org/xfce/",
                "https://download.manjaro.org/kde/",
                "https://download.manjaro.org/gnome/",
            ]
            items: list[RemoteIsoItem] = []
            for base in candidates:
                try:
                    page = http_get_text(base)
                except Exception:
                    continue
                links = parse_apache_listing_for_links(page)
                iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
                if not iso_names:
                    iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
                    if not iso_names:
                        continue

                sums: dict[str, str] = {}
                sha_files = [l for l in links if l.lower().endswith(".sha256") or l.lower().endswith("sha256")]
                for sha in sha_files[:3]:
                    try:
                        sums.update(parse_checksum_lines(http_get_text(join_url(base, sha))))
                    except Exception:
                        pass

                for name in iso_names:
                    items.append(RemoteIsoItem(name=f"{Path(base).name}-{name}", url=join_url(base, name), sha256=sums.get(name)))

            items.sort(key=lambda x: x.name.lower())
            if not items:
                raise RuntimeError("No Manjaro ISOs found (server layout may have changed)")
            return items

        if source == "Gentoo":
            base = "https://distfiles.gentoo.org/releases/amd64/autobuilds/current-install-amd64-minimal/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            if not iso_names:
                raise RuntimeError("No Gentoo minimal ISO found")
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=None))
            return items

        if source == "Slackware":
            # ISOs live under slackware-iso/slackware64-<ver>-iso/ (not directly under slackware/).
            roots = [
                "https://mirrors.slackware.com/slackware/slackware-iso/",
                "https://slackware.uk/slackware/slackware-iso/",
                "https://mirrors.kernel.org/slackware/slackware-iso/",
                "https://ftp.osuosl.org/pub/slackware/slackware-iso/",
                "https://mirrors.ocf.berkeley.edu/slackware/slackware-iso/",
            ]
            last_err: Exception | None = None
            for root in roots:
                try:
                    index = http_get_text(root)
                except Exception as e:
                    last_err = e
                    continue
                vers = set(re.findall(r"slackware64-(\d+(?:\.\d+)*)-iso/", index))
                if not vers:
                    vers = {"15.0"}
                for ver in sorted(vers, key=lambda s: [int(p) for p in s.split(".")], reverse=True):
                    base = f"{root}slackware64-{ver}-iso/"
                    try:
                        page = http_get_text(base)
                    except Exception as e:
                        last_err = e
                        continue
                    iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)(?=[\"'<])", page)))
                    if not iso_names:
                        continue
                    # Slackware publishes MD5 only, so no sha256 is attached.
                    return [RemoteIsoItem(name=n, url=join_url(base, n), sha256=None) for n in iso_names]
            if last_err is not None:
                raise RuntimeError(f"Slackware listing failed: {last_err}")
            raise RuntimeError("Slackware listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Rocky Linux":
            base = "https://download.rockylinux.org/pub/rocky/9/isos/x86_64/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            if not iso_names:
                raise RuntimeError("No Rocky Linux ISOs found")
            sums: dict[str, str] = {}
            for candidate in ("CHECKSUM", "CHECKSUMS", "sha256sum.txt", "SHA256SUMS"):
                if candidate in links:
                    try:
                        sums = parse_checksum_lines(http_get_text(join_url(base, candidate)))
                        break
                    except Exception:
                        pass
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "AlmaLinux":
            base = "https://repo.almalinux.org/almalinux/9/isos/x86_64/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            if not iso_names:
                raise RuntimeError("No AlmaLinux ISOs found")
            sums: dict[str, str] = {}
            for candidate in ("CHECKSUM", "CHECKSUMS", "sha256sum.txt", "SHA256SUMS"):
                if candidate in links:
                    try:
                        sums = parse_checksum_lines(http_get_text(join_url(base, candidate)))
                        break
                    except Exception:
                        pass
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if source == "CentOS Stream":
            candidates = [
                "https://mirror.stream.centos.org/10-stream/isos/x86_64/",
                "https://mirror.stream.centos.org/9-stream/isos/x86_64/",
            ]
            last_err: Exception | None = None
            for base in candidates:
                try:
                    page = http_get_text(base)
                except Exception as e:
                    last_err = e
                    continue
                links = parse_apache_listing_for_links(page)
                iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
                if not iso_names:
                    iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)", page)))
                    if not iso_names:
                        continue
                sums: dict[str, str] = {}
                for candidate in ("sha256sum.txt", "SHA256SUMS", "CHECKSUM", "CHECKSUMS"):
                    if candidate in links:
                        try:
                            sums = parse_checksum_lines(http_get_text(join_url(base, candidate)))
                            break
                        except Exception:
                            pass
                items = []
                for name in iso_names:
                    items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
                if items:
                    return items
            if last_err is not None:
                raise RuntimeError(f"CentOS Stream listing failed: {last_err}")
            raise RuntimeError("CentOS Stream listing failed. Use 'Open Source Page' or 'Custom Source'.")

        if source == "Tiny Core Linux":
            # tinycorelinux.net/ is a normal homepage (no directory listing), so version
            # folders like 16.x/ can't be discovered there. Use listing mirrors, then probe.
            bases = [
                "https://distro.ibiblio.org/tinycorelinux/",
                "https://mirrors.kernel.org/tinycorelinux/",
                "http://tinycorelinux.net/",
            ]
            last_err: Exception | None = None
            for base in bases:
                vers: list[int] = []
                try:
                    index = http_get_text(base)
                    vers = sorted({int(v) for v in re.findall(r"href=[\"'](?:[^\"']*/)?(\d+)\.x/[\"']", index)}, reverse=True)
                except Exception as e:
                    last_err = e
                if not vers:
                    vers = list(range(20, 12, -1))  # probe newest-first
                for v in vers:
                    rel_base = f"{base}{v}.x/x86/release/"
                    try:
                        page = http_get_text(rel_base, timeout=10.0)
                    except Exception as e:
                        last_err = e
                        continue
                    iso_names = sorted(set(re.findall(r"([A-Za-z0-9._-]+\.iso)(?=[\"'<])", page)))
                    if not iso_names:
                        continue
                    return [RemoteIsoItem(name=f"[{v}.x] {n}", url=join_url(rel_base, n), sha256=None) for n in iso_names]
            if last_err is not None:
                raise RuntimeError(f"Tiny Core Linux listing failed: {last_err}")
            raise RuntimeError("No Tiny Core versions found")

        if source == "EndeavourOS":
            candidates = [
                "https://mirror.alpix.eu/endeavouros/iso/",
                "https://mirrors.gigenet.com/endeavouros/iso/",
                "https://ca.gate.endeavouros.com/iso/",
                "https://mirror.moson.org/endeavouros/iso/",
            ]
            last_err: Exception | None = None
            for base in candidates:
                try:
                    page = http_get_text(base)
                except Exception as e:
                    last_err = e
                    continue
                iso_names = sorted(set(re.findall(r"(EndeavourOS[A-Za-z0-9._-]*\.iso)(?=[\"'<])", page)), reverse=True)
                if not iso_names:
                    continue
                # EndeavourOS publishes sha512 only, so no sha256 is attached.
                return [RemoteIsoItem(name=n, url=join_url(base, n), sha256=None) for n in iso_names]
            if last_err is not None:
                raise RuntimeError(f"EndeavourOS listing failed: {last_err}")
            raise RuntimeError("EndeavourOS listing failed. Use 'Open Source Page'.")

        if source in (
            "Garuda Linux",
            "KDE neon",
            "NixOS",
            "Puppy Linux",
            "Tails",
            "Rescuezilla",
            "GParted Live",
            "Clonezilla",
            "Zorin OS",
            "Deepin",
        ):
            raise RuntimeError("This source is best downloaded via its official page. Use 'Open Source Page'.")

        if source == "Custom Source":
            if not custom_base:
                raise RuntimeError("Custom Source requires an ISO directory URL")
            base = custom_base if custom_base.endswith("/") else custom_base + "/"
            page = http_get_text(base)
            links = parse_apache_listing_for_links(page)
            iso_names = sorted({l for l in links if l.lower().endswith(".iso")})
            sums: dict[str, str] = {}
            if custom_sum:
                sums_text = http_get_text(custom_sum)
                sums = parse_checksum_lines(sums_text)
            else:
                for candidate in ("SHA256SUMS", "sha256sums.txt", "SHA256SUMS.txt"):
                    if candidate in links:
                        try:
                            sums_text = http_get_text(join_url(base, candidate))
                            sums = parse_checksum_lines(sums_text)
                            break
                        except Exception:
                            pass
            items = []
            for name in iso_names:
                items.append(RemoteIsoItem(name=name, url=join_url(base, name), sha256=sums.get(name)))
            return items

        if is_windows_source(source):
            level = int(getattr(self, "_web_archive_level", 0) or 0)
            try:
                win_items = windows_iso_search(source, level)
            except Exception as e:
                raise RuntimeError(
                    f"{source}: Internet Archive lookup failed ({e}). "
                    "Use 'Open Source Page' or 'Custom Source'."
                )
            if not win_items:
                raise RuntimeError(
                    f"{source}: no installer ISO found in the online archives. "
                    "Use 'Open Source Page' or 'Custom Source'."
                )
            return win_items

        if is_ia_source(source):
            level = int(getattr(self, "_web_archive_level", 0) or 0)
            try:
                ia_items = ia_iso_search(source, level)
            except Exception as e:
                raise RuntimeError(
                    f"{source}: Archive.org lookup failed ({e}). "
                    "Use 'Open Source Page' or 'Custom Source'."
                )
            if not ia_items:
                raise RuntimeError(
                    f"{source}: no ISO found in the Archive.org catalogue. "
                    "Raise 'Load more' depth or use 'Open Source Page'."
                )
            return ia_items

        raise RuntimeError(f"Unsupported source: {source}")

    def _download_clicked(self):
        items = self._selected_remotes()
        if not items:
            return

        if len(items) == 1:
            iso = items[0]
            target = filedialog.asksaveasfilename(
                title="Save ISO as",
                defaultextension=".iso",
                initialfile=iso.name,
                filetypes=[("ISO files", "*.iso"), ("All files", "*.*")],
            )
            if not target:
                return

            self._stop_download.clear()
            self._set_net_progress(0.0)
            self._set_net_status("Queued")
            self._set_remote_actions_enabled(False)

            self._enqueue_download(url=iso.url, target=Path(target), expected_sha256=iso.sha256, label=iso.name)
            return

        out_dir = filedialog.askdirectory(title=f"Select folder for {len(items)} downloads")
        if not out_dir:
            return
        out_dir_p = Path(out_dir)

        self._stop_download.clear()
        self._set_net_progress(0.0)
        self._set_net_status(f"Queued {len(items)} file(s)")
        self._set_remote_actions_enabled(False)

        for it in items:
            filename = it.name
            if not filename.lower().endswith(".iso"):
                filename += ".iso"
            self._enqueue_download(url=it.url, target=(out_dir_p / filename), expected_sha256=it.sha256, label=it.name)

    def _stop_download_clicked(self):
        self._stop_download.set()
        self._stop_validate.set()
        self._stop_audit.set()

        # Also clear queued downloads.
        try:
            with self._download_queue_lock:
                while True:
                    try:
                        self._download_queue.get_nowait()
                    except queue.Empty:
                        break
                self._download_queue_count = 0
            self._work_q.put(("download_batch_status", "Download queue cleared", None))
            for job_id, job in list(getattr(self, "_download_jobs", {}).items()):
                if str(job.get("status")) in ("Queued", "Paused", "Downloading"):
                    self._work_q.put(("download_job_update", {
                        "job_id": job_id, "status": "Cancelled", "speed": None, "eta": None,
                    }, None))
        except Exception:
            pass

    def _enqueue_download(self, *, url: str, target: Path, expected_sha256: str | None, label: str) -> None:
        try:
            self._download_job_seq = int(self._download_job_seq) + 1
        except Exception:
            self._download_job_seq = 1
        job_id = f"dl{self._download_job_seq}"

        verify = False
        try:
            verify = bool(getattr(self, "_auto_verify_var", None).get())
        except Exception:
            verify = False

        job = {
            "job_id": job_id,
            "url": str(url),
            "target": str(target),
            "sha256": (str(expected_sha256).strip().lower() if expected_sha256 else None),
            "label": str(label),
            "retries_left": 2,
            "pause_event": threading.Event(),
            "status": "Queued",
            "progress": 0.0,
            "verify": bool(verify),
            "in_queue": False,
        }
        try:
            self._download_jobs[job_id] = job
            self._requeue_job(job)
            self._log(f"Queued download: {label}")
            self._work_q.put(("download_batch_status", f"Queued: {label}", None))
            self._work_q.put(("download_job_added", {"job_id": job_id, "label": label, "target": str(target)}, None))
            self._save_download_jobs(force=True)
        except Exception:
            pass

    def _download_manager_worker(self) -> None:
        while not self._download_manager_stop.is_set():
            try:
                job = self._download_queue.get(timeout=0.25)
            except queue.Empty:
                continue

            try:
                with self._download_queue_lock:
                    self._download_queue_count = max(0, int(self._download_queue_count) - 1)
            except Exception:
                pass

            try:
                job["in_queue"] = False
            except Exception:
                pass

            try:
                if self._download_manager_stop.is_set():
                    return
                if self._stop_download.is_set():
                    continue

                try:
                    jid = str(job.get("job_id") or "")
                except Exception:
                    jid = ""

                try:
                    pause_ev = job.get("pause_event")
                    if pause_ev is not None and pause_ev.is_set():
                        # Put it back and skip for now.
                        self._requeue_job(job)
                        time.sleep(0.2)
                        continue
                except Exception:
                    pass

                url = str(job.get("url") or "")
                target = Path(str(job.get("target") or ""))
                sha = job.get("sha256")
                label = str(job.get("label") or target.name or "download")
                retries_left = int(job.get("retries_left") or 0)
                verify = bool(job.get("verify"))

                try:
                    pending = int(self._download_queue_count)
                except Exception:
                    pending = 0

                self._download_active.set()
                self._work_q.put(("download_batch_status", f"Downloading: {label} ({pending} queued)", None))
                self._log(f"Download started: {label} -> {target}")

                if jid:
                    self._work_q.put(("download_job_update", {"job_id": jid, "status": "Downloading", "progress": 0.0}, None))

                def prog(frac: float, done=None, total=None, speed=None, eta=None, job_id=jid):
                    try:
                        self._work_q.put(("download_job_progress", {
                            "job_id": job_id,
                            "progress": float(frac),
                            "done_bytes": done,
                            "total_bytes": total,
                            "speed": speed,
                            "eta": eta,
                        }, None))
                        self._work_q.put(("download_progress", float(frac), None))
                    except Exception:
                        pass

                self._download_url_to_file(url, target, expected_sha256=(sha if verify else None), progress_cb=prog, resume=True, job=job)
                self._log(f"Download complete: {label}")
                if jid:
                    size_done = None
                    try:
                        size_done = int(target.stat().st_size)
                    except Exception:
                        size_done = None
                    self._work_q.put(("download_job_update", {
                        "job_id": jid, "status": "Done", "progress": 1.0,
                        "done_bytes": size_done, "total_bytes": size_done,
                        "speed": None, "eta": None,
                    }, None))
                self._work_q.put(("download_done", str(target), (sha is not None and verify)))
            except Exception as e:
                try:
                    label2 = str(job.get("label") or "download")
                except Exception:
                    label2 = "download"
                try:
                    retries_left = int(job.get("retries_left") or 0)
                except Exception:
                    retries_left = 0

                if self._stop_download.is_set():
                    self._log(f"Download cancelled: {label2}")
                    if jid:
                        self._work_q.put(("download_job_update", {
                            "job_id": jid, "status": "Cancelled", "speed": None, "eta": None,
                        }, None))
                    continue

                if retries_left > 0:
                    try:
                        job["retries_left"] = retries_left - 1
                    except Exception:
                        pass
                    self._log(f"Download failed (retrying): {label2} ({e})")
                    self._requeue_job(job)
                    if jid:
                        self._work_q.put(("download_job_update", {
                            "job_id": jid, "status": "Queued", "speed": None, "eta": None,
                            "note": str(e),
                        }, None))
                else:
                    self._log(f"Download failed: {label2} ({e})")
                    if jid:
                        self._work_q.put(("download_job_update", {
                            "job_id": jid, "status": "Failed", "speed": None, "eta": None,
                            "note": str(e),
                        }, None))
                    self._work_q.put(("error_net", f"Download failed: {e}", None))
            finally:
                try:
                    self._download_active.clear()
                except Exception:
                    pass

    def _download_url_to_file(self, url: str, target: Path, expected_sha256: str | None, progress_cb=None, *, resume: bool = True, job: dict | None = None):
        target.parent.mkdir(parents=True, exist_ok=True)

        existing = 0
        if resume:
            try:
                if target.exists():
                    existing = int(target.stat().st_size)
            except Exception:
                existing = 0

        headers = {"User-Agent": "IsoPackageManager/1.0 (+https://example.invalid)"}
        if resume and existing > 0:
            headers["Range"] = f"bytes={existing}-"

        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=30.0, context=ssl_context_for_https()) as resp:
            try:
                status = int(getattr(resp, "status", 200) or 200)
            except Exception:
                status = 200

            if existing > 0 and status != 206:
                existing = 0

            total = resp.headers.get("Content-Length")
            rem_bytes = int(total) if total and total.isdigit() else None
            total_bytes = (existing + rem_bytes) if rem_bytes is not None else None

            done = existing
            mode = "ab" if existing > 0 else "wb"

            # Speed / ETA bookkeeping. The average is taken over a short sliding
            # window so a slow patch does not make the estimate jump around.
            started = time.monotonic()
            window: list[tuple[float, int]] = [(started, done)]
            last_post = 0.0

            with target.open(mode) as f:
                while True:
                    if self._stop_download.is_set():
                        raise RuntimeError("Download cancelled")

                    try:
                        if job is not None:
                            ev = job.get("pause_event")
                            while ev is not None and ev.is_set():
                                if self._stop_download.is_set():
                                    raise RuntimeError("Download cancelled")
                                time.sleep(0.2)
                                # Ignore the paused time when measuring speed.
                                window = [(time.monotonic(), done)]
                    except Exception:
                        pass

                    chunk = resp.read(1024 * 1024)
                    if not chunk:
                        break
                    f.write(chunk)
                    done += len(chunk)

                    now = time.monotonic()
                    window.append((now, done))
                    while len(window) > 2 and now - window[0][0] > 8.0:
                        window.pop(0)

                    speed = None
                    eta = None
                    try:
                        span = now - window[0][0]
                        if span > 0.35:
                            speed = (done - window[0][1]) / span
                            if speed > 0 and total_bytes:
                                eta = max(0.0, (total_bytes - done) / speed)
                    except Exception:
                        speed = None

                    frac = (done / total_bytes) if total_bytes else 0.0
                    if now - last_post >= 0.25 or done >= (total_bytes or 0):
                        last_post = now
                        if progress_cb is not None:
                            try:
                                progress_cb(frac, done, total_bytes, speed, eta)
                            except TypeError:
                                progress_cb(frac)
                        elif total_bytes:
                            self._work_q.put(("download_progress", frac, None))

        if expected_sha256:
            h = hashlib.sha256()
            with target.open("rb") as f:
                while True:
                    chunk = f.read(1024 * 1024)
                    if not chunk:
                        break
                    h.update(chunk)
            got = h.hexdigest().lower()
            if got != expected_sha256.lower():
                raise RuntimeError("SHA-256 verification failed")

    def _set_status(self, text: str):
        self._status_var.set(text)

    def _set_progress(self, frac: float | None):
        if frac is None:
            self._progress.configure(mode="indeterminate")
            self._progress.start(10)
            return
        self._progress.stop()
        self._progress.configure(mode="determinate")
        self._progress["value"] = max(0.0, min(100.0, frac * 100.0))

    def _set_actions_enabled(self, enabled: bool):
        state = "normal" if enabled else "disabled"
        for b in (
            self._btn_mount_open,
            self._btn_details,
            getattr(self, "_btn_dupes", None),
            self._btn_extract,
            self._btn_copy,
            getattr(self, "_btn_eject", None),
        ):
            if not b:
                continue
            b.configure(state=state)

    def _add_folder(self):
        d = filedialog.askdirectory(title="Select folder to scan")
        if not d:
            return
        self._folders_list.insert(tk.END, d)

    def _remove_selected_folders(self):
        sel = list(self._folders_list.curselection())
        if not sel:
            return
        for idx in reversed(sel):
            self._folders_list.delete(idx)

    def _clear_filter(self):
        self._filter_var.set("")
        self._distro_filter_var.set("All")
        self._apply_filter()

    def _folders(self) -> list[Path]:
        out: list[Path] = []
        for i in range(self._folders_list.size()):
            p = Path(self._folders_list.get(i))
            if p.exists() and p.is_dir():
                out.append(p)
        return out

    def _start_scan(self):
        if self._scan_thread and self._scan_thread.is_alive():
            return
        folders = self._folders()
        if not folders:
            messagebox.showwarning("No folders", "Add at least one valid folder to scan.")
            return

        self._stop_scan.clear()
        self._set_progress(None)
        self._set_status("Scanning...")
        self._set_actions_enabled(False)

        self._set_scan_running(True)
        self._scan_thread = threading.Thread(target=self._scan_worker, args=(folders,), daemon=True)
        self._scan_thread.start()

    def _set_scan_running(self, running: bool) -> None:
        """Only offer Stop while a scan is actually going."""
        for name, on in (("_btn_scan", not running), ("_btn_stop_scan", running)):
            button = getattr(self, name, None)
            if button is None:
                continue
            try:
                button.configure(state="normal" if on else "disabled")
            except Exception:
                pass

    def _stop_scan_clicked(self):
        self._stop_scan.set()

    def _scan_worker(self, folders: list[Path]):
        try:
            found: list[IsoItem] = []
            scanned_dirs = 0
            start = time.time()

            def consider_file(fp: Path):
                if fp.suffix.lower() in ISO_EXTENSIONS:
                    found.append(IsoItem(fp))

            for root in folders:
                for dirpath, dirnames, filenames in os.walk(root):
                    if self._stop_scan.is_set():
                        self._work_q.put(("scan_done", None, True))
                        return

                    scanned_dirs += 1
                    if scanned_dirs % 50 == 0:
                        self._work_q.put(("scan_status", f"Scanning... ({scanned_dirs} folders)", None))

                    base = Path(dirpath)
                    for fn in filenames:
                        if fn.lower().endswith(".iso"):
                            consider_file(base / fn)

            found.sort(key=lambda x: x.name.lower())
            elapsed = time.time() - start
            self._work_q.put(("scan_done", (found, elapsed), False))
        except Exception as e:
            self._work_q.put(("error", f"Scan failed: {e}", None))

    def _apply_filter(self):
        term = (self._filter_var.get() or "").strip().lower()
        distro = (self._distro_filter_var.get() or "").strip()
        self._tree.delete(*self._tree.get_children())
        visible = []
        # Normalized distro keywords for matching
        distro_keywords = {
            "linux mint": ["linuxmint", "linux mint"],
            "elementary os": ["elementary", "elementaryos", "elementary os"],
            "zorin os": ["zorin", "zorinos", "zorin os"],
            "deepin": ["deepin"],
            "mx linux": ["mx", "mxlinux", "mx linux"],
            "ubuntu": ["ubuntu"],
            "debian": ["debian"],
            "fedora": ["fedora"],
            "arch": ["arch", "archlinux", "arch linux"],
            "kali": ["kali", "kalilinux", "kali linux"],
            "alpine": ["alpine", "alpinelinux", "alpine linux"],
            "opensuse": ["opensuse", "opensuse leap", "suse"],
            "pop!_os": ["pop", "pop_os", "pop!_os", "pop! os"],
            "manjaro": ["manjaro", "manjarolinux", "manjaro linux"],
            "freebsd": ["freebsd"],
            "openbsd": ["openbsd"],
            "netbsd": ["netbsd"],
        }
        for item in self._items:
            name_lower = item.name.lower()
            path_lower = str(item.path).lower()
            # Name/text filter
            if term and not (term in name_lower or term in path_lower):
                continue
            # Distro filter
            if distro and distro != "All":
                distro_lc = distro.lower()
                keywords = distro_keywords.get(distro_lc, [distro_lc])
                if not any(kw in name_lower for kw in keywords):
                    continue
            visible.append(item)

        for idx, item in enumerate(visible):
            m = datetime.fromtimestamp(item.mtime).strftime("%Y-%m-%d %H:%M")
            info = self._local_meta_text(item)
            self._tree.insert(
                "",
                tk.END,
                iid=str(item.path),
                values=(item.name, info, str(item.path), human_bytes(item.size_bytes), m),
            )

        self._stripe_rows(self._tree)
        self._set_status(f"Found {len(visible)} ISO(s) (filtered from {len(self._items)})")
        try:
            total_size = sum(int(getattr(i, "size_bytes", 0) or 0) for i in visible)
            if visible:
                self._local_count_var.set(
                    f"{len(visible)} / {len(self._items)} ISO  ·  {human_bytes(total_size)}"
                )
            else:
                self._local_count_var.set("")
        except Exception:
            pass
        self._set_actions_enabled(False)

    def _on_selection_changed(self):
        iso = self._selected_iso()
        self._set_actions_enabled(iso is not None)

    def _selected_iso(self) -> IsoItem | None:
        sel = self._tree.selection()
        if not sel:
            return None
        p = Path(sel[0])
        for it in self._items:
            if it.path == p:
                return it
        return None

    def _copy_path_clicked(self):
        iso = self._selected_iso()
        if not iso:
            return
        self.clipboard_clear()
        self.clipboard_append(str(iso.path))
        self._set_status("Copied path to clipboard")

    def _mount_and_open_clicked(self):
        iso = self._selected_iso()
        if not iso:
            return

        if not mount_supported():
            try:
                open_path_default(iso.path)
                self._set_status(f"Opened with default application ({platform_label()}: no mount backend)")
            except Exception as e:
                messagebox.showerror("Error", f"Open failed: {e}")
            return

        self._set_status(f"Mounting ({mount_backend()})...")
        self._set_progress(None)
        self._set_actions_enabled(False)

        def worker():
            try:
                mount_iso(iso.path)
                point = None
                for _ in range(30):
                    point = get_mount_point(iso.path)
                    if point:
                        break
                    time.sleep(0.2)

                self._work_q.put(("mount_done", (str(iso.path), point), None))
            except Exception as e:
                self._work_q.put(("error", f"Mount failed: {e}", None))

        threading.Thread(target=worker, daemon=True).start()

    def _eject_clicked(self):
        """Unmount / eject a previously mounted ISO on any platform."""
        iso = self._selected_iso()
        if not iso:
            return

        if not mount_supported():
            messagebox.showinfo(
                "Eject ISO",
                f"Ejecting is not supported on {platform_label()}.\n\nUnmount the image with your OS tools.",
            )
            return

        self._set_status("Ejecting...")
        self._set_actions_enabled(False)

        def worker():
            try:
                ok = unmount_iso(iso.path)
                self._work_q.put(("eject_done", (str(iso.path), ok), None))
            except Exception as e:
                self._work_q.put(("error", f"Eject failed: {e}", None))

        threading.Thread(target=worker, daemon=True).start()

    def _details_clicked(self):
        iso = self._selected_iso()
        if not iso:
            return

        if self._hash_thread and self._hash_thread.is_alive():
            messagebox.showinfo("Busy", "Already computing details for a file.")
            return

        self._stop_hash.clear()
        self._set_progress(0.0)
        self._set_status("Computing SHA-256...")
        self._set_actions_enabled(False)

        def worker():
            try:
                def prog(frac):
                    self._work_q.put(("hash_progress", frac, None))

                digest = sha256_file(iso.path, progress_cb=prog, stop_flag=self._stop_hash)

                info = {
                    "Name": iso.name,
                    "Path": str(iso.path),
                    "Size": human_bytes(iso.size_bytes),
                    "Modified": datetime.fromtimestamp(iso.mtime).isoformat(sep=" ", timespec="seconds"),
                    "SHA-256": digest,
                }
                self._work_q.put(("details_done", info, None))
            except Exception as e:
                self._work_q.put(("error", f"Details failed: {e}", None))

        self._hash_thread = threading.Thread(target=worker, daemon=True)
        self._hash_thread.start()

    def _local_meta_text(self, iso: IsoItem) -> str:
        key = str(iso.path)
        cached = self._local_meta_cache.get(key)
        if cached is not None:
            return cached

        name = (iso.name or "").lower()

        distro = ""
        distro_map = (
            ("ubuntu", "Ubuntu"),
            ("debian", "Debian"),
            ("fedora", "Fedora"),
            ("arch", "Arch"),
            ("archlinux", "Arch"),
            ("kali", "Kali"),
            ("alpine", "Alpine"),
            ("opensuse", "openSUSE"),
            ("suse", "openSUSE"),
            ("pop", "Pop!_OS"),
            ("pop!", "Pop!_OS"),
            ("manjaro", "Manjaro"),
            ("linuxmint", "Linux Mint"),
            ("mint", "Linux Mint"),
            ("elementary", "elementary OS"),
            ("zorin", "Zorin OS"),
            ("deepin", "Deepin"),
            ("mx", "MX Linux"),
            ("freebsd", "FreeBSD"),
            ("openbsd", "OpenBSD"),
            ("netbsd", "NetBSD"),
        )
        for needle, label in distro_map:
            if needle in name:
                distro = label
                break

        ver = infer_version_from_iso_name(iso.name) or ""

        arch = ""
        if re.search(r"\b(x86_64|amd64)\b", name):
            arch = "x86_64"
        elif re.search(r"\b(i386|i686|x86)\b", name):
            arch = "x86"
        elif re.search(r"\b(aarch64|arm64)\b", name):
            arch = "arm64"
        elif re.search(r"\barmv7\b", name):
            arch = "armv7"

        parts = [p for p in (distro, ver, arch) if p]
        out = " ".join(parts)
        self._local_meta_cache[key] = out
        return out

    def _find_duplicates_clicked(self) -> None:
        if not self._items:
            messagebox.showinfo("Duplicates", "Scan for local ISOs first.")
            return

        if self._hash_thread and self._hash_thread.is_alive():
            messagebox.showinfo("Busy", "Already computing SHA-256 for a file.")
            return

        self._stop_hash.clear()
        self._set_progress(0.0)
        self._set_status("Computing SHA-256 for duplicates...")
        self._set_actions_enabled(False)

        def worker(items: list[IsoItem]):
            try:
                groups: dict[str, list[str]] = {}
                total = max(1, len(items))
                for i, it in enumerate(items):
                    if self._stop_hash.is_set():
                        raise RuntimeError("Hashing cancelled")

                    def prog(frac: float, idx=i, tot=total):
                        overall = (float(idx) + float(frac)) / float(tot)
                        self._work_q.put(("dupes_progress", overall, None))

                    digest = sha256_file(it.path, progress_cb=prog, stop_flag=self._stop_hash)
                    groups.setdefault(digest, []).append(str(it.path))
                    self._work_q.put(("dupes_status", f"Hashed {i + 1}/{len(items)}", None))

                dupes = {h: ps for h, ps in groups.items() if len(ps) > 1}
                self._work_q.put(("dupes_done", dupes, None))
            except Exception as e:
                self._work_q.put(("error", f"Duplicate scan failed: {e}", None))

        self._hash_thread = threading.Thread(target=worker, args=(list(self._items),), daemon=True)
        self._hash_thread.start()

    def _extract_clicked(self):
        iso = self._selected_iso()
        if not iso:
            return

        seven = which_7z()
        if not seven:
            messagebox.showerror(
                "7-Zip not found",
                f"Extract requires 7-Zip in PATH ({seven_zip_hint()}).\n\nInstall 7-Zip, then restart the app.",
            )
            return

        out_dir = filedialog.askdirectory(title="Select output folder")
        if not out_dir:
            return

        out_dir_p = Path(out_dir)
        self._set_status("Extracting...")
        self._set_progress(None)
        self._set_actions_enabled(False)

        def worker():
            try:
                # 7z x image.iso -oC:\out -y
                cp = subprocess.run(
                    [seven, "x", str(iso.path), f"-o{str(out_dir_p)}", "-y"],
                    capture_output=True,
                    text=True,
                )
                if cp.returncode != 0:
                    err = (cp.stderr or cp.stdout or "Extraction failed").strip()
                    raise RuntimeError(err)
                self._work_q.put(("extract_done", str(out_dir_p), None))
            except Exception as e:
                self._work_q.put(("error", f"Extract failed: {e}", None))

        threading.Thread(target=worker, daemon=True).start()

    def _poll_queue(self):
        try:
            while True:
                kind, payload, extra = self._work_q.get_nowait()
                if kind == "scan_status":
                    self._set_status(payload)
                elif kind == "remote_items":
                    source, items = payload
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 0
                    self._remote_items = items
                    # Reset to current page size.
                    self._remote_display_limit = getattr(self, "_remote_display_limit_step", 200)
                    self._apply_remote_filter()
                    self._net_progress["value"] = 100
                    self._set_remote_actions_enabled(bool(self._selected_remotes()))
                elif kind == "remote_items_more":
                    more = payload
                    if not isinstance(more, list):
                        more = []
                    seen = {it.url for it in getattr(self, "_remote_items", [])}
                    added = 0
                    for it in more:
                        try:
                            u = it.url
                        except Exception:
                            continue
                        if u in seen:
                            continue
                        seen.add(u)
                        self._remote_items.append(it)
                        added += 1
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 100
                    self._apply_remote_filter()
                    if added == 0:
                        self._set_net_status(f"No older versions found ({len(self._remote_items)} ISO(s) total)")
                    else:
                        self._set_net_status(f"Added {added} older ISO(s), {len(self._remote_items)} total")
                elif kind == "scan_done":
                    cancelled = bool(extra)
                    found, elapsed = payload if payload else ([], 0.0)
                    self._set_scan_running(False)
                    self._progress.stop()
                    self._progress.configure(mode="determinate")
                    self._progress["value"] = 0
                    if cancelled:
                        self._items = []
                        self._tree.delete(*self._tree.get_children())
                        self._set_status("Scan cancelled")
                    else:
                        self._items = found
                        self._apply_filter()
                        self._set_status(f"Found {len(found)} ISO(s) in {elapsed:.1f}s")
                elif kind == "hash_progress":
                    self._set_progress(float(payload))
                elif kind == "dupes_progress":
                    try:
                        self._set_progress(float(payload))
                    except Exception:
                        pass
                elif kind == "dupes_status":
                    try:
                        self._set_status(str(payload))
                    except Exception:
                        pass
                elif kind == "dupes_done":
                    self._set_progress(0.0)
                    self._set_actions_enabled(self._selected_iso() is not None)
                    dupes = payload if isinstance(payload, dict) else {}
                    if not dupes:
                        self._set_status("No duplicates found")
                        try:
                            self._log("Duplicate scan: 0 groups")
                        except Exception:
                            pass
                        messagebox.showinfo("Duplicates", "No duplicate ISOs found.")
                    else:
                        lines: list[str] = []
                        for h, paths in dupes.items():
                            lines.append(f"SHA-256: {h}")
                            for p in paths:
                                lines.append(f"  - {p}")
                            lines.append("")
                        msg = "\n".join(lines).strip()
                        self._set_status(f"Found {len(dupes)} duplicate group(s)")
                        try:
                            self._log(f"Duplicate scan: {len(dupes)} groups")
                        except Exception:
                            pass
                        messagebox.showinfo("Duplicates", msg)
                elif kind == "details_done":
                    self._set_progress(0.0)
                    self._set_actions_enabled(self._selected_iso() is not None)
                    self._set_status("Ready")
                    info = payload
                    msg = "\n".join([f"{k}: {v}" for k, v in info.items()])
                    messagebox.showinfo("ISO Details", msg)
                elif kind == "mount_done":
                    self._progress.stop()
                    self._progress.configure(mode="determinate")
                    self._progress["value"] = 0
                    self._set_actions_enabled(self._selected_iso() is not None)

                    iso_path, point = payload
                    if point:
                        self._set_status(f"Mounted at {point}")
                        open_explorer(point)
                    else:
                        self._set_status("Mounted (mount point not found)")
                        messagebox.showinfo(
                            "Mounted",
                            "Mounted the ISO, but could not determine the mount point automatically.\n\n"
                            "Open it from your file manager.",
                        )
                elif kind == "eject_done":
                    self._set_actions_enabled(self._selected_iso() is not None)
                    iso_path, ok = payload
                    if ok:
                        self._set_status("ISO ejected")
                    else:
                        self._set_status("ISO was not mounted")
                elif kind == "extract_done":
                    self._progress.stop()
                    self._progress.configure(mode="determinate")
                    self._progress["value"] = 0
                    self._set_actions_enabled(self._selected_iso() is not None)
                    out_dir = payload
                    self._set_status("Extraction complete")
                    open_explorer(out_dir)
                elif kind == "download_progress":
                    self._set_net_progress(float(payload))
                elif kind == "download_batch_status":
                    self._set_net_status(str(payload))
                elif kind == "download_job_added":
                    try:
                        info = payload if isinstance(payload, dict) else {}
                        jid = str(info.get("job_id") or "")
                        if jid:
                            self._refresh_job_row(jid)
                            self._update_jobs_summary()
                    except Exception:
                        pass
                elif kind in ("download_job_progress", "download_job_update"):
                    try:
                        info = payload if isinstance(payload, dict) else {}
                        jid = str(info.get("job_id") or "")
                        if not jid:
                            continue
                        job = self._download_jobs.get(jid)
                        if job is not None:
                            status = info.get("status")
                            if isinstance(status, str) and status:
                                job["status"] = status
                            elif kind == "download_job_progress":
                                # A progress message posted just before the user
                                # hit Pause must not flip the job back to running.
                                paused = False
                                try:
                                    event = job.get("pause_event")
                                    paused = bool(event is not None and event.is_set())
                                except Exception:
                                    paused = False
                                if not paused and str(job.get("status")) != "Paused":
                                    job["status"] = "Downloading"
                                else:
                                    info = dict(info)
                                    info["speed"] = None
                                    info["eta"] = None
                            for key in ("progress", "done_bytes", "total_bytes", "speed", "eta"):
                                if key in info:
                                    job[key] = info.get(key)
                            note = info.get("note")
                            if note:
                                job["note"] = str(note)
                        self._refresh_job_row(jid)
                        self._update_jobs_summary()
                        self._save_download_jobs(force=(kind == "download_job_update"))
                    except Exception:
                        pass
                elif kind == "download_batch_progress":
                    try:
                        idx, tot, frac = payload
                        overall = (float(idx) + float(frac)) / max(1.0, float(tot))
                        self._set_net_progress(overall)
                    except Exception:
                        pass
                elif kind == "download_batch_done":
                    ok, failed = payload
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 0
                    self._set_remote_actions_enabled(bool(self._selected_remotes()))

                    if failed:
                        self._set_net_status(f"Downloaded {len(ok)} file(s), {len(failed)} failed")
                        preview = failed[:20]
                        lines = [f"Downloaded: {len(ok)}", f"Failed: {len(failed)}", "", "Failed items:"]
                        for name, err in preview:
                            lines.append(f"{name}\n  {err}")
                        if len(failed) > len(preview):
                            lines.append("")
                            lines.append(f"...and {len(failed) - len(preview)} more")
                        messagebox.showwarning("Batch download finished", "\n".join(lines))
                    else:
                        self._set_net_status(f"Downloaded {len(ok)} file(s)")
                        messagebox.showinfo("Batch download finished", f"Downloaded {len(ok)} file(s).")
                elif kind == "download_done":
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 0
                    self._set_remote_actions_enabled(bool(self._selected_remotes()))
                    path, verified = payload, extra
                    self._set_net_status("Download complete" + (" (verified)" if verified else ""))
                    messagebox.showinfo("Download complete", path)
                    try:
                        open_path_default(Path(path).parent)
                    except Exception:
                        pass
                elif kind == "validate_progress":
                    self._set_net_progress(float(payload))
                elif kind == "validate_done":
                    bad = payload
                    total = int(extra) if extra is not None else 0
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 0
                    self._set_remote_actions_enabled(bool(self._selected_remotes()))
                    if not bad:
                        self._set_net_status(f"Validation OK: {total} link(s) checked")
                        messagebox.showinfo("Validate", f"All links look OK ({total} checked).")
                    else:
                        self._set_net_status(f"Validation done: {len(bad)} broken link(s)")
                        preview = bad[:20]
                        lines = [f"{len(bad)} broken link(s) out of {total} checked:", ""]
                        for url, err in preview:
                            lines.append(f"{url}\n  {err}")
                        if len(bad) > len(preview):
                            lines.append("")
                            lines.append(f"...and {len(bad) - len(preview)} more")
                        messagebox.showwarning("Broken links", "\n".join(lines))
                elif kind == "audit_progress":
                    self._set_net_progress(float(payload))
                elif kind == "audit_done":
                    results = payload
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 0
                    self._set_remote_actions_enabled(bool(self._selected_remotes()))

                    bad = [(s, info) for (s, ok, info) in results if not ok]
                    ok_count = len(results) - len(bad)
                    if not bad:
                        self._set_net_status(f"Audit OK: {ok_count}/{len(results)} sources")
                        messagebox.showinfo("Audit", f"All sources look reachable ({len(results)} checked).")
                    else:
                        self._set_net_status(f"Audit done: {len(bad)} failing source(s)")
                        preview = bad[:25]
                        lines = [f"Failing sources: {len(bad)} out of {len(results)}", ""]
                        for s, info in preview:
                            lines.append(f"{s}\n  {info}")
                        if len(bad) > len(preview):
                            lines.append("")
                            lines.append(f"...and {len(bad) - len(preview)} more")
                        messagebox.showwarning("Audit results", "\n".join(lines))
                elif kind == "error":
                    self._progress.stop()
                    self._progress.configure(mode="determinate")
                    self._progress["value"] = 0
                    self._set_actions_enabled(self._selected_iso() is not None)
                    self._set_status("Error")
                    messagebox.showerror("Error", str(payload))
                elif kind == "error_net":
                    self._net_progress.stop()
                    self._net_progress.configure(mode="determinate")
                    self._net_progress["value"] = 0
                    self._set_remote_actions_enabled(self._selected_remote() is not None)
                    self._set_net_status("Error")
                    messagebox.showerror("Error", str(payload))
                else:
                    pass
        except queue.Empty:
            pass
        self.after(100, self._poll_queue)


def main() -> int:
    if "--webview" in sys.argv:
        if webview is None:
            return 2
        try:
            i = sys.argv.index("--webview")
            url = sys.argv[i + 1]
            title = "Browser"
            if "--title" in sys.argv:
                j = sys.argv.index("--title")
                if j + 1 < len(sys.argv):
                    title = sys.argv[j + 1]

            ipc_path = None
            if "--ipc" in sys.argv:
                k = sys.argv.index("--ipc")
                if k + 1 < len(sys.argv):
                    ipc_path = sys.argv[k + 1]

            try:
                webview.settings["ALLOW_DOWNLOADS"] = True
            except Exception:
                pass

            window = webview.create_window(title, url)
            seen = threading.Event()

            def on_response(resp):
                if seen.is_set():
                    return
                try:
                    u = getattr(resp, "url", None)
                except Exception:
                    u = None
                if not u or not isinstance(u, str):
                    return
                if is_plausible_iso_download_url(u):
                    seen.set()
                    if ipc_path:
                        try:
                            with open(ipc_path, "w", encoding="utf-8") as f:
                                f.write(u)
                        except Exception:
                            pass
                    try:
                        window.destroy()
                    except Exception:
                        pass

            try:
                window.events.response_received += on_response
            except Exception:
                pass
            webview.start(gui=None, debug=False)
            return 0
        except Exception:
            return 1

    try:
        app = App()
        app.mainloop()
        return 0
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
