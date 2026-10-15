#!/usr/bin/env python3
"""Hotline Miami 2 - launcher: choose the game folder, press Play."""
import json
import os
import runpy
import subprocess
import sys
import threading
import time
from pathlib import Path

FROZEN = getattr(sys, "frozen", False)
HERE = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent
CONFIG = HERE / "launcher_config.json"
LOG = HERE / "launcher_last_run.log"
WINDOWS = sys.platform == "win32"
MAX_TRIES = 6          # attempts when the game closes by itself with an error
EARLY_SECONDS = 40     # an exit with an error before this counts as "closed by itself"


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


def main():
    import tkinter as tk
    from tkinter import filedialog, ttk

    if WINDOWS:
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
    cfg = load_config()
    root = tk.Tk()
    root.title("Hotline Miami 2 - PC")
    root.geometry("560x290")
    root.minsize(520, 270)
    pad = {"padx": 16}

    ttk.Label(root, text="Hotline Miami 2: Wrong Number", font=("Segoe UI", 15, "bold")).pack(anchor="w", pady=(16, 2), **pad)
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

    status_var = tk.StringVar(value="")
    ttk.Label(root, textvariable=status_var, foreground="#555").pack(anchor="w", pady=(10, 0), **pad)

    btns = ttk.Frame(root)
    btns.pack(fill="x", pady=14, **pad)
    play = ttk.Button(btns, text="JOGAR")
    stop = ttk.Button(btns, text="Parar", state="disabled")
    logbtn = ttk.Button(btns, text="Abrir log")
    state = {"running": False, "stop": threading.Event(), "procs": []}

    def refresh(*_):
        ok, text = check_game_dir(folder_var.get().strip())
        msg_var.set(("OK: " if ok else "") + text)
        msg.configure(foreground="#1a7f37" if ok else "#b3261e")
        play.configure(state="normal" if ok and not state["running"] else "disabled")
        return ok

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
        if not refresh() or state["running"]:
            return
        folder = folder_var.get().strip()
        save_config({"game_dir": folder, "retry": bool(retry_var.get())})
        state["running"] = True
        state["stop"].clear()
        state["procs"].clear()
        play.configure(state="disabled")
        stop.configure(state="normal")
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
    play.pack(side="left", ipadx=24, ipady=6)
    stop.pack(side="left", padx=8, ipady=6)
    logbtn.pack(side="right", ipady=6)
    folder_var.trace_add("write", refresh)

    def on_close():
        if state["running"]:
            halt()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    refresh()
    root.mainloop()


if __name__ == "__main__":
    helper_mode()
    main()
