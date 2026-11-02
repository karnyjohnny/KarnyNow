# gui.py — the dark-mode PyQt5 launcher.
#
# Tab 1 "Login": the RFC 8628 device grant — a copyable verification URL
# (PIN embedded when the endpoint supports it), the user code in big
# type, live poll status; on success the vault is sealed (DPAPI) and the
# token verified against v2/serverInfo (which doubles as the vpcId
# discovery). Tab 2 "Games & launch": the game list, the smart search,
# the store-variant picker, the knvstd parameter block, the LOG FOLDER
# (auto-suggested, operator numbers from 100) and the launch button
# (QProcess; stdout mirrored into the console pane AND the draw's own
# console log).
#
# Round N slice 4q -- the responsiveness + provenance rewrite:
#   * the list paints INSTANTLY from the disk cache (store.load_catalog)
#     before any socket is opened;
#   * the default refresh pulls ONLY the account's owned library (one
#     query, <1 s) instead of walking all ~6200 titles;
#   * "full catalog" is an opt-in checkbox that STREAMS the catalog page
#     by page, repainting as it grows (the selection survives re-renders);
#   * the search box asks NVIDIA's servers (`searchQuery`) after a
#     debounce, so any title is findable with nothing cached locally;
#   * a title offered by several stores (Steam/Epic/Gaijin) gets a
#     variant picker, pre-set to the one the web app would launch;
#   * the log FILE became a log FOLDER: logs/runNNN_logs/<draw>/ holds
#     knvstd's own logs (--log-dir), the wire dump (--wire-dump), the
#     console copy and a manifest written BEFORE the flight starts.
#
# The token is injected into argv by the GUI — never typed, never
# written to the log file or the console (argv is displayed redacted).
#
# Python 3.8 target (the Win7 rig — tests/requirements.txt): the
# future import makes annotations lazy so PEP 604/585 spellings never
# evaluate at import time (F37, round N slice 4o — their absence was
# the "failed gui" TypeError on the rig).

from __future__ import annotations

import os
import re
import sys
import time
from pathlib import Path

from PyQt5.QtCore import (QProcess, Qt, QThread, QTimer, QUrl, pyqtSignal)
from PyQt5.QtGui import QDesktopServices, QFont
from PyQt5.QtWidgets import (
    QAbstractItemView, QApplication, QCheckBox, QComboBox, QHBoxLayout,
    QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow,
    QMessageBox, QPlainTextEdit, QPushButton, QTabWidget, QVBoxLayout,
    QWidget,
)

from . import auth, catalog, search, store
from .params import (DEFAULT_PARAMS, inject_draw_flags, redact_argv,
                     split_params)

REPO_ROOT = Path(__file__).resolve().parents[2]
LOGS_DIR = REPO_ROOT / "logs"

# Slice 4q: server-side search tuning. The debounce keeps one keystroke
# from costing one HTTPS round trip; two characters is the shortest
# query worth asking NVIDIA about ("cs" already disambiguates).
SEARCH_DEBOUNCE_MS = 350
SEARCH_MIN_LEN = 2
LIST_CAP = 300

ACCENT = "#76b900"      # NVIDIA green
DARK_QSS = """
* { font-family: 'Segoe UI', 'Ubuntu', sans-serif; font-size: 13px; }
QMainWindow, QWidget { background: #1b1d20; color: #e6e6e6; }
QTabWidget::pane { border: 1px solid #2e3136; background: #1b1d20; }
QTabBar::tab { background: #22252a; color: #9aa0a6; padding: 8px 22px;
               border: 1px solid #2e3136; border-bottom: none;
               border-top-left-radius: 6px; border-top-right-radius: 6px; }
QTabBar::tab:selected { background: #1b1d20; color: #ffffff;
                        border-bottom: 2px solid %(accent)s; }
QLabel#big { font-size: 30px; font-weight: 700; color: %(accent)s;
             letter-spacing: 4px; }
QLabel#hdr { font-size: 15px; font-weight: 600; color: #ffffff; }
QLabel#dim { color: #9aa0a6; }
QLineEdit, QPlainTextEdit { background: #22252a; color: #e6e6e6;
    border: 1px solid #2e3136; border-radius: 6px; padding: 7px;
    selection-background-color: %(accent)s; selection-color: #101010; }
QLineEdit:focus, QPlainTextEdit:focus { border: 1px solid %(accent)s; }
QLineEdit:read-only { color: #b9c0c7; }
QPushButton { background: #2c2f35; color: #e6e6e6; border: 1px solid
    #3a3e45; border-radius: 6px; padding: 8px 18px; font-weight: 600; }
QPushButton:hover { background: #34383f; border-color: %(accent)s; }
QPushButton:pressed { background: #23262b; }
QPushButton:disabled { color: #5d6269; border-color: #2e3136; }
QPushButton#go { background: %(accent)s; color: #10140a;
                 border: none; font-size: 14px; }
QPushButton#go:hover { background: #84cf00; }
QPushButton#go:disabled { background: #3a4a20; color: #77806a; }
QPushButton#danger { color: #ff8f85; border-color: #5a3434; }
QListWidget { background: #202328; border: 1px solid #2e3136;
              border-radius: 6px; outline: 0; }
QListWidget::item { padding: 7px 10px; border-bottom: 1px solid #26292e; }
QListWidget::item:selected { background: #2e3a1e; color: #ffffff;
                             border-left: 3px solid %(accent)s; }
QCheckBox { spacing: 8px; color: #b9c0c7; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px;
                       border: 1px solid #3a3e45; background: #22252a; }
QCheckBox::indicator:checked { background: %(accent)s;
                               border-color: %(accent)s; }
QScrollBar:vertical { background: #1b1d20; width: 10px; }
QScrollBar::handle:vertical { background: #3a3e45; border-radius: 5px;
                              min-height: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
""" % {"accent": ACCENT}


