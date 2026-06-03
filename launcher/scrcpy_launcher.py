import tkinter as tk
from tkinter import messagebox
import subprocess, threading, os, sys, json, re, time, ctypes
from datetime import datetime

# ─── Configuração ─────────────────────────────────────────────
SCRCPY_EXE = "base"
ADB_EXE    = "adb.exe"
WIFI_PORT  = 9990
DATA_FILE  = "devices_data.json"
POLL_MS    = 2500
MUTEX_NAME = "MyAndroid_SingleInstance_Mutex"

DEFAULT_DEV_CFG = {
    "fps":     "60",
    "res":     "1080",
    "bitrate": "8M",
    "audio":   True,
    "console": False,   # False = sem console (silencioso)
}
# ──────────────────────────────────────────────────────────────

def get_base_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

BASE_DIR   = get_base_dir()
SCRCPY     = os.path.join(BASE_DIR, SCRCPY_EXE)
ADB        = os.path.join(BASE_DIR, ADB_EXE)
DATA_PATH  = os.path.join(BASE_DIR, DATA_FILE)
PRINTS_DIR = os.path.join(BASE_DIR, "Prints")

# ─── Instância única ──────────────────────────────────────────
def ensure_single_instance():
    k32   = ctypes.windll.kernel32
    mutex = k32.CreateMutexW(None, True, MUTEX_NAME)
    if k32.GetLastError() == 183:
        u32  = ctypes.windll.user32
        hwnd = u32.FindWindowW(None, "My Android")
        if hwnd:
            u32.ShowWindow(hwnd, 9)
            u32.SetForegroundWindow(hwnd)
        sys.exit(0)
    return mutex

# ─── Devices data ─────────────────────────────────────────────
def load_data():
    if os.path.exists(DATA_PATH):
        try:
            with open(DATA_PATH, "r", encoding="utf-8") as f:
                raw = json.load(f)
            return {k: v for k, v in raw.items() if not is_wifi_serial(k)}
        except:
            pass
    return {}

