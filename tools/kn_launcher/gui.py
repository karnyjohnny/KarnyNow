# gui.py — the dark-mode PyQt5 launcher.
#
# Tab 1 "Login": the RFC 8628 device grant — a copyable verification URL
# (PIN embedded when the endpoint supports it), the user code in big
# type, live poll status; on success the vault is sealed (DPAPI) and the
# token verified against v2/serverInfo (which doubles as the vpcId
# discovery). Tab 2 "Games & launch": the GraphQL catalog with OWNED
# badges, the smart search, the knvstd parameter block, the runNNN.log
# name (auto-suggested, operator numbers from 100) and the launch button
# (QProcess; stdout mirrored to the console pane AND the log file).
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

from PyQt5.QtCore import (QProcess, Qt, QThread, QUrl, pyqtSignal)
from PyQt5.QtGui import QDesktopServices, QFont
from PyQt5.QtWidgets import (
    QAbstractItemView, QApplication, QCheckBox, QHBoxLayout, QLabel,
    QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMessageBox,
    QPlainTextEdit, QPushButton, QTabWidget, QVBoxLayout, QWidget,
)

from . import auth, catalog, search, store
from .params import DEFAULT_PARAMS, split_params

REPO_ROOT = Path(__file__).resolve().parents[2]
LOGS_DIR = REPO_ROOT / "logs"

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
    progress = pyqtSignal(int, object)         # count, total|None|str
    done = pyqtSignal(list)
    fail = pyqtSignal(str)

    def __init__(self, token, vpc_id):
        super().__init__()
        self.token, self.vpc_id = token, vpc_id

    def run(self):
        try:
            def cb(n, total):
                self.progress.emit(n, total)
            games = catalog.fetch_games(self.token, self.vpc_id,
                                        progress=cb)
            self.done.emit(games)
        except Exception as exc:
            self.fail.emit(str(exc))