# --------------------------------------------------------------- workers
class VerifyWorker(QThread):
    """Vault load -> refresh-if-needed -> verify_and_vpc."""
    done = pyqtSignal(bool, str, object)   # ok, vpc_or_reason, bundle|None

    def run(self):
        bundle = store.load()
        if not bundle:
            self.done.emit(False, "no saved credential", None)
            return
        try:
            bundle, changed = auth.ensure_fresh(bundle)
            if changed:
                store.save(bundle)
            ok, info = auth.verify_and_vpc(bundle)
            if ok:
                self.done.emit(True, info, bundle)
            elif info.startswith("token rejected") and \
                    bundle.get("refresh_token"):
                fresh, changed = auth.ensure_fresh(bundle)   # forced retry
                if changed:
                    store.save(fresh)
                    ok, info = auth.verify_and_vpc(fresh)
                    self.done.emit(ok, info, fresh if ok else None)
                else:
                    self.done.emit(False, info, None)
            else:
                self.done.emit(False, info, None)
        except Exception as exc:                # never kill the GUI
            self.done.emit(False, "verify crashed: %s" % exc, None)


class DeviceLoginWorker(QThread):
    """The whole device grant: authorize -> show URL/code -> poll."""
    challenge = pyqtSignal(str, str, int)      # url, user_code, interval
    status = pyqtSignal(str)
    ok = pyqtSignal(object)                    # the vault bundle
    fail = pyqtSignal(str)

    def __init__(self, deadline_s=900):
        super().__init__()
        self._stop = False
        self.deadline_s = deadline_s

    def stop(self):
        self._stop = True

    def run(self):
        try:
            client = auth._client(timeout=20.0)
            try:
                self.status.emit("requesting the device code…")
                resp = auth.device_authorize(client=client)
                device_code = resp.get("device_code") or \
                    resp.get("deviceCode") or ""
                if not device_code:
                    keys = ",".join(sorted(resp.keys()))[:200]
                    self.fail.emit("no device_code in the response "
                                   "(keys: %s)" % keys)
                    return
                url, code = auth.verification_urls(resp)
                interval = int(resp.get("interval") or 5)
                self.challenge.emit(url or "(no verification URI "
                                    "returned)", code, interval)
                t_end = time.time() + self.deadline_s
                while time.time() < t_end and not self._stop:
                    time.sleep(max(1, min(interval, 30)))
                    if self._stop:
                        return
                    res = auth.poll_token(device_code, client=client)
                    st = res.get("state")
                    if st == "ok":
                        bundle = auth.make_bundle(res["tokens"])
                        self.ok.emit(bundle)
                        return
                    if st == auth.SLOW_DOWN:
                        interval = min(interval + 5, 3600)
                        self.status.emit("slow_down — polling every %d s"
                                         % interval)
                    elif st == auth.PENDING:
                        self.status.emit("waiting for the browser login… "
                                         "(poll every %d s)" % interval)
                    elif st == auth.EXPIRED:
                        self.fail.emit("the device code expired — start "
                                       "again")
                        return
                    elif st == auth.DENIED:
                        self.fail.emit("login denied in the browser")
                        return
                    else:
                        self.fail.emit("token poll: %s" %
                                       res.get("error", st))
                        return
                if not self._stop:
                    self.fail.emit("timed out waiting for the login")
            finally:
                client.close()
        except Exception as exc:
            self.fail.emit("device login crashed: %s" % exc)


class CatalogWorker(QThread):
    """Slice 4q — two modes, cheapest first.

    full=False (the DEFAULT): the account's OWNED library only — one
    filtered query, a handful of pages, <1 s. This is the operator's
    "może ograniczenie zapytania realnie tylko do tych, które ??mam??".
    full=True (the "full catalog" checkbox): the whole ~6200-title
    catalog, but STREAMED — `page` fires after every 100 apps, so the
    list paints immediately and keeps growing while the rest downloads
    instead of appearing a minute later.
    """
    progress = pyqtSignal(int, object)         # count, total|None|str
    page = pyqtSignal(list, object)            # this page, total|None
    done = pyqtSignal(list, str)               # every record, kind
    fail = pyqtSignal(str)

    def __init__(self, token, vpc_id, full=False):
        super().__init__()
        self.token, self.vpc_id, self.full = token, vpc_id, bool(full)

    def run(self):
        try:
            def cb(n, total):
                self.progress.emit(n, total)
            if not self.full:
                games = catalog.fetch_library(self.token, self.vpc_id,
                                              progress=cb)
                self.done.emit(games, "library")
                return
            by_id, games = {}, []
            for records, total in catalog.iter_browse(self.token,
                                                      self.vpc_id):
                fresh = []
                for g in records:
                    prev = by_id.get(g["id"])
                    if prev is None:
                        by_id[g["id"]] = g
                        games.append(g)
                        fresh.append(g)
                    else:
                        catalog.merge_records(prev, g)
                        fresh.append(prev)
                cb(len(games), total)
                # copies: the worker keeps mutating its own records, and
                # a queued signal must not hand the GUI a moving target
                self.page.emit([dict(x) for x in fresh], total)
            catalog.sort_games(games)
            self.done.emit([dict(x) for x in games], "full")
        except Exception as exc:
            self.fail.emit(str(exc))