def save_data(data):
    try:
        with open(DATA_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except:
        pass

def get_dev_cfg(data, usb_serial):
    """Retorna config do dispositivo com fallback para padrões."""
    entry = data.get(usb_serial, {})
    cfg   = dict(DEFAULT_DEV_CFG)
    for k in DEFAULT_DEV_CFG:
        if k in entry:
            cfg[k] = entry[k]
    return cfg

def save_dev_cfg(data, usb_serial, cfg):
    """Salva config no entry do dispositivo."""
    entry = data.setdefault(usb_serial, {})
    for k in DEFAULT_DEV_CFG:
        entry[k] = cfg[k]
    save_data(data)

# ─── ADB ──────────────────────────────────────────────────────
def run_adb(*args, timeout=8):
    try:
        r = subprocess.run(
            [ADB, *args], capture_output=True, text=True,
            timeout=timeout, cwd=BASE_DIR,
            creationflags=subprocess.CREATE_NO_WINDOW)
        return r.stdout.strip()
    except:
        return ""

def is_wifi_serial(s):
    return bool(re.match(r'^\d+\.\d+\.\d+\.\d+:\d+$', s))

def get_ip_from_device(serial):
    out = run_adb("-s", serial, "shell", "ifconfig wlan0")
    m = re.search(r'inet addr:([\d.]+)', out)
    if m: return m.group(1)
    out = run_adb("-s", serial, "shell", "ip addr show wlan0")
    m = re.search(r'inet ([\d.]+)/', out)
    if m: return m.group(1)
    return None

def fetch_raw_devices():
    out  = run_adb("devices", "-l")
    devs = []
    for linha in out.splitlines()[1:]:
        linha = linha.strip()
        if not linha: continue
        partes = linha.split()
        if len(partes) < 2: continue
        serial, status = partes[0], partes[1]
        modelo = next((p.replace("model:","").replace("_"," ")
                       for p in partes if p.startswith("model:")), serial)
        devs.append({"serial": serial, "modelo": modelo, "status": status})
    return devs

def build_groups(raw_devs, data):
    usb_map, wifi_map = {}, {}
    for d in raw_devs:
        (wifi_map if is_wifi_serial(d["serial"]) else usb_map)[d["serial"]] = d
    groups = []
    for usb_serial, entry in data.items():
        usb_d      = usb_map.get(usb_serial)
        usb_on     = usb_d is not None and usb_d["status"] == "device"
        modelo     = usb_d["modelo"] if usb_d else entry.get("_modelo", usb_serial)
        wifi_serial = entry.get("wifi_serial")
        wifi_on    = False
        if wifi_serial:
            wd      = wifi_map.get(wifi_serial)
            wifi_on = wd is not None and wd["status"] != "offline"
        if not usb_on and not wifi_on:
            continue
        groups.append({"key": usb_serial, "usb_serial": usb_serial,
                        "usb_on": usb_on, "wifi_serial": wifi_serial,
                        "wifi_on": wifi_on, "modelo": modelo})
    return groups

# ─── Lança scrcpy ─────────────────────────────────────────────
# Processos ativos: key → Popen
_active_procs = {}

def build_scrcpy_cmd(scrcpy_path, target, title, cfg):
    cmd = [scrcpy_path,
           "-s", target,
           "--window-title", title,
           "--gamepad=uhid",
           "--print-fps",
           f"--max-fps={cfg['fps']}",
           "--stay-awake",
           f"-m", cfg["res"],
           f"-b", cfg["bitrate"]]
    if not cfg.get("audio", True):
        cmd.append("--no-audio")
    return cmd

def launch_scrcpy(target, title, cfg, on_exit=None):
    """Lança scrcpy e retorna o Popen. on_exit chamado ao fechar."""
    env = os.environ.copy()
    env["SCRCPY_SERVER_PATH"] = os.path.join(BASE_DIR, "server")

    cmd = build_scrcpy_cmd(SCRCPY, target, title, cfg)

    # Modo console: com janela visível. Silencioso: sem janela.
    flags = 0 if cfg.get("console", False) else subprocess.CREATE_NO_WINDOW

    proc = subprocess.Popen(cmd, cwd=BASE_DIR, env=env,
                            creationflags=flags)
    if on_exit:
        def _watch():
            proc.wait()
            on_exit(proc)
        threading.Thread(target=_watch, daemon=True).start()
    return proc

# ─── Janela flutuante de controle (toolbar do dispositivo) ────
class DeviceToolbar(tk.Toplevel):
    """
    Janela flutuante pequena que aparece ao conectar um dispositivo.
    Fica sempre no topo, pode ser arrastada.
    Contém: print, áudio, vol−, vol+, voltar, home, recentes.
    """
    def __init__(self, parent, serial, nome, on_close_cb=None):
        super().__init__(parent)
        self.serial      = serial
        self.nome        = nome
        self.on_close_cb = on_close_cb
        self._audio_on   = True

        self.title(f"⚡ {nome}")
        self.resizable(False, False)
        self.configure(bg="#161616")
        self.attributes("-topmost", True)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # Arrastar
        self._drag_x = self._drag_y = 0
        self.bind("<ButtonPress-1>",   self._drag_start)
        self.bind("<B1-Motion>",       self._drag_move)

        self._build()

        # Posiciona no canto superior direito
        self.update_idletasks()
        sw = self.winfo_screenwidth()
        self.geometry(f"+{sw - self.winfo_width() - 20}+60")

    def _build(self):
        def btn(sym, tip, cmd):
            b = tk.Button(self, text=sym, font=("Segoe UI", 13),
                          bg="#161616", fg="#cccccc",
                          activebackground="#2a2a2a",
                          activeforeground="#ffffff",
                          relief="flat", cursor="hand2",
                          width=2, pady=6, bd=0,
                          command=cmd)
            b.pack(side="left", padx=1)
            b.bind("<Enter>", lambda e, t=tip: self._tip(t))
            b.bind("<Leave>", lambda e: self._tip(""))
            return b

        self.lbl_tip = tk.Label(self, text="", font=("Segoe UI", 7),
                                bg="#161616", fg="#555555", width=18)
        self.lbl_tip.pack(side="bottom", fill="x")

        bar = tk.Frame(self, bg="#161616")
        bar.pack(padx=4, pady=(6,2))

        def mkbtn(sym, tip, cmd):
            b = tk.Button(bar, text=sym, font=("Segoe UI", 13),
                          bg="#161616", fg="#cccccc",
                          activebackground="#2a2a2a",
                          activeforeground="#ffffff",
                          relief="flat", cursor="hand2",
                          width=2, pady=5, bd=0,
                          command=cmd)
            b.pack(side="left", padx=2)
            b.bind("<Enter>", lambda e, t=tip: self._tip(t))
            b.bind("<Leave>", lambda e: self._tip(""))
            return b

        mkbtn("📷", "Print da tela",     self._screenshot)
        self.btn_aud = mkbtn("🔊", "Áudio Windows/Celular", self._toggle_audio)
        mkbtn("🔉", "Volume −",          self._vol_down)
        mkbtn("🔊", "Volume +",          self._vol_up)

        tk.Frame(bar, bg="#333333", width=1, height=28).pack(side="left", padx=3)

        mkbtn("◀", "Voltar",    self._nav_back)
        mkbtn("⏺", "Home",     self._nav_home)
        mkbtn("▦", "Recentes", self._nav_recents)

    def _tip(self, text):
        self.lbl_tip.config(text=text)

    def _drag_start(self, e):
        self._drag_x = e.x_root - self.winfo_x()
        self._drag_y = e.y_root - self.winfo_y()

    def _drag_move(self, e):
        self.geometry(f"+{e.x_root - self._drag_x}+{e.y_root - self._drag_y}")

    def _on_close(self):
        if self.on_close_cb:
            self.on_close_cb()
        self.destroy()

    # ── Ações ─────────────────────────────────────────────────
    def _screenshot(self):
        os.makedirs(PRINTS_DIR, exist_ok=True)
        ts   = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(PRINTS_DIR, f"print_{ts}.png")
        def run():
            try:
                out = subprocess.check_output(
                    [ADB, "-s", self.serial, "exec-out", "screencap", "-p"],
                    cwd=BASE_DIR,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                with open(path, "wb") as f:
                    f.write(out)
                self.after(0, lambda: messagebox.showinfo(
                    "Print salvo", f"Salvo em:\nPrints\\print_{ts}.png"))
            except Exception as ex:
                self.after(0, lambda: messagebox.showerror(
                    "Erro", f"Falha:\n{ex}"))
        threading.Thread(target=run, daemon=True).start()

    def _toggle_audio(self):
        self._audio_on = not self._audio_on
        self.btn_aud.config(text="🔊" if self._audio_on else "🔇")

    def _adb(self, *args):
        threading.Thread(
            target=lambda: run_adb("-s", self.serial, *args),
            daemon=True).start()

    def _vol_down(self):
        self._adb("shell", "input keyevent KEYCODE_VOLUME_DOWN")

    def _vol_up(self):
        self._adb("shell", "input keyevent KEYCODE_VOLUME_UP")

    def _nav_back(self):
        self._adb("shell", "input keyevent KEYCODE_BACK")

    def _nav_home(self):
        self._adb("shell", "input keyevent KEYCODE_HOME")

    def _nav_recents(self):
        self._adb("shell", "input keyevent KEYCODE_APP_SWITCH")


# ─── Janela de Configurações por dispositivo ──────────────────
class SettingsWindow(tk.Toplevel):
    def __init__(self, parent, usb_serial, nome, data, on_save):
        super().__init__(parent)
        self.usb_serial = usb_serial
        self.data       = data
        self.on_save    = on_save
        self.cfg        = get_dev_cfg(data, usb_serial)

        self.title(f"Configurações — {nome}")
        self.geometry("380x360")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")
        self.grab_set()

        self._build()

    def _build(self):
        tk.Label(self, text="Configurações de Transmissão",
                 font=("Segoe UI", 11, "bold"),
                 bg="#1e1e1e", fg="#ffffff").pack(pady=(14, 6))

        pad = {"padx": 22, "pady": 5}

        def row(label, var, options):
            f = tk.Frame(self, bg="#1e1e1e")
            f.pack(fill="x", **pad)
            tk.Label(f, text=label, font=("Segoe UI", 9),
                     bg="#1e1e1e", fg="#aaaaaa",
                     width=16, anchor="w").pack(side="left")
            btns = {}
            for opt in options:
                sel = var.get() == opt
                b = tk.Button(f, text=opt,
                              font=("Segoe UI", 8, "bold"),
                              bg="#1976d2" if sel else "#2d2d2d",
                              fg="#ffffff",
                              relief="flat", padx=9, pady=3,
                              cursor="hand2")
                b.pack(side="left", padx=2)
                btns[opt] = b
                b.config(command=lambda v=opt, vr=var, bs=btns:
                         self._sel(v, vr, bs))
            return btns

        self.var_fps = tk.StringVar(value=self.cfg["fps"])
        self.var_res = tk.StringVar(value=self.cfg["res"])
        self.var_bit = tk.StringVar(value=self.cfg["bitrate"])
        self.var_aud = tk.BooleanVar(value=self.cfg["audio"])
        self.var_con = tk.BooleanVar(value=self.cfg["console"])

        row("FPS",       self.var_fps, ["30", "60", "120", "240"])
        row("Resolução", self.var_res, ["540", "720", "1080", "1440"])
        row("Bitrate",   self.var_bit, ["4M", "8M", "16M", "32M"])

        # Áudio
        fa = tk.Frame(self, bg="#1e1e1e")
        fa.pack(fill="x", padx=22, pady=5)
        tk.Label(fa, text="Áudio no Windows",
                 font=("Segoe UI", 9), bg="#1e1e1e", fg="#aaaaaa",
                 width=16, anchor="w").pack(side="left")
        tk.Checkbutton(fa, variable=self.var_aud,
                       text="Ativado", font=("Segoe UI", 9),
                       bg="#1e1e1e", fg="#ffffff",
                       selectcolor="#1976d2",
                       activebackground="#1e1e1e",
                       cursor="hand2").pack(side="left")

        # Console
        fc = tk.Frame(self, bg="#1e1e1e")
        fc.pack(fill="x", padx=22, pady=5)
        tk.Label(fc, text="Mostrar console",
                 font=("Segoe UI", 9), bg="#1e1e1e", fg="#aaaaaa",
                 width=16, anchor="w").pack(side="left")
        tk.Checkbutton(fc, variable=self.var_con,
                       text="Ativado (janela CMD visível)",
                       font=("Segoe UI", 9),
                       bg="#1e1e1e", fg="#ffffff",
                       selectcolor="#1976d2",
                       activebackground="#1e1e1e",
                       cursor="hand2").pack(side="left")

        # Botão Salvar
        tk.Button(self, text="💾  Salvar e Reconectar",
                  font=("Segoe UI", 10, "bold"),
                  bg="#1976d2", fg="#ffffff",
                  activebackground="#1565c0",
                  relief="flat", cursor="hand2",
                  padx=20, pady=7,
                  command=self._save).pack(pady=16)

    def _sel(self, val, var, btns):
        var.set(val)
        for k, b in btns.items():
            b.config(bg="#1976d2" if k == val else "#2d2d2d")

    def _save(self):
        self.cfg["fps"]     = self.var_fps.get()
        self.cfg["res"]     = self.var_res.get()
        self.cfg["bitrate"] = self.var_bit.get()
        self.cfg["audio"]   = self.var_aud.get()
        self.cfg["console"] = self.var_con.get()
        save_dev_cfg(self.data, self.usb_serial, self.cfg)
        self.on_save(self.cfg)
        self.destroy()


# ─── App principal ────────────────────────────────────────────
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("My Android")
        self.geometry("560x440")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")

        ico = os.path.join(BASE_DIR, "icone.ico")
        if os.path.exists(ico):
            try: self.iconbitmap(ico)
            except: pass

        self.data      = load_data()
        self._rows     = {}
        self._polling  = True
        # Processos e toolbars ativos: key → {"proc": Popen, "toolbar": DeviceToolbar}
        self._sessions = {}

        self._build_ui()
        self._poll()

    def _build_ui(self):
        hdr = tk.Frame(self, bg="#111111", pady=10)
        hdr.pack(fill="x")
        tk.Label(hdr, text="My Android",
                 font=("Segoe UI", 15, "bold"),
                 bg="#111111", fg="#ffffff").pack()
        tk.Label(hdr, text="Dispositivos detectados automaticamente",
                 font=("Segoe UI", 8), bg="#111111", fg="#555555").pack()

        self.frame_lista = tk.Frame(self, bg="#1e1e1e")
        self.frame_lista.pack(fill="both", expand=True, padx=18, pady=10)

        ftr = tk.Frame(self, bg="#141414", pady=5)
        ftr.pack(fill="x")
        self.lbl_status = tk.Label(ftr, text="Aguardando...",
                                   font=("Segoe UI", 8),
                                   bg="#141414", fg="#444444")
        self.lbl_status.pack(side="left", padx=14)

    # ── Polling ───────────────────────────────────────────────
    def _poll(self):
        if not self._polling: return
        threading.Thread(target=self._fetch, daemon=True).start()

    def _fetch(self):
        raw = fetch_raw_devices()
        for d in raw:
            if is_wifi_serial(d["serial"]): continue
            if d["status"] != "device": continue
            usb = d["serial"]
            ip  = get_ip_from_device(usb)
            if not ip: continue
            new_wifi = f"{ip}:{WIFI_PORT}"
            entry    = self.data.setdefault(usb, {})
            old_wifi = entry.get("wifi_serial")
            entry["_modelo"] = d["modelo"]
            if old_wifi != new_wifi:
                if old_wifi: run_adb("disconnect", old_wifi)
                entry["wifi_serial"] = new_wifi
                save_data(self.data)
                run_adb("-s", usb, "tcpip", str(WIFI_PORT))
                time.sleep(0.6)
                run_adb("connect", new_wifi)
            else:
                run_adb("connect", new_wifi)
        groups = build_groups(raw, self.data)
        self.after(0, self._update_ui, groups)

    def _update_ui(self, groups):
        keys_now = {g["key"] for g in groups}
        for k in list(self._rows.keys()):
            if k not in keys_now:
                self._rows[k]["frame"].destroy()
                del self._rows[k]
        for g in groups:
            if g["key"] not in self._rows:
                self._build_row(g)
            else:
                self._update_row(g)
        n = len(groups)
        self.lbl_status.config(
            text=(f"{n} dispositivo{'s' if n!=1 else ''} encontrado{'s' if n!=1 else ''}"
                  if n else "Nenhum dispositivo conectado"))
        self.after(POLL_MS, self._poll)

    # ── Row ───────────────────────────────────────────────────
    def _get_name(self, key, modelo):
        return self.data.get(key, {}).get("name", modelo)

    def _build_row(self, g):
        key    = g["key"]
        modelo = g["modelo"]

        frame = tk.Frame(self.frame_lista, bg="#2a2a2a",
                         pady=7, padx=10,
                         highlightbackground="#3a3a3a",
                         highlightthickness=1)
        frame.pack(fill="x", pady=3)

        left = tk.Frame(frame, bg="#2a2a2a")
        left.pack(side="left", fill="x", expand=True)

        tk.Label(left, text="📱", font=("Segoe UI", 16),
                 bg="#2a2a2a").pack(side="left", padx=(0, 7))

        info = tk.Frame(left, bg="#2a2a2a")
        info.pack(side="left")

        nome     = self._get_name(key, modelo)
        lbl_nome = tk.Label(info, text=nome,
                            font=("Segoe UI", 10, "bold"),
                            bg="#2a2a2a", fg="#ffffff",
                            cursor="hand2", anchor="w")
        lbl_nome.pack(anchor="w")
        lbl_nome.bind("<Button-1>",
                      lambda e, k=key, m=modelo: self._editar_nome(k, m))

        lbl_usb  = tk.Label(info, text="", font=("Segoe UI", 7),
                            bg="#2a2a2a", anchor="w")
        lbl_usb.pack(anchor="w")
        lbl_wifi = tk.Label(info, text="", font=("Segoe UI", 7),
                            bg="#2a2a2a", anchor="w")
        lbl_wifi.pack(anchor="w")

        right    = tk.Frame(frame, bg="#2a2a2a")
        right.pack(side="right", anchor="center")

        modo_var = tk.StringVar(value="usb")

        btn_cfg = tk.Button(right, text="⚙",
                            font=("Segoe UI", 11),
                            bg="#2a2a2a", fg="#666666",
                            activebackground="#333333",
                            activeforeground="#aaaaaa",
                            relief="flat", cursor="hand2",
                            padx=4, pady=4, bd=0)
        btn_cfg.pack(side="left", padx=(0, 4))
        btn_cfg.config(command=lambda k=key: self._open_settings(k))

        btn_modo = tk.Button(right, text="USB ▾",
                             font=("Segoe UI", 8, "bold"),
                             bg="#1a3a1a", fg="#4caf50",
                             activebackground="#224422",
                             activeforeground="#66bb6a",
                             relief="flat", cursor="hand2",
                             padx=9, pady=5, bd=0)
        btn_modo.pack(side="left", padx=(0, 2))

        btn_conectar = tk.Button(right, text="Conectar",
                                 font=("Segoe UI", 9, "bold"),
                                 bg="#1976d2", fg="#ffffff",
                                 activebackground="#1565c0",
                                 activeforeground="#ffffff",
                                 relief="flat", cursor="hand2",
                                 padx=11, pady=5, bd=0)
        btn_conectar.pack(side="left")

        self._rows[key] = {
            "frame": frame, "lbl_nome": lbl_nome,
            "lbl_usb": lbl_usb, "lbl_wifi": lbl_wifi,
            "btn_modo": btn_modo, "btn_conectar": btn_conectar,
            "btn_cfg": btn_cfg,
            "modo_var": modo_var, "modelo": modelo, "g": g,
            "tem_gaveta": False,
        }

        btn_modo.config(command=lambda k=key: self._abrir_menu(k))
        btn_conectar.config(command=lambda k=key: self._conectar(k))
        self._update_row(g)

    def _update_row(self, g):
        key = g["key"]
        row = self._rows.get(key)
        if not row: return
        row["g"] = g

        if g["usb_serial"]:
            cor = "#4caf50" if g["usb_on"] else "#555555"
            row["lbl_usb"].config(
                text=f"🔌 {g['usb_serial']}  •  {'ON' if g['usb_on'] else 'OFF'}",
                fg=cor)
        else:
            row["lbl_usb"].config(text="")

        if g["wifi_serial"]:
            cor = "#4caf50" if g["wifi_on"] else "#555555"
            row["lbl_wifi"].config(
                text=f"📶 {g['wifi_serial']}  •  {'ON' if g['wifi_on'] else 'OFF'}",
                fg=cor)
        else:
            row["lbl_wifi"].config(text="")

        usb_on  = g["usb_on"]
        wifi_on = g["wifi_on"]
        modo    = row["modo_var"].get()

        if modo == "usb" and not usb_on and wifi_on:
            modo = "wifi"; row["modo_var"].set("wifi")
        elif modo == "wifi" and not wifi_on and usb_on:
            modo = "usb";  row["modo_var"].set("usb")

        btn = row["btn_modo"]
        if usb_on and wifi_on:
            row["tem_gaveta"] = True
            btn.config(state="normal", cursor="hand2")
            if modo == "wifi":
                btn.config(text="Wi-Fi ▾", bg="#1a2a3a", fg="#42a5f5",
                           activebackground="#1e3550",
                           activeforeground="#64b5f6")
                row["btn_conectar"].config(bg="#00796b",
                                           activebackground="#00695c")
            else:
                btn.config(text="USB ▾", bg="#1a3a1a", fg="#4caf50",
                           activebackground="#224422",
                           activeforeground="#66bb6a")
                row["btn_conectar"].config(bg="#1976d2",
                                           activebackground="#1565c0")
        elif usb_on:
            row["tem_gaveta"] = False
            row["modo_var"].set("usb"); modo = "usb"
            btn.config(text="USB", state="disabled", cursor="",
                       bg="#1a3a1a", fg="#4caf50",
                       activebackground="#1a3a1a",
                       activeforeground="#4caf50")
            row["btn_conectar"].config(bg="#1976d2",
                                       activebackground="#1565c0")
        else:
            row["tem_gaveta"] = False
            row["modo_var"].set("wifi"); modo = "wifi"
            btn.config(text="Wi-Fi", state="disabled", cursor="",
                       bg="#1a2a3a", fg="#42a5f5",
                       activebackground="#1a2a3a",
                       activeforeground="#42a5f5")
            row["btn_conectar"].config(bg="#00796b",
                                       activebackground="#00695c")

        # Destaca se tem sessão ativa
        has_session = key in self._sessions
        row["frame"].config(
            highlightbackground="#1976d2" if has_session else "#3a3a3a")

    # ── Menu USB/WiFi ─────────────────────────────────────────
    def _abrir_menu(self, key):
        row = self._rows.get(key)
        if not row or not row["tem_gaveta"]: return
        g    = row["g"]
        modo = row["modo_var"].get()

        menu = tk.Menu(self, tearoff=0, bg="#2d2d2d", fg="#ffffff",
                       activebackground="#333333", activeforeground="#ffffff",
                       font=("Segoe UI", 9), bd=0)

        def set_usb():
            row["modo_var"].set("usb")
            row["btn_modo"].config(text="USB ▾", bg="#1a3a1a", fg="#4caf50",
                                   activebackground="#224422",
                                   activeforeground="#66bb6a")
            row["btn_conectar"].config(bg="#1976d2",
                                       activebackground="#1565c0")

        def set_wifi():
            row["modo_var"].set("wifi")
            row["btn_modo"].config(text="Wi-Fi ▾", bg="#1a2a3a", fg="#42a5f5",
                                   activebackground="#1e3550",
                                   activeforeground="#64b5f6")
            row["btn_conectar"].config(bg="#00796b",
                                       activebackground="#00695c")

        if g["usb_on"]:
            menu.add_command(label="🔌  USB" + ("  ✓" if modo=="usb" else ""),
                             command=set_usb)
        if g["wifi_on"]:
            menu.add_command(label="📶  Wi-Fi" + ("  ✓" if modo=="wifi" else ""),
                             command=set_wifi)

        btn = row["btn_modo"]
        menu.tk_popup(btn.winfo_rootx(),
                      btn.winfo_rooty() + btn.winfo_height())

    # ── Configurações ─────────────────────────────────────────
    def _open_settings(self, key):
        row   = self._rows.get(key)
        if not row: return
        nome  = self._get_name(key, row["modelo"])

        def on_save(new_cfg):
            # Reconecta se houver sessão ativa
            if key in self._sessions:
                self._reconectar(key, new_cfg)

        SettingsWindow(self, key, nome, self.data, on_save)

    def _reconectar(self, key, cfg):
        """Encerra sessão atual e relança com novas configs."""
        sess = self._sessions.get(key)
        if sess:
            # Fecha toolbar
            tb = sess.get("toolbar")
            if tb and tb.winfo_exists():
                try: tb.destroy()
            except: pass
            # Termina processo
            proc = sess.get("proc")
            if proc:
                try: proc.terminate()
                except: pass
            del self._sessions[key]

        # Aguarda um momento e relança
        self.after(800, lambda: self._conectar(key, force_cfg=cfg))

    # ── Conectar ──────────────────────────────────────────────
    def _conectar(self, key, force_cfg=None):
        row = self._rows.get(key)
        if not row: return
        g    = row["g"]
        modo = row["modo_var"].get()
        nome = self._get_name(key, row["modelo"])
        btn  = row["btn_conectar"]
        cfg  = force_cfg if force_cfg else get_dev_cfg(self.data, key)

        btn.config(state="disabled", text="Aguarde...")

        def run():
            try:
                target = g["wifi_serial"] if modo=="wifi" else g["usb_serial"]
                if not target:
                    self.after(0, messagebox.showerror,
                               "Erro", "Serial não disponível.")
                    return
                tipo  = "WI-FI" if modo=="wifi" else "USB"
                title = f"{tipo} - {nome}"
                proc  = launch_scrcpy(target, title, cfg)

                # Cria toolbar flutuante
                def make_toolbar():
                    tb = DeviceToolbar(
                        self, target, nome,
                        on_close_cb=lambda k=key: self._session_ended(k))
                    self._sessions[key] = {"proc": proc, "toolbar": tb}
                    row["frame"].config(highlightbackground="#1976d2")

                self.after(0, make_toolbar)

                # Monitora encerramento do scrcpy
                def watch():
                    proc.wait()
                    self.after(0, lambda: self._session_ended(key))
                threading.Thread(target=watch, daemon=True).start()

            except FileNotFoundError:
                self.after(0, messagebox.showerror, "Erro",
                           f"Arquivo 'base' não encontrado em:\n{BASE_DIR}")
            finally:
                self.after(600, lambda: btn.config(
                    state="normal", text="Conectar"))

        threading.Thread(target=run, daemon=True).start()

    def _session_ended(self, key):
        """Chamado quando scrcpy ou toolbar fecha."""
        sess = self._sessions.pop(key, None)
        if sess:
            tb = sess.get("toolbar")
            if tb:
                try:
                    if tb.winfo_exists(): tb.destroy()
                except: pass
        row = self._rows.get(key)
        if row:
            row["frame"].config(highlightbackground="#3a3a3a")

    # ── Editar nome ───────────────────────────────────────────
    def _editar_nome(self, key, modelo):
        row = self._rows.get(key)
        if not row: return
        lbl        = row["lbl_nome"]
        nome_atual = self._get_name(key, modelo)
        lbl.pack_forget()
        entry = tk.Entry(lbl.master, font=("Segoe UI", 10, "bold"),
                         bg="#3a3a3a", fg="#ffffff",
                         insertbackground="#ffffff",
                         relief="flat", width=20)
        entry.insert(0, nome_atual)
        entry.pack(anchor="w")
        entry.focus_set()
        entry.select_range(0, tk.END)

        def salvar(event=None):
            novo = entry.get().strip() or modelo
            self.data.setdefault(key, {})["name"] = novo
            save_data(self.data)
            entry.destroy()
            lbl.config(text=novo)
            lbl.pack(anchor="w")

        def cancelar(event=None):
            entry.destroy()
            lbl.pack(anchor="w")

        entry.bind("<Return>",   salvar)
        entry.bind("<Escape>",   cancelar)
        entry.bind("<FocusOut>", salvar)

    def on_close(self):
        self._polling = False
        # Encerra todas as sessões
        for key, sess in list(self._sessions.items()):
            tb = sess.get("toolbar")
            if tb:
                try:
                    if tb.winfo_exists(): tb.destroy()
                except: pass
        self.destroy()


if __name__ == "__main__":
    _mutex = ensure_single_instance()
    app    = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()