# ---------------------------------------------------------------- window
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("KarnyNow launcher")
        self.resize(1080, 760)
        self.bundle = None
        self.vpc_id = ""
        self.games = []
        self.login_worker = None
        self.verify_worker = None
        self.catalog_worker = None
        self.proc = None
        self.log_fh = None

        self.tabs = QTabWidget()
        self.setCentralWidget(self.tabs)
        self.tabs.addTab(self._build_login_tab(), "1 · Login")
        self.tabs.addTab(self._build_games_tab(), "2 · Games && launch")
        self.tabs.setTabEnabled(1, False)

        warn = store.plaintext_warning()
        if warn:
            self.login_status.setText("⚠ " + warn)
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
            if not self.games:
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

        top = QHBoxLayout()
        self.search_box = QLineEdit()
        self.search_box.setPlaceholderText(
            "smart search — try “endfield”, “ark end”, “aknd”…")
        self.search_box.textChanged.connect(self._render_games)
        top.addWidget(self.search_box, 1)
        self.owned_chk = QCheckBox("owned only")
        self.owned_chk.stateChanged.connect(self._render_games)
        top.addWidget(self.owned_chk)
        self.refresh_btn = QPushButton("Refresh catalog")
        self.refresh_btn.clicked.connect(self._refresh_catalog)
        top.addWidget(self.refresh_btn)
        lay.addLayout(top)

        self.catalog_status = QLabel("catalog not loaded yet")
        self.catalog_status.setObjectName("dim")
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
        self.params.setFixedHeight(120)
        right.addWidget(self.params)
        note = QLabel("--session, --session-token and --session-app are "
                      "added automatically (the token never passes "
                      "through this box or the log).")
        note.setObjectName("dim")
        note.setWordWrap(True)
        right.addWidget(note)

        lrow = QHBoxLayout()
        lrow.addWidget(QLabel("log file:"))
        self.logname = QLineEdit(self._suggest_log_name())
        self.logname.setFixedWidth(140)
        lrow.addWidget(self.logname)
        right.addLayout(lrow)

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
        return w

    def _suggest_log_name(self) -> str:
        """runNNN.log — the operator numbers from 100; suggest max+1."""
        best = 99
        try:
            for p in LOGS_DIR.glob("run*.log"):
                m = re.match(r"run(\d+)\.log$", p.name)
                if m:
                    best = max(best, int(m.group(1)))
        except OSError:
            pass
        return "run%d.log" % (best + 1)

    def _refresh_catalog(self):
        if not self.bundle or not self.vpc_id:
            return
        if self.catalog_worker and self.catalog_worker.isRunning():
            return
        self.refresh_btn.setEnabled(False)
        self.catalog_status.setText("fetching the library + catalog…")
        self.catalog_worker = CatalogWorker(
            auth.service_token(self.bundle), self.vpc_id)
        self.catalog_worker.progress.connect(self._catalog_progress)
        self.catalog_worker.done.connect(self._catalog_done)
        self.catalog_worker.fail.connect(self._catalog_fail)
        self.catalog_worker.start()

    def _catalog_progress(self, n, total):
        if isinstance(total, str):
            self.catalog_status.setText(total)
        elif n and n < 0:
            self.catalog_status.setText(str(total))
        else:
            self.catalog_status.setText("fetched %d apps%s…" %
                                        (n, (" / %s" % total) if total
                                         else ""))

    def _catalog_done(self, games):
        self.games = games
        self.refresh_btn.setEnabled(True)
        owned = sum(1 for g in games if g["owned"])
        self.catalog_status.setText(
            "%d games in the catalog · %d owned by this account" %
            (len(games), owned))
        self._render_games()

    def _catalog_fail(self, why):
        self.refresh_btn.setEnabled(True)
        self.catalog_status.setText("catalog failed: %s" % why)

    def _render_games(self):
        q = self.search_box.text()
        games = search.smart_filter(self.games, q, cap=300)
        if self.owned_chk.isChecked():
            games = [g for g in games if g["owned"]]
        self.game_list.clear()
        for g in games:
            badge = "★ OWNED  " if g["owned"] else "        "
            tier = ("  [%s]" % g["tier"]) if g.get("tier") else ""
            ids = ",".join(str(i) for i in g["app_ids"][:3])
            item = QListWidgetItem("%s%s — %s%s   ⟦app %s⟧" %
                                   (badge, g["title"],
                                    g.get("developer") or "?", tier,
                                    ids or "—"))
            item.setData(Qt.UserRole, g)
            self.game_list.addItem(item)

    def _on_game_selected(self):
        items = self.game_list.selectedItems()
        if items:
            g = items[0].data(Qt.UserRole)
            self.console.setPlainText("")
            self._print("# selected: %s (app ids: %s, owned: %s)" %
                        (g["title"], g["app_ids"], g["owned"]))

    def _selected_app_id(self):
        items = self.game_list.selectedItems()
        if not items:
            return None
        g = items[0].data(Qt.UserRole)
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
        if not any(a.startswith("--session-device") for a in extra):
            argv.append("--session-device=" + store.device_id())
        argv += extra
        log_name = self.logname.text().strip() or self._suggest_log_name()
        if not re.match(r"^[\w.\-]+\.log$", log_name):
            QMessageBox.critical(self, "log", "bad log name: %s" % log_name)
            return
        LOGS_DIR.mkdir(parents=True, exist_ok=True)
        log_path = LOGS_DIR / log_name
        try:
            self.log_fh = open(log_path, "a", encoding="utf-8")
        except OSError as exc:
            QMessageBox.critical(self, "log", "cannot open %s: %s" %
                                 (log_path, exc))
            return
        red = [a if not a.startswith("--session-token=")
               else "--session-token=<redacted:%d>" %
               len(auth.service_token(self.bundle)) for a in argv]
        self._print("$ %s" % " ".join(red))
        self.proc = QProcess(self)
        self.proc.setWorkingDirectory(str(REPO_ROOT))
        self.proc.setProcessChannelMode(QProcess.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self._on_stdout)
        self.proc.finished.connect(self._on_finished)
        self.proc.start(argv[0], argv[1:])
        self.launch_btn.setEnabled(False)
        self.stop_btn.setEnabled(True)

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
        self.launch_btn.setEnabled(True)
        self.stop_btn.setEnabled(False)
        self.logname.setText(self._suggest_log_name())

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