class SearchWorker(QThread):
    """Slice 4q — the search box asks NVIDIA's servers directly
    (`searchQuery`, the same field the web app and OpenNOW use), so any
    of the ~6200 catalog titles is findable in ONE round trip even
    while the local list holds only the owned library. Debounced by the
    GUI; a stale reply is discarded by query text."""
    done = pyqtSignal(list, str)               # records, the query text
    fail = pyqtSignal(str, str)                # reason, the query text

    def __init__(self, token, vpc_id, text):
        super().__init__()
        self.token, self.vpc_id, self.text = token, vpc_id, text

    def run(self):
        try:
            hits = catalog.search_server(self.token, self.vpc_id, self.text)
            self.done.emit(hits, self.text)
        except Exception as exc:
            self.fail.emit(str(exc), self.text)


# ---------------------------------------------------------------- window
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("KarnyNow launcher")
        self.resize(1080, 760)
        self.bundle = None
        self.vpc_id = ""
        self.games = []
        self.games_kind = ""            # "" | "cache" | "library" | "full"
        self.games_total = None         # pageInfo.totalCount while paging
        self.cache_meta = {}            # the on-disk catalog envelope
        self.search_hits = []           # server-side searchQuery results
        self.search_query = ""          # the query those hits belong to
        self.login_worker = None
        self.verify_worker = None
        self.catalog_worker = None
        self.search_worker = None
        self.proc = None
        self.log_fh = None
        self.draw_dir = ""              # logs/<run>/<N> of the live flight
        self.draw_index = 0
        self._last_selected_id = ""     # re-render guard (_on_game_selected)

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)
        self.tabs.addTab(self._build_login_tab(), "1 · Login")
        self.tabs.addTab(self._build_games_tab(), "2 · Games && launch")
        self.tabs.setTabEnabled(1, False)

        warn = store.plaintext_warning()
        if warn:
            self.login_status.setText("⚠ " + warn)
        # Slice 4q: paint the list from the DISK CACHE before anything
        # touches the network — startup shows games instantly and the
        # library refresh corrects them a second later.
        self._load_cache()
        self.verify_worker = VerifyWorker()
        self.verify_worker.done.connect(self._on_verified)
        self.verify_worker.start()

    # ------------------------------------------------------------ login
    def _build_login_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setSpacing(14)
        lay.setContentsMargins(28, 24, 28, 24)

        hdr = QLabel("GeForce NOW — device login (OAuth 2.0, RFC 8628)")
        hdr.setObjectName("hdr")
        lay.addWidget(hdr)

        self.login_status = QLabel("checking the saved credential…")
        self.login_status.setObjectName("dim")
        self.login_status.setWordWrap(True)
        lay.addWidget(self.login_status)

        self.url_field = QLineEdit()
        self.url_field.setReadOnly(True)
        self.url_field.setPlaceholderText(
            "the verification URL appears here once you start the login")
        lay.addWidget(self.url_field)

        row = QHBoxLayout()
        self.copy_btn = QPushButton("Copy URL")
        self.copy_btn.clicked.connect(self._copy_url)
        self.copy_btn.setEnabled(False)
        self.open_btn = QPushButton("Open in browser")
        self.open_btn.clicked.connect(self._open_url)
        self.open_btn.setEnabled(False)
        self.start_btn = QPushButton("Start device login")
        self.start_btn.clicked.connect(self._start_login)
        row.addWidget(self.copy_btn)
        row.addWidget(self.open_btn)
        row.addStretch(1)
        row.addWidget(self.start_btn)
        lay.addLayout(row)

        self.code_label = QLabel("")
        self.code_label.setObjectName("big")
        self.code_label.setAlignment(Qt.AlignCenter)
        self.code_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        lay.addWidget(self.code_label)

        hint = QLabel("If the page asks for a PIN, type the code above. "
                      "The GUI polls automatically — it unlocks tab 2 "
                      "the moment the login lands.")
        hint.setObjectName("dim")
        hint.setWordWrap(True)
        lay.addWidget(hint)

        self.logout_btn = QPushButton("Erase the saved credential")
        self.logout_btn.setObjectName("danger")
        self.logout_btn.clicked.connect(self._logout)
        self.logout_btn.setEnabled(False)
        lay.addStretch(1)
        lay.addWidget(self.logout_btn)
        return w

    def _copy_url(self):
        QApplication.clipboard().setText(self.url_field.text())
        self.login_status.setText("URL copied to the clipboard.")

    def _open_url(self):
        url = self.url_field.text()
        if url.startswith("http"):
            QDesktopServices.openUrl(QUrl(url))

    def _start_login(self):
        if self.login_worker and self.login_worker.isRunning():
            return
        self.start_btn.setEnabled(False)
        self.code_label.setText("")
        self.login_status.setText("starting the device grant…")
        self.login_worker = DeviceLoginWorker()
        self.login_worker.challenge.connect(self._on_challenge)
        self.login_worker.status.connect(self.login_status.setText)
        self.login_worker.ok.connect(self._on_login_ok)
        self.login_worker.fail.connect(self._on_login_fail)
        self.login_worker.start()

    def _on_challenge(self, url, code, interval):
        self.url_field.setText(url)
        self.copy_btn.setEnabled(True)
        self.open_btn.setEnabled(url.startswith("http"))
        self.code_label.setText(code or "")
        self.login_status.setText(
            "open the URL (Copy / Open), confirm the PIN if asked — "
            "polling every %d s" % interval)

    def _on_login_ok(self, bundle):
        self.bundle = bundle
        store.save(bundle)
        who = (bundle.get("user") or {}).get("email") or "the account"
        self.login_status.setText("logged in as %s — verifying…" % who)
        self.start_btn.setEnabled(True)
        self.logout_btn.setEnabled(True)
        self.verify_worker = VerifyWorker()
        self.verify_worker.done.connect(self._on_verified)
        self.verify_worker.start()

    def _on_login_fail(self, why):
        self.start_btn.setEnabled(True)
        self.login_status.setText("login failed: %s" % why)

    def _logout(self):
        store.wipe()
        self.bundle = None
        self.vpc_id = ""
        self.tabs.setTabEnabled(1, False)
        self.logout_btn.setEnabled(False)
        self.url_field.setText("")
        self.code_label.setText("")
        self.copy_btn.setEnabled(False)
        self.open_btn.setEnabled(False)
        self.login_status.setText("the vault was erased. Start a new "
                                  "device login when ready.")

    def _on_verified(self, ok, info, bundle):
        if ok and bundle:
            self.bundle = bundle
            self.vpc_id = info
            who = (bundle.get("user") or {}).get("email") or "account"
            exp = bundle.get("expires_at") or 0
            left = ("token valid ~%d min" % max(0, int((exp - time.time())
                                                        // 60))) if exp \
                else "token expiry unknown"
            self.login_status.setText(
                "✔ verified: %s · vpc %s · %s\nvault: %s" %
                (who, info, left, store.vault_path()))
            self.logout_btn.setEnabled(True)
            self.tabs.setTabEnabled(1, True)
            self.tabs.setTabEnabled(0, True)
            if self.tabs.currentIndex() != 1 and self.games_ready:
                self.tabs.setCurrentIndex(1)
            # Slice 4q: the disk cache may already have painted a list,
            # so `if not self.games` would skip the network forever. The
            # default refresh is now the OWNED LIBRARY (one query, <1 s),
            # so always run it — it corrects ownership and new titles.
            self._refresh_catalog()
        else:
            self.tabs.setTabEnabled(1, False)
            if not (self.login_worker and self.login_worker.isRunning()):
                self.start_btn.setEnabled(True)
                self.login_status.setText(
                    "no usable credential (%s). Start the device login — "
                    "or the vault is on another machine/user." % info)

    # ------------------------------------------------------------ games
    @property
    def games_ready(self):
        return bool(self.bundle)

    def _build_games_tab(self):
        w = QWidget()
        lay = QVBoxLayout(w)
        lay.setSpacing(10)
        lay.setContentsMargins(18, 14, 18, 14)

        # Slice 4q: the search box is debounced and then asks NVIDIA's
        # servers (`searchQuery`), so it reaches all ~6200 titles even
        # when the local list holds only the owned library.
        self.search_timer = QTimer(self)
        self.search_timer.setSingleShot(True)
        self.search_timer.setInterval(SEARCH_DEBOUNCE_MS)
        self.search_timer.timeout.connect(self._run_server_search)

        top = QHBoxLayout()
        self.search_box = QLineEdit()
        self.search_box.setPlaceholderText(
            "smart search — “endfield”, “ark end”, “aknd”…  (server-side: "
            "finds any of the ~6200 titles without downloading them)")
        self.search_box.textChanged.connect(self._on_search_changed)
        top.addWidget(self.search_box, 1)
        self.owned_chk = QCheckBox("owned only")
        self.owned_chk.stateChanged.connect(self._render_games)
        top.addWidget(self.owned_chk)
        # The full-catalog walk is now OPT-IN: unticked, a refresh pulls
        # only the account's library (one query, <1 s); ticked, the whole
        # catalog streams in page by page and paints as it arrives.
        self.full_chk = QCheckBox("full catalog")
        self.full_chk.setToolTip(
            "Download every GFN title (~6200) in the background, page by "
            "page. Unticked, only YOUR library is fetched — the search "
            "box still reaches the whole catalog through the server.")
        self.full_chk.stateChanged.connect(self._on_full_toggled)
        top.addWidget(self.full_chk)
        self.refresh_btn = QPushButton("Refresh")
        self.refresh_btn.clicked.connect(self._refresh_catalog)
        top.addWidget(self.refresh_btn)
        lay.addLayout(top)

        self.catalog_status = QLabel("catalog not loaded yet")
        self.catalog_status.setObjectName("dim")
        self.catalog_status.setWordWrap(True)
        lay.addWidget(self.catalog_status)

        body = QHBoxLayout()
        self.game_list = QListWidget()
        self.game_list.setSelectionMode(QAbstractItemView.SingleSelection)
        self.game_list.itemSelectionChanged.connect(self._on_game_selected)
        body.addWidget(self.game_list, 3)

        right = QVBoxLayout()
        right.setSpacing(10)
        ghdr = QLabel("knvstd parameters")
        ghdr.setObjectName("hdr")
        right.addWidget(ghdr)
        self.params = QPlainTextEdit(DEFAULT_PARAMS)
        self.params.setFixedHeight(110)
        right.addWidget(self.params)
        note = QLabel("--session, --session-token, --session-app, "
                      "--session-device, --log-dir and --wire-dump are "
                      "added automatically (the token never passes "
                      "through this box or a log).")
        note.setObjectName("dim")
        note.setWordWrap(True)
        right.addWidget(note)

        # Slice 4q: one GFN title can be offered by several stores
        # (War Thunder = GAIJIN 10839111 + STEAM 100234911), each a
        # DIFFERENT numeric appId. The picker chooses which one flies;
        # the pre-selection is the variant the web app would pick.
        vrow = QHBoxLayout()
        self.variant_lbl = QLabel("store variant:")
        vrow.addWidget(self.variant_lbl)
        self.variant_box = QComboBox()
        self.variant_box.currentIndexChanged.connect(self._on_variant_changed)
        vrow.addWidget(self.variant_box, 1)
        right.addLayout(vrow)
        self.variant_hint = QLabel("")
        self.variant_hint.setObjectName("dim")
        self.variant_hint.setWordWrap(True)
        right.addWidget(self.variant_hint)
        self._set_variant_visible(False)

        # Slice 4q: log FILE -> log FOLDER. One directory per run, one
        # numbered subdirectory per draw, and everything for that draw
        # lands inside it (knvstd's logs, the wire dump, the console
        # copy, the manifest). No more hand-made run103_logs folders and
        # no more draws without logs (run103/3).
        lrow = QHBoxLayout()
        lrow.addWidget(QLabel("log folder:"))
        self.logfolder = QLineEdit(self._suggest_run_name())
        self.logfolder.setFixedWidth(150)
        self.logfolder.textChanged.connect(self._update_draw_hint)
        lrow.addWidget(self.logfolder)
        right.addLayout(lrow)
        self.draw_hint = QLabel("")
        self.draw_hint.setObjectName("dim")
        self.draw_hint.setWordWrap(True)
        right.addWidget(self.draw_hint)

        self.launch_btn = QPushButton("▶  Launch knvstd")
        self.launch_btn.setObjectName("go")
        self.launch_btn.clicked.connect(self._launch)
        right.addWidget(self.launch_btn)
        self.stop_btn = QPushButton("Stop")
        self.stop_btn.setEnabled(False)
        self.stop_btn.clicked.connect(self._stop_proc)
        right.addWidget(self.stop_btn)

        self.console = QPlainTextEdit()
        self.console.setReadOnly(True)
        self.console.setMaximumBlockCount(20000)
        f = QFont("Consolas")
        f.setStyleHint(QFont.Monospace)
        self.console.setFont(f)
        right.addWidget(self.console, 1)
        body.addLayout(right, 2)
        lay.addLayout(body, 1)
        self._update_draw_hint()
        return w

    # ------------------------------------------------------ run folders
    def _suggest_run_name(self) -> str:
        """runNNN_logs — the operator numbers from 100; suggest max+1.
        Legacy runNNN.log files count too, so a hand-numbered 103 offers
        run104_logs."""
        return store.suggest_run_name(LOGS_DIR)

    def _run_name(self) -> str:
        return (self.logfolder.text() or "").strip()

    def _update_draw_hint(self, *_a):
        """Slice 4q: show the operator EXACTLY where the next launch
        writes, and refuse a name that could not be a directory."""
        name = self._run_name()
        busy = self.proc is not None and \
            self.proc.state() != QProcess.NotRunning
        if not store.valid_run_name(name):
            self.draw_hint.setText("⚠ not a run name — use runNNN_logs "
                                   "(e.g. run104_logs)")
            self.launch_btn.setEnabled(False)
            return
        self.launch_btn.setEnabled(not busy)
        have = store.draw_dirs(LOGS_DIR, name)
        nxt = store.next_draw_index(LOGS_DIR, name)
        self.draw_hint.setText(
            "→ logs/%s/%d    (draws so far: %s)" %
            (name, nxt, ", ".join(str(d) for d in have) if have else "none"))

    # ---------------------------------------------------- cache + fetch
    def _load_cache(self):
        """Paint the list from disk BEFORE any network (slice 4q): the
        tab opens with games already there, and the library refresh a
        second later corrects ownership and new titles."""
        self.cache_meta = store.load_catalog()
        games = self.cache_meta.get("games") or []
        if not games:
            return
        self.games = [dict(g) for g in games]
        self.games_kind = "cache"
        self._render_games()
        self.catalog_status.setText(
            "%s — will refresh after login" %
            store.cache_summary(self.cache_meta))

    def _on_full_toggled(self, *_a):
        if self.bundle and self.vpc_id:
            self._refresh_catalog()

    def _refresh_catalog(self):
        if not self.bundle or not self.vpc_id:
            return
        if self.catalog_worker and self.catalog_worker.isRunning():
            return
        full = bool(self.full_chk.isChecked())
        self.refresh_btn.setEnabled(False)
        self.catalog_status.setText(
            "fetching %s…" % ("the FULL catalog (streaming, page by page)"
                              if full else "YOUR library only"))
        self.catalog_worker = CatalogWorker(
            auth.service_token(self.bundle), self.vpc_id, full=full)
        self.catalog_worker.progress.connect(self._catalog_progress)
        self.catalog_worker.page.connect(self._catalog_page)
        self.catalog_worker.done.connect(self._catalog_done)
        self.catalog_worker.fail.connect(self._catalog_fail)
        self.catalog_worker.start()

    def _catalog_progress(self, n, total):
        if isinstance(total, str):
            self.catalog_status.setText(total)
        elif n and n < 0:
            self.catalog_status.setText(str(total))
        else:
            self.games_total = total
            self.catalog_status.setText("fetched %d apps%s…" %
                                        (n, (" / %s" % total) if total
                                         else ""))

    def _catalog_page(self, records, total):
        """Slice 4q: paint every page the moment it lands, so a full
        catalog watch grows instead of spinning."""
        if total:
            self.games_total = total
        self._merge_games(records)
        self._render_games()
        self.catalog_status.setText(
            "%d games%s — still downloading…" %
            (len(self.games), (" / %s" % total) if total else ""))

    def _merge_games(self, records):
        """Union `records` into self.games by UUID (library + browse +
        server-search results all describe the same titles with
        different field coverage, so they MERGE rather than replace)."""
        by_id = {}
        for g in self.games:
            by_id.setdefault(g.get("id") or "", g)
        for g in records or []:
            gid = g.get("id") or ""
            if gid and gid in by_id:
                catalog.merge_records(by_id[gid], g)
            else:
                rec = dict(g)
                if gid:
                    by_id[gid] = rec
                self.games.append(rec)

    def _catalog_done(self, games, kind=""):
        if games:
            self.games = [dict(g) for g in games]
        self.games_kind = kind or self.games_kind
        self.refresh_btn.setEnabled(True)
        owned = sum(1 for g in self.games if g.get("owned"))
        store.save_catalog(self.games, self.vpc_id, self.games_kind)
        self.cache_meta = store.load_catalog()
        self.catalog_status.setText(
            "%d games (%s) · %d owned by this account%s" %
            (len(self.games), self.games_kind or "?", owned,
             "" if self.full_chk.isChecked()
             else " · tick “full catalog” for all ~6200"))
        self._render_games()

    def _catalog_fail(self, why):
        self.refresh_btn.setEnabled(True)
        keep = (" (keeping the %d %s games already listed)" %
                (len(self.games), self.games_kind or "cached")
                if self.games else "")
        self.catalog_status.setText("catalog failed: %s%s" % (why, keep))

    # ------------------------------------------------- server-side search
    def _on_search_changed(self, text):
        """Local smart filter fires instantly (zero latency, works
        offline on the cache); the server query is debounced behind it
        so typing costs one round trip, not one per keystroke."""
        q = (text or "").strip()
        if q != self.search_query:
            self.search_hits = []          # hits of a previous query
            self.search_query = ""
        self._render_games()
        if len(q) >= SEARCH_MIN_LEN and self.bundle and self.vpc_id:
            self.search_timer.start()
        else:
            self.search_timer.stop()

    def _run_server_search(self):
        q = self.search_box.text().strip()
        if len(q) < SEARCH_MIN_LEN or not self.bundle or not self.vpc_id:
            return
        if self.search_worker and self.search_worker.isRunning():
            self.search_timer.start()      # busy — ask again shortly
            return
        self.search_worker = SearchWorker(
            auth.service_token(self.bundle), self.vpc_id, q)
        self.search_worker.done.connect(self._search_done)
        self.search_worker.fail.connect(self._search_fail)
        self.search_worker.start()

    def _search_done(self, hits, text):
        if text != self.search_box.text().strip():
            return                         # the operator typed on: stale
        self.search_hits = [dict(h) for h in (hits or [])]
        self.search_query = text
        self._merge_games(self.search_hits)   # enrich ownership/variants
        self._render_games()

    def _search_fail(self, why, text):
        if text != self.search_box.text().strip():
            return
        self.search_hits = []
        self.search_query = ""
        self._render_games()
        self.catalog_status.setText(
            "server search failed (%s) — local matches only" % why)

    # ------------------------------------------------------------ render
    def _selected_id(self) -> str:
        items = self.game_list.selectedItems()
        if not items:
            return ""
        return ((items[0].data(Qt.UserRole) or {}).get("id")) or ""

    def _restore_selection(self, gid):
        """Keep the operator's pick across a re-render — a streaming full
        catalog re-renders dozens of times, and losing the selection
        would make the list unusable while it downloads."""
        if not gid:
            return
        for i in range(self.game_list.count()):
            g = self.game_list.item(i).data(Qt.UserRole) or {}
            if g.get("id") == gid:
                self.game_list.setCurrentRow(i)
                return

    def _make_item(self, g):
        badge = "★ OWNED  " if g.get("owned") else "        "
        tier = ("  [%s]" % g["tier"]) if g.get("tier") else ""
        picks = [v for v in (g.get("variants") or []) if v.get("app_id")]
        if len(picks) > 1:
            allv = g.get("variants") or []
            idx = g.get("variant_index") or 0
            store_name = (allv[idx].get("store")
                          if 0 <= idx < len(allv) else picks[0].get("store"))
            ids = "%d stores ▸ %s" % (len(picks), store_name or "?")
        else:
            ids = ",".join(str(v["app_id"]) for v in picks[:3]) or "—"
        item = QListWidgetItem("%s%s — %s%s   ⟦app %s⟧" %
                               (badge, g.get("title"),
                                g.get("developer") or "?", tier, ids))
        item.setData(Qt.UserRole, g)
        return item

    def _render_games(self):
        q = self.search_box.text()
        qs = q.strip()
        games, seen = [], set()

        def push(g):
            # A record with no UUID (a degraded/partial fetch, a test
            # fixture) is never deduped away — only real ids collide.
            gid = g.get("id") or ""
            if gid:
                if gid in seen:
                    return False
                seen.add(gid)
            if len(games) < LIST_CAP:
                games.append(g)
                return True
            return False

        # Server hits first: they are the authoritative answer to a query
        # the local (library-only) list may not contain at all.
        if qs and self.search_query == qs:
            for g in self.search_hits:
                push(g)
        for g in search.smart_filter(self.games, q, cap=LIST_CAP):
            if not push(g):
                break
        if self.owned_chk.isChecked():
            games = [g for g in games if g.get("owned")]
        keep = self._selected_id()
        self.game_list.clear()
        for g in games:
            self.game_list.addItem(self._make_item(g))
        self._restore_selection(keep)

    # --------------------------------------------------------- variants
    def _set_variant_visible(self, on):
        self.variant_lbl.setVisible(bool(on))
        self.variant_box.setVisible(bool(on))
        self.variant_hint.setVisible(bool(on))

    def _launchable_variants(self, g):
        return [v for v in ((g or {}).get("variants") or [])
                if v.get("app_id")]

    def _fill_variants(self, g):
        """The Steam/Epic picker. Pre-selected = the variant the GFN web
        app would launch (OpenNOW catalog.rs:854 — library.selected,
        else in-library, else the first)."""
        variants = self._launchable_variants(g)
        self.variant_box.blockSignals(True)
        self.variant_box.clear()
        for v in variants:
            tag = []
            if v.get("selected"):
                tag.append("web default ✓")
            if v.get("in_library"):
                tag.append("in library")
            elif v.get("status"):
                tag.append(v["status"])
            if not v.get("in_library") and v.get("gfn_status") == "AVAILABLE":
                tag.append("free-to-play")
            self.variant_box.addItem(
                "%s · %s%s" % (v.get("store") or "?", v["app_id"],
                               ("  (%s)" % ", ".join(tag)) if tag else ""),
                dict(v))
        self.variant_box.blockSignals(False)
        self._set_variant_visible(len(variants) > 1)
        allv = (g or {}).get("variants") or []
        idx = (g or {}).get("variant_index") or 0
        want = allv[idx].get("id") if 0 <= idx < len(allv) else None
        for i in range(self.variant_box.count()):
            if (self.variant_box.itemData(i) or {}).get("id") == want:
                self.variant_box.setCurrentIndex(i)
                break
        self._update_variant_hint()

    def _update_variant_hint(self, *_a):
        v = self._selected_variant()
        self.variant_hint.setText(
            "" if not v else "--session-app=%s   (%s)" %
            (v["app_id"], v.get("short_name") or v.get("store") or "?"))

    def _on_variant_changed(self, *_a):
        self._update_variant_hint()

    def _selected_game(self):
        items = self.game_list.selectedItems()
        return (items[0].data(Qt.UserRole) or {}) if items else {}

    def _selected_variant(self):
        g = self._selected_game()
        if self.variant_box.count():
            v = self.variant_box.currentData()
            if v:
                return v
        variants = self._launchable_variants(g)
        if not variants:
            return None
        allv = g.get("variants") or []
        idx = g.get("variant_index") or 0
        if 0 <= idx < len(allv) and allv[idx].get("app_id"):
            return allv[idx]
        return variants[0]

    def _on_game_selected(self):
        gid = self._selected_id()
        if gid == getattr(self, "_last_selected_id", ""):
            return            # a re-render, not a new choice by the operator
        self._last_selected_id = gid
        g = self._selected_game()
        if not g:
            return
        self.console.setPlainText("")
        self._fill_variants(g)
        self._print("# selected: %s (app id: %s, owned: %s)" %
                    (g.get("title"), self._selected_app_id(), g.get("owned")))

    def _selected_app_id(self):
        """The numeric appId that flies — the PICKED store variant, via
        OpenNOW's launch_id rule (selected -> in-library -> first
        numeric)."""
        v = self._selected_variant()
        if v and v.get("app_id"):
            return v["app_id"]
        g = self._selected_game()
        if not g:
            return None
        if g.get("default_app_id"):
            return g["default_app_id"]
        ids = g.get("app_ids") or []
        return ids[0] if ids else None

    # ----------------------------------------------------------- launch
    def _print(self, line):
        self.console.appendPlainText(line)
        if self.log_fh:
            try:
                self.log_fh.write(line + "\n")
                self.log_fh.flush()
            except OSError:
                pass

    def _console_log_name(self, run_name):
        """run104_logs -> run104.log (the console copy inside the draw
        folder; the spelling the operator already archives by)."""
        return re.sub(r"_logs$", "", run_name) + ".log"

    def _launch(self):
        if self.proc is not None and \
                self.proc.state() != QProcess.NotRunning:
            QMessageBox.warning(self, "knvstd", "a flight is already "
                                "running — stop it first")
            return
        if not self.bundle:
            QMessageBox.warning(self, "knvstd", "log in first (tab 1)")
            return
        exe = REPO_ROOT / "build" / \
            ("knvstd.exe" if os.name == "nt" else "knvstd")
        if not exe.is_file():
            QMessageBox.critical(self, "knvstd",
                                 "not found: %s\nbuild the streamer "
                                 "first (T10)." % exe)
            return
        app_id = self._selected_app_id()
        user_params = self.params.toPlainText().strip()
        try:
            extra = split_params(user_params)
        except ValueError as exc:
            QMessageBox.critical(self, "params", "cannot parse: %s" % exc)
            return
        if any(a.startswith("--session-token") for a in extra):
            QMessageBox.critical(self, "params",
                                 "--session-token must NOT be typed here — "
                                 "the GUI injects it. Remove it.")
            return
        if "--session" in extra:
            QMessageBox.critical(self, "params",
                                 "--session is added automatically — "
                                 "remove it from the parameters.")
            return
        argv = [str(exe), "--session",
                "--session-token=" + auth.service_token(self.bundle)]
        has_app = any(a.startswith("--session-app") for a in extra)
        if app_id and not has_app:
            argv.append("--session-app=%d" % app_id)
        elif not has_app:
            QMessageBox.warning(self, "params",
                                "no game selected and no --session-app in "
                                "the parameters — knvstd would refuse, so "
                                "nothing was launched")
            return
        # F38a (run-102 read): ONE device identity per install. auth.py
        # already sends store.device_id() as the login x-device-id, and
        # the reference reuses the SAME value for CloudMatch's
        # x-device-id/deviceHashId — knvstd's per-log-dir random made
        # every draw a fresh "device" (run 102 flew five in one
        # evening) and split the login/stream identity. An
        # operator-typed --session-device wins (no duplicate).
        device = store.device_id()
        if not any(a.startswith("--session-device") for a in extra):
            argv.append("--session-device=" + device)
        argv += extra

        # Slice 4q: log FILE -> log FOLDER. Every launch allocates the
        # next free draw directory inside the run folder and everything
        # for that draw lands inside it — knvstd's own channel logs
        # (--log-dir, which also receives the diag bundle), the TLS wire
        # mirror (--wire-dump), this console copy and manifest.txt. The
        # manifest is written BEFORE the flight starts, so even a crash
        # leaves the provenance behind; run103/3 was a draw with no logs
        # at all, and that class is now impossible.
        run_name = self._run_name() or self._suggest_run_name()
        if not store.valid_run_name(run_name):
            QMessageBox.critical(self, "log folder",
                                 "bad run name: %s\nuse runNNN_logs "
                                 "(e.g. run104_logs)" % run_name)
            return
        try:
            draw_dir, draw_index = store.open_draw_dir(LOGS_DIR, run_name)
        except OSError as exc:
            QMessageBox.critical(self, "log folder",
                                 "cannot create the draw folder: %s" % exc)
            return
        argv = inject_draw_flags(argv, draw_dir)
        self.draw_dir, self.draw_index = draw_dir, draw_index
        console_path = os.path.join(draw_dir,
                                    self._console_log_name(run_name))
        try:
            self.log_fh = open(console_path, "a", encoding="utf-8")
        except OSError as exc:
            QMessageBox.critical(self, "log", "cannot open %s: %s" %
                                 (console_path, exc))
            return
        game = self._selected_game()
        variant = self._selected_variant() or {}
        store.write_manifest(os.path.join(draw_dir, "manifest.txt"), {
            "run": run_name,
            "draw": draw_index,
            "started_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "game_title": game.get("title") or "(typed --session-app)",
            "game_uuid": game.get("id") or "",
            "app_id": app_id if app_id is not None else "",
            "app_store": variant.get("store") or "",
            "variant_short_name": variant.get("short_name") or "",
            "owned": bool(game.get("owned")),
            "device_id": device,
            "knvstd": str(exe),
            "argv_redacted": redact_argv(argv,
                                         auth.service_token(self.bundle)),
            "params": user_params,
            "log_dir": draw_dir,
            "wire_dump": os.path.join(draw_dir, "wire_r1.txt"),
            "console_log": console_path,
        })
        self._print("$ %s" % " ".join(
            redact_argv(argv, auth.service_token(self.bundle))))
        self._print("# draw folder: %s" % draw_dir)
        self.proc = QProcess(self)
        self.proc.setWorkingDirectory(str(REPO_ROOT))
        self.proc.setProcessChannelMode(QProcess.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self._on_stdout)
        self.proc.finished.connect(self._on_finished)
        self.proc.start(argv[0], argv[1:])
        self.launch_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)
        self.logfolder.setText(run_name)
        self._update_draw_hint()

    def _on_stdout(self):
        if not self.proc:
            return
        data = bytes(self.proc.readAllStandardOutput())
        text = data.decode("utf-8", errors="replace")
        for line in text.splitlines():
            self.console.appendPlainText(line)
            if self.log_fh:
                try:
                    self.log_fh.write(line + "\n")
                    self.log_fh.flush()
                except OSError:
                    pass

    def _on_finished(self, code, status):
        tail = "exit code %s" % code
        if status == QProcess.CrashExit:
            tail += " (CRASH)"
        self._print("# knvstd finished — %s" % tail)
        if self.log_fh:
            self.log_fh.close()
            self.log_fh = None
        # Slice 4q: the draw folder must also say how the draw ENDED, so
        # a zipped run104_logs is self-describing without opening a log.
        if self.draw_dir:
            try:
                with open(os.path.join(self.draw_dir, "manifest.txt"), "a",
                          encoding="utf-8") as f:
                    f.write("%-20s %s\n" %
                            ("finished_at:",
                             time.strftime("%Y-%m-%d %H:%M:%S")))
                    f.write("%-20s %s\n" % ("exit_code:", code))
                    f.write("%-20s %s\n" %
                            ("crash:", status == QProcess.CrashExit))
            except OSError:
                pass
        self.launch_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        # The run name STAYS (the operator flies several draws per run);
        # only the draw number advances, and it does so by itself.
        self._update_draw_hint()

    def _stop_proc(self):
        if self.proc and self.proc.state() != QProcess.NotRunning:
            self._print("# stopping knvstd (terminate)…")
            self.proc.terminate()
            if not self.proc.waitForFinished(5000):
                self._print("# no exit in 5 s — kill")
                self.proc.kill()

    def closeEvent(self, ev):
        if self.login_worker and self.login_worker.isRunning():
            self.login_worker.stop()
            self.login_worker.wait(2000)
        if self.proc and self.proc.state() != QProcess.NotRunning:
            r = QMessageBox.question(
                self, "knvstd running",
                "A flight is still running. Terminate it and close?",
                QMessageBox.Yes | QMessageBox.No)
            if r != QMessageBox.Yes:
                ev.ignore()
                return
            self.proc.terminate()
            self.proc.waitForFinished(3000)
        if self.log_fh:
            self.log_fh.close()
        ev.accept()


def run() -> int:
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_QSS)
    win = MainWindow()
    win.show()
    return app.exec_()
