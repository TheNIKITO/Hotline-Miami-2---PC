#!/usr/bin/env python3
"""Hotline Miami 2 - launcher: choose the game folder, check for updates, press Play."""
import json
import os
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import webbrowser
import zipfile
from pathlib import Path

FROZEN = getattr(sys, "frozen", False)
HERE = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent
CONFIG = HERE / "launcher_config.json"
LOG = HERE / "launcher_last_run.log"
WINDOWS = sys.platform == "win32"
MAX_TRIES = 6          # attempts when the game closes by itself with an error
EARLY_SECONDS = 40     # an exit with an error before this counts as "closed by itself"

# Updates come from the latest GitHub release: its tag is the version, its .zip the package.
REPO = "TheNIKITO/Hotline-Miami-2---PC"
RELEASES_API = "https://api.github.com/repos/%s/releases/latest" % REPO
VERSION_FILE = HERE / "VERSION"      # the release tag this package was built as (package_hm2.sh)
UPDATE_DIR = HERE / "_update"
EXE_NAME = "Hotline Miami 2.exe"


def load_config():
    try:
        return json.loads(CONFIG.read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_config(cfg):
    try:
        CONFIG.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    except Exception:
        pass


def check_game_dir(folder):
    """(ok, message) for a selected game folder."""
    p = Path(folder) if folder else None
    if not p or not p.is_dir():
        return False, "Escolha a pasta do jogo."
    if not (p / "eboot.bin").is_file():
        return False, "Nao encontrei eboot.bin nessa pasta."
    missing = [n for n in ("sce_module", "sce_sys") if not (p / n).exists()]
    if missing:
        return False, "Faltam itens na pasta: " + ", ".join(missing)
    return True, "Jogo encontrado."


def python_exe():
    exe = Path(sys.executable)
    if exe.name.lower() == "pythonw.exe":
        cand = exe.with_name("python.exe")
        if cand.exists():
            return str(cand)
    return str(exe)


def game_command():
    """Frozen executable: it runs run.py itself (--run); otherwise Python runs it."""
    if FROZEN:
        return [sys.executable, "--run"]
    return [python_exe(), str(HERE / "run.py")]


def fix_std():
    """A windowed frozen program has no stdout/stderr: use the inherited handles."""
    for name, fd in (("stdout", 1), ("stderr", 2)):
        if getattr(sys, name, None) is None:
            try:
                os.fstat(fd)
                setattr(sys, name, os.fdopen(fd, "w", encoding="utf-8", errors="replace", buffering=1, closefd=False))
            except OSError:
                setattr(sys, name, open(os.devnull, "w"))


def helper_mode():
    """--run [args]: run.py; --script <file> [args]: a preparation script."""
    if len(sys.argv) < 2 or sys.argv[1] not in ("--run", "--script"):
        return
    fix_std()
    if sys.argv[1] == "--run":
        path, args = str(HERE / "run.py"), sys.argv[2:]
    else:
        if len(sys.argv) < 3:
            sys.exit(2)
        path, args = sys.argv[2], sys.argv[3:]
    sys.path.insert(0, str(Path(path).resolve().parent))
    sys.argv = [path] + args
    runpy.run_path(path, run_name="__main__")
    sys.exit(0)


def run_game(folder, retry, status, stop_flag, procs):
    """Runs run.py; repeats when the game closes by itself with an error."""
    env = dict(os.environ, GOW3_GAME_DIR=str(folder), PYTHONIOENCODING="utf-8")
    flags = subprocess.CREATE_NO_WINDOW if WINDOWS else 0
    tries = MAX_TRIES if retry else 1
    code = 0
    with open(LOG, "w", encoding="utf-8", errors="replace") as log:
        for attempt in range(1, tries + 1):
            if stop_flag.is_set():
                return -1
            status("Abrindo o jogo..." if attempt == 1 else "Abrindo de novo (tentativa %d de %d)..." % (attempt, tries))
            log.write("=== tentativa %d ===\n" % attempt)
            log.flush()
            start = time.monotonic()
            try:
                proc = subprocess.Popen(game_command(), cwd=str(HERE), env=env,
                                        stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        creationflags=flags)
            except OSError as exc:
                status("Erro ao iniciar: %s" % exc)
                return 1
            procs.append(proc)
            code = proc.wait()
            elapsed = time.monotonic() - start
            if stop_flag.is_set():
                return code
            if code == 0 or elapsed >= EARLY_SECONDS:
                break
            log.write("(fechou sozinho com codigo %s apos %.0fs)\n" % (code, elapsed))
    status("Jogo encerrado." if code == 0 else "O jogo fechou com erro (codigo %s). Veja o log." % code)
    return code


# ---- updates ------------------------------------------------------------------------------
def local_version():
    """The release tag this copy was packaged as ("v0.2"), or None (a source checkout)."""
    try:
        return VERSION_FILE.read_text(encoding="utf-8").strip() or None
    except OSError:
        return None


def version_key(text):
    """(0, 2, 1) for "v0.2.1": the numbers of a tag, compared in order."""
    return tuple(int(n) for n in re.findall(r"\d+", text or ""))


def http_get(url, timeout):
    return urllib.request.urlopen(urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json", "User-Agent": "hm2-launcher"}), timeout=timeout)


def latest_release(timeout=15):
    """{tag, page, asset} of the latest release; asset is its first .zip {name, url, size} or None."""
    with http_get(RELEASES_API, timeout) as r:
        data = json.load(r)
    asset = next(({"name": a.get("name", ""), "url": a.get("browser_download_url", ""), "size": a.get("size") or 0}
                  for a in data.get("assets", []) if a.get("name", "").lower().endswith(".zip")), None)
    return {"tag": data.get("tag_name", ""), "page": data.get("html_url", ""), "asset": asset}


def download(url, dest, size_hint, progress):
    """Saves url to dest, calling progress(done, total) as it goes."""
    with http_get(url, 60) as r, open(dest, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0) or size_hint
        done = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            done += len(chunk)
            progress(done, total)
    if total and done != total:
        raise IOError("download incompleto (%d de %d bytes)" % (done, total))


def unpack_update(zip_path, tag):
    """Extracts the release zip under UPDATE_DIR; returns the folder that holds the launcher."""
    target = UPDATE_DIR / "new"
    shutil.rmtree(target, ignore_errors=True)
    with zipfile.ZipFile(zip_path) as z:
        bad = z.testzip()
        if bad:
            raise IOError("arquivo corrompido no zip: %s" % bad)
        z.extractall(target)
    roots = [p.parent for p in target.rglob(EXE_NAME)]
    if not roots:
        raise IOError("o zip nao tem %s" % EXE_NAME)
    root = min(roots, key=lambda p: len(p.parts))
    (root / "VERSION").write_text(tag + "\n", encoding="utf-8")
    return root


# Runs after the launcher closes: copies the new files over this folder (saves, settings and
# logs are not in the package, so they stay), removes the download and opens the launcher.
APPLY_SCRIPT = r"""@echo off
timeout /t 2 /nobreak >nul
robocopy "%~1" "%~2" /E /R:30 /W:1 /NP /NFL /NDL /NJH /NJS >nul
if %ERRORLEVEL% LSS 8 rmdir /s /q "%~3"
start "" "%~2\Hotline Miami 2.exe"
del "%~f0"
"""


def apply_update(root):
    """Starts APPLY_SCRIPT detached; the caller must close the launcher right after."""
    fd, script = tempfile.mkstemp(prefix="hm2-update-", suffix=".cmd")
    with os.fdopen(fd, "w", encoding="ascii") as f:
        f.write(APPLY_SCRIPT)
    subprocess.Popen(["cmd", "/c", script, str(root), str(HERE), str(UPDATE_DIR)],
                     creationflags=subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP,
                     close_fds=True)


def main():
    import tkinter as tk
    from tkinter import filedialog, ttk

    if WINDOWS:
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    if FROZEN and UPDATE_DIR.exists():
        shutil.rmtree(UPDATE_DIR, ignore_errors=True)  # left by an update that has been applied
    cfg = load_config()
    version = local_version()
    root = tk.Tk()
    root.title("Hotline Miami 2 - PC")
    root.geometry("580x370")
    root.minsize(540, 350)
    pad = {"padx": 16}

    head = ttk.Frame(root)
    head.pack(fill="x", pady=(16, 2), **pad)
    ttk.Label(head, text="Hotline Miami 2: Wrong Number", font=("Segoe UI", 15, "bold")).pack(side="left")
    ttk.Label(head, text=version or "versao de desenvolvimento", foreground="#555").pack(side="right")
    ttk.Label(root, text="Pasta do jogo (a que tem o eboot.bin):").pack(anchor="w", pady=(8, 2), **pad)

    row = ttk.Frame(root)
    row.pack(fill="x", **pad)
    folder_var = tk.StringVar(value=cfg.get("game_dir", ""))
    entry = ttk.Entry(row, textvariable=folder_var)
    entry.pack(side="left", fill="x", expand=True)
    msg_var = tk.StringVar()
    msg = ttk.Label(root, textvariable=msg_var)
    msg.pack(anchor="w", pady=(4, 0), **pad)

    retry_var = tk.BooleanVar(value=cfg.get("retry", True))
    ttk.Checkbutton(root, text="Abrir de novo automaticamente se o jogo fechar sozinho",
                    variable=retry_var).pack(anchor="w", pady=(10, 0), **pad)

    upd = ttk.Frame(root)
    upd.pack(fill="x", pady=(14, 0), **pad)
    bar = ttk.Progressbar(upd, mode="indeterminate", maximum=100)
    bar.pack(side="left", fill="x", expand=True)
    update_btn = ttk.Button(upd, text="Atualizar", state="disabled")
    update_btn.pack(side="left", padx=(8, 0))
    update_var = tk.StringVar(value="")
    ttk.Label(root, textvariable=update_var, foreground="#555").pack(anchor="w", pady=(4, 0), **pad)

    status_var = tk.StringVar(value="")
    ttk.Label(root, textvariable=status_var, foreground="#555").pack(anchor="w", pady=(6, 0), **pad)

    btns = ttk.Frame(root)
    btns.pack(fill="x", pady=14, **pad)
    play = ttk.Button(btns, text="JOGAR")
    stop = ttk.Button(btns, text="Parar", state="disabled")
    logbtn = ttk.Button(btns, text="Abrir log")
    # update: "checking", "current", "available", "offline" (check failed) or "updating".
    # JOGAR waits for the check; it stays off while a newer release has not been installed.
    state = {"running": False, "stop": threading.Event(), "procs": [], "update": "checking", "release": None}

    def ui(fn, *args):
        root.after(0, lambda: fn(*args))

    def refresh(*_):
        ok, text = check_game_dir(folder_var.get().strip())
        msg_var.set(("OK: " if ok else "") + text)
        msg.configure(foreground="#1a7f37" if ok else "#b3261e")
        allowed = state["update"] in ("current", "offline") or (state["update"] == "available" and not FROZEN)
        play.configure(state="normal" if ok and allowed and not state["running"] else "disabled")
        can_update = state["update"] in ("available", "offline") and not state["running"]
        update_btn.configure(state="normal" if can_update else "disabled",
                             text="Verificar de novo" if state["update"] == "offline" else "Atualizar")
        return ok

    def bar_busy():
        bar.configure(mode="indeterminate")
        bar.start(12)

    def bar_value(value):
        bar.stop()
        bar.configure(mode="determinate", value=value)

    def checked(release, error):
        state["release"] = release
        if error:
            state["update"] = "offline"
            bar_value(0)
            update_var.set("Nao consegui verificar atualizacoes (%s). Pode jogar mesmo assim." % error)
        elif version and version_key(release["tag"]) <= version_key(version):
            state["update"] = "current"
            bar_value(100)
            update_var.set("Voce esta na versao mais recente (%s)." % version)
        elif not FROZEN:
            state["update"] = "available"
            bar_value(100)
            update_var.set("Versao %s no GitHub. Aqui (codigo-fonte) atualize pelo git; Atualizar abre a pagina."
                           % release["tag"])
        elif not release["asset"]:
            state["update"] = "offline"
            bar_value(0)
            update_var.set("A versao %s ainda nao tem o pacote .zip no GitHub. Pode jogar mesmo assim." % release["tag"])
        else:
            state["update"] = "available"
            bar_value(0)
            update_var.set("Nova versao %s disponivel (%.0f MB). Clique em Atualizar para poder jogar."
                           % (release["tag"], release["asset"]["size"] / 1e6))
        refresh()

    def check():
        state["update"] = "checking"
        update_var.set("Verificando atualizacoes...")
        bar_busy()
        refresh()

        def work():
            try:
                release, error = latest_release(), None
            except Exception as exc:
                release, error = None, str(getattr(exc, "reason", None) or exc)[:80]
            ui(checked, release, error)
        threading.Thread(target=work, daemon=True).start()

    def update():
        if state["update"] == "offline":
            check()
            return
        release = state["release"]
        if not FROZEN:
            webbrowser.open(release["page"])
            return
        state["update"] = "updating"
        bar_value(0)
        refresh()
        asset = release["asset"]
        shown = [0.0]

        def progress(done, total):
            now = time.monotonic()
            if now - shown[0] < 0.1 and done != total:
                return  # a few redraws per second are enough
            shown[0] = now
            ui(bar_value, done * 100.0 / total if total else 0)
            ui(update_var.set, "Baixando %s: %.0f de %.0f MB" % (release["tag"], done / 1e6, total / 1e6))

        def failed(error):
            state["update"] = "available"
            bar_value(0)
            update_var.set("A atualizacao falhou: %s. Tente de novo." % error)
            refresh()

        def ready(folder):
            update_var.set("Instalando %s... o launcher vai reabrir sozinho." % release["tag"])
            try:
                apply_update(folder)
            except Exception as exc:
                failed(exc)
                return
            root.after(800, root.destroy)

        def work():
            try:
                UPDATE_DIR.mkdir(exist_ok=True)
                zip_path = UPDATE_DIR / "update.zip"
                download(asset["url"], zip_path, asset["size"], progress)
                ui(update_var.set, "Conferindo os arquivos...")
                ui(bar_busy)
                folder = unpack_update(zip_path, release["tag"])
            except Exception as exc:
                ui(failed, str(exc)[:120])
                return
            ui(ready, folder)
        threading.Thread(target=work, daemon=True).start()

    def browse():
        d = filedialog.askdirectory(title="Escolha a pasta do jogo", initialdir=folder_var.get() or str(Path.home()))
        if d:
            folder_var.set(d)

    def say(text):
        root.after(0, lambda: status_var.set(text))

    def finished():
        state["running"] = False
        stop.configure(state="disabled")
        refresh()

    def worker(folder, retry):
        try:
            run_game(folder, retry, say, state["stop"], state["procs"])
        finally:
            root.after(0, finished)

    def start():
        if not refresh() or state["running"] or str(play["state"]) == "disabled":
            return
        folder = folder_var.get().strip()
        save_config({"game_dir": folder, "retry": bool(retry_var.get())})
        state["running"] = True
        state["stop"].clear()
        state["procs"].clear()
        play.configure(state="disabled")
        stop.configure(state="normal")
        refresh()
        threading.Thread(target=worker, args=(folder, bool(retry_var.get())), daemon=True).start()

    def halt():
        state["stop"].set()
        for p in list(state["procs"]):
            try:
                p.terminate()
            except Exception:
                pass
        say("Parando...")

    def open_log():
        if not LOG.exists():
            status_var.set("Ainda nao ha log.")
            return
        try:
            os.startfile(str(LOG)) if WINDOWS else subprocess.Popen(["xdg-open", str(LOG)])
        except Exception as exc:
            status_var.set("Nao consegui abrir o log: %s" % exc)

    ttk.Button(row, text="Procurar...", command=browse).pack(side="left", padx=(8, 0))
    play.configure(command=start)
    stop.configure(command=halt)
    logbtn.configure(command=open_log)
    update_btn.configure(command=update)
    play.pack(side="left", ipadx=24, ipady=6)
    stop.pack(side="left", padx=8, ipady=6)
    logbtn.pack(side="right", ipady=6)
    folder_var.trace_add("write", refresh)

    def on_close():
        if state["update"] == "updating":
            return  # the download finishes and the launcher closes itself to install
        if state["running"]:
            halt()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    refresh()
    check()
    root.mainloop()


if __name__ == "__main__":
    helper_mode()
    main()
