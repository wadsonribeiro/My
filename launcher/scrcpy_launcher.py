import tkinter as tk
from tkinter import messagebox
import subprocess, threading, os, sys, json, re, time, ctypes
from datetime import datetime

# ─── Configuração ─────────────────────────────────────────────
SCRCPY_EXE = "base"
ADB_EXE    = "adb.exe"
WIFI_PORT  = 9990
DATA_FILE  = "devices_data.json"
CFG_FILE   = "settings.json"
POLL_MS    = 2500
MUTEX_NAME = "MyAndroid_SingleInstance_Mutex"
DRAWER_W   = 52   # largura da gaveta lateral
# ──────────────────────────────────────────────────────────────

def get_base_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

BASE_DIR  = get_base_dir()
SCRCPY    = os.path.join(BASE_DIR, SCRCPY_EXE)
ADB       = os.path.join(BASE_DIR, ADB_EXE)
DATA_PATH = os.path.join(BASE_DIR, DATA_FILE)
CFG_PATH  = os.path.join(BASE_DIR, CFG_FILE)

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

# ─── Settings ─────────────────────────────────────────────────
DEFAULT_CFG = {"fps": "60", "res": "1080", "bitrate": "8M", "audio": True}

def load_cfg():
    if os.path.exists(CFG_PATH):
        try:
            with open(CFG_PATH, "r") as f:
                c = json.load(f)
            for k, v in DEFAULT_CFG.items():
                c.setdefault(k, v)
            return c
        except:
            pass
    return dict(DEFAULT_CFG)

def save_cfg(cfg):
    try:
        with open(CFG_PATH, "w") as f:
            json.dump(cfg, f, indent=2)
    except:
        pass

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
        usb_d   = usb_map.get(usb_serial)
        usb_on  = usb_d is not None and usb_d["status"] == "device"
        modelo  = usb_d["modelo"] if usb_d else entry.get("_modelo", usb_serial)
        wifi_serial = entry.get("wifi_serial")
        wifi_on = False
        if wifi_serial:
            wd = wifi_map.get(wifi_serial)
            wifi_on = wd is not None and wd["status"] != "offline"
        if not usb_on and not wifi_on:
            continue
        groups.append({"key": usb_serial, "usb_serial": usb_serial,
                        "usb_on": usb_on, "wifi_serial": wifi_serial,
                        "wifi_on": wifi_on, "modelo": modelo})
    return groups

# ─── Janela de Configurações ──────────────────────────────────
class SettingsWindow(tk.Toplevel):
    def __init__(self, parent, cfg, on_save):
        super().__init__(parent)
        self.title("Configurações")
        self.geometry("340x300")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")
        self.grab_set()
        self.cfg    = dict(cfg)
        self.on_save = on_save

        pad = {"padx": 20, "pady": 6}

        def row(label, var, options):
            f = tk.Frame(self, bg="#1e1e1e")
            f.pack(fill="x", **pad)
            tk.Label(f, text=label, font=("Segoe UI", 9),
                     bg="#1e1e1e", fg="#aaaaaa", width=20, anchor="w").pack(side="left")
            for opt in options:
                bg = "#1976d2" if var.get() == opt else "#2d2d2d"
                fg = "#ffffff"
                b  = tk.Button(f, text=opt, font=("Segoe UI", 8, "bold"),
                               bg=bg, fg=fg, relief="flat",
                               padx=8, pady=3, cursor="hand2")
                b.pack(side="left", padx=2)
                b.config(command=lambda v=opt, bref=b, vr=var, opts=options, fr=f:
                         self._select(v, vr, fr, opts))

        self.var_fps  = tk.StringVar(value=self.cfg["fps"])
        self.var_res  = tk.StringVar(value=self.cfg["res"])
        self.var_bit  = tk.StringVar(value=self.cfg["bitrate"])
        self.var_aud  = tk.BooleanVar(value=self.cfg["audio"])

        tk.Label(self, text="Configurações de Transmissão",
                 font=("Segoe UI", 11, "bold"),
                 bg="#1e1e1e", fg="#ffffff").pack(pady=(14, 4))

        row("FPS",       self.var_fps, ["30", "60", "120", "240"])
        row("Resolução", self.var_res, ["540", "720", "1080", "1440"])
        row("Bitrate",   self.var_bit, ["4M", "8M", "16M", "32M"])

        # Áudio
        fa = tk.Frame(self, bg="#1e1e1e")
        fa.pack(fill="x", padx=20, pady=6)
        tk.Label(fa, text="Áudio no Windows", font=("Segoe UI", 9),
                 bg="#1e1e1e", fg="#aaaaaa", width=20, anchor="w").pack(side="left")
        self.chk_audio = tk.Checkbutton(
            fa, variable=self.var_aud,
            bg="#1e1e1e", fg="#ffffff",
            selectcolor="#1976d2",
            activebackground="#1e1e1e",
            text="Ativado", font=("Segoe UI", 9), cursor="hand2")
        self.chk_audio.pack(side="left")

        tk.Button(self, text="Salvar", font=("Segoe UI", 10, "bold"),
                  bg="#1976d2", fg="#ffffff",
                  activebackground="#1565c0",
                  relief="flat", cursor="hand2",
                  padx=20, pady=6,
                  command=self._save).pack(pady=14)

    def _select(self, val, var, frame, opts):
        var.set(val)
        for w in frame.winfo_children():
            if isinstance(w, tk.Button):
                w.config(bg="#1976d2" if w.cget("text") == val else "#2d2d2d")

    def _save(self):
        self.cfg["fps"]     = self.var_fps.get()
        self.cfg["res"]     = self.var_res.get()
        self.cfg["bitrate"] = self.var_bit.get()
        self.cfg["audio"]   = self.var_aud.get()
        save_cfg(self.cfg)
        self.on_save(self.cfg)
        self.destroy()


# ─── App principal ────────────────────────────────────────────
class App(tk.Tk):
    WIN_W = 580
    WIN_H = 460

    def __init__(self):
        super().__init__()
        self.title("My Android")
        self.geometry(f"{self.WIN_W}x{self.WIN_H}")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")

        ico = os.path.join(BASE_DIR, "icone.ico")
        if os.path.exists(ico):
            try: self.iconbitmap(ico)
            except: pass

        self.cfg      = load_cfg()
        self.data     = load_data()
        self._rows    = {}
        self._polling = True

        # Estado da gaveta lateral
        self._drawer_open    = False
        self._drawer_target  = None   # key do device ativo
        self._audio_on       = self.cfg.get("audio", True)

        self._build_ui()
        self._poll()

    # ── UI principal ──────────────────────────────────────────
    def _build_ui(self):
        # Header
        hdr = tk.Frame(self, bg="#111111", pady=10)
        hdr.pack(fill="x")
        tk.Label(hdr, text="My Android",
                 font=("Segoe UI", 15, "bold"),
                 bg="#111111", fg="#ffffff").pack()
        tk.Label(hdr, text="Dispositivos detectados automaticamente",
                 font=("Segoe UI", 8), bg="#111111", fg="#555555").pack()

        # Container principal (lista + gaveta lado a lado)
        self.container = tk.Frame(self, bg="#1e1e1e")
        self.container.pack(fill="both", expand=True)

        # Lista de dispositivos
        self.frame_lista = tk.Frame(self.container, bg="#1e1e1e")
        self.frame_lista.pack(side="left", fill="both",
                              expand=True, padx=16, pady=10)

        # Gaveta lateral (direita) — começa fechada
        self.drawer = tk.Frame(self.container, bg="#161616",
                               width=DRAWER_W)
        self.drawer.pack(side="right", fill="y")
        self.drawer.pack_propagate(False)
        self._build_drawer()

        # Footer
        ftr = tk.Frame(self, bg="#141414", pady=5)
        ftr.pack(fill="x")
        self.lbl_status = tk.Label(ftr, text="Aguardando...",
                                   font=("Segoe UI", 8),
                                   bg="#141414", fg="#444444")
        self.lbl_status.pack(side="left", padx=14)

    # ── Gaveta lateral ────────────────────────────────────────
    def _build_drawer(self):
        """Monta os botões fixos da gaveta lateral."""
        def dbtn(symbol, tooltip, cmd, danger=False):
            fg  = "#e53935" if danger else "#cccccc"
            abg = "#3a1a1a" if danger else "#2a2a2a"
            b = tk.Button(self.drawer, text=symbol,
                          font=("Segoe UI", 14),
                          bg="#161616", fg=fg,
                          activebackground=abg,
                          activeforeground=fg,
                          relief="flat", cursor="hand2",
                          width=3, pady=8, bd=0,
                          command=cmd)
            b.pack(fill="x", pady=1)
            # Tooltip simples
            b.bind("<Enter>", lambda e, t=tooltip, w=b: self._show_tip(t, w))
            b.bind("<Leave>", lambda e: self._hide_tip())
            return b

        # Separador
        def sep():
            tk.Frame(self.drawer, bg="#2a2a2a", height=1).pack(
                fill="x", pady=4, padx=4)

        dbtn("⌨",  "Ativar/Desativar console",  self._toggle_console)
        sep()
        dbtn("📷",  "Tirar print da tela",       self._screenshot)
        sep()
        self.btn_audio = dbtn(
            "🔊", "Áudio: Windows / Celular",   self._toggle_audio)
        sep()
        dbtn("🔉",  "Volume −",                  self._vol_down)
        dbtn("🔊",  "Volume +",                  self._vol_up)
        sep()
        dbtn("◀",  "Voltar",                     self._nav_back)
        dbtn("⏺",  "Home",                       self._nav_home)
        dbtn("▦",   "Recentes",                  self._nav_recents)
        sep()
        dbtn("⚙",  "Configurações",              self._open_settings)

        self._tip_label = None

    def _show_tip(self, text, widget):
        self._hide_tip()
        x = widget.winfo_rootx() - 160
        y = widget.winfo_rooty() + 4
        self._tip_win = tk.Toplevel(self)
        self._tip_win.wm_overrideredirect(True)
        self._tip_win.geometry(f"+{x}+{y}")
        tk.Label(self._tip_win, text=text,
                 font=("Segoe UI", 8),
                 bg="#2d2d2d", fg="#ffffff",
                 padx=8, pady=4).pack()

    def _hide_tip(self):
        if hasattr(self, "_tip_win") and self._tip_win:
            try: self._tip_win.destroy()
            except: pass
            self._tip_win = None

    # ── Ações da gaveta ───────────────────────────────────────
    def _get_active_serial(self):
        """Retorna o serial ativo (USB ou Wi-Fi) do dispositivo selecionado."""
        key = self._drawer_target
        if not key: return None
        row = self._rows.get(key)
        if not row: return None
        g    = row["g"]
        modo = row["modo_var"].get()
        return g["wifi_serial"] if modo == "wifi" else g["usb_serial"]

    def _toggle_console(self):
        bat = os.path.join(BASE_DIR, "auto-usb.bat")
        if not os.path.exists(bat):
            messagebox.showwarning("Aviso", f"Arquivo não encontrado:\n{bat}")
            return
        try:
            subprocess.Popen(
                ["wscript.exe", "/nologo",
                 os.path.join(BASE_DIR, "auto-usb.vbs")],
                creationflags=subprocess.CREATE_NO_WINDOW)
        except Exception as e:
            # Fallback direto
            subprocess.Popen(
                f'cmd /c "{bat}"',
                shell=True,
                creationflags=subprocess.CREATE_NO_WINDOW)

    def _screenshot(self):
        serial = self._get_active_serial()
        if not serial:
            messagebox.showwarning("Aviso", "Nenhum dispositivo selecionado.")
            return
        ts   = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(BASE_DIR, f"print_{ts}.png")
        def run():
            try:
                out = subprocess.check_output(
                    [ADB, "-s", serial, "exec-out", "screencap", "-p"],
                    cwd=BASE_DIR,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                with open(path, "wb") as f:
                    f.write(out)
                self.after(0, lambda: messagebox.showinfo(
                    "Print salvo", f"Salvo em:\n{path}"))
            except Exception as e:
                self.after(0, lambda: messagebox.showerror(
                    "Erro", f"Falha ao tirar print:\n{e}"))
        threading.Thread(target=run, daemon=True).start()

    def _toggle_audio(self):
        self._audio_on = not self._audio_on
        self.cfg["audio"] = self._audio_on
        save_cfg(self.cfg)
        icon = "🔊" if self._audio_on else "🔇"
        self.btn_audio.config(text=icon)

    def _vol_down(self):
        serial = self._get_active_serial()
        if serial:
            threading.Thread(
                target=lambda: run_adb("-s", serial, "shell",
                                       "input keyevent KEYCODE_VOLUME_DOWN"),
                daemon=True).start()

    def _vol_up(self):
        serial = self._get_active_serial()
        if serial:
            threading.Thread(
                target=lambda: run_adb("-s", serial, "shell",
                                       "input keyevent KEYCODE_VOLUME_UP"),
                daemon=True).start()

    def _nav_back(self):
        serial = self._get_active_serial()
        if serial:
            threading.Thread(
                target=lambda: run_adb("-s", serial, "shell",
                                       "input keyevent KEYCODE_BACK"),
                daemon=True).start()

    def _nav_home(self):
        serial = self._get_active_serial()
        if serial:
            threading.Thread(
                target=lambda: run_adb("-s", serial, "shell",
                                       "input keyevent KEYCODE_HOME"),
                daemon=True).start()

    def _nav_recents(self):
        serial = self._get_active_serial()
        if serial:
            threading.Thread(
                target=lambda: run_adb("-s", serial, "shell",
                                       "input keyevent KEYCODE_APP_SWITCH"),
                daemon=True).start()

    def _open_settings(self):
        SettingsWindow(self, self.cfg,
                       on_save=lambda c: setattr(self, "cfg", c))

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
        # Se o target sumiu, pega o primeiro disponível
        if self._drawer_target not in self._rows:
            self._drawer_target = groups[0]["key"] if groups else None

        n = len(groups)
        self.lbl_status.config(
            text=(f"{n} dispositivo{'s' if n!=1 else ''} encontrado{'s' if n!=1 else ''}"
                  if n else "Nenhum dispositivo conectado"))
        self.after(POLL_MS, self._poll)

    # ── Row de dispositivo ────────────────────────────────────
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

        # Define como alvo da gaveta ao clicar no card
        frame.bind("<Button-1>", lambda e, k=key: self._set_target(k))

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

        lbl_usb = tk.Label(info, text="", font=("Segoe UI", 7),
                           bg="#2a2a2a", anchor="w")
        lbl_usb.pack(anchor="w")
        lbl_wifi = tk.Label(info, text="", font=("Segoe UI", 7),
                            bg="#2a2a2a", anchor="w")
        lbl_wifi.pack(anchor="w")

        right    = tk.Frame(frame, bg="#2a2a2a")
        right.pack(side="right", anchor="center")

        modo_var = tk.StringVar(value="usb")

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
            "modo_var": modo_var, "modelo": modelo, "g": g,
            "tem_gaveta": False,
        }

        btn_modo.config(command=lambda k=key: self._abrir_menu(k))
        btn_conectar.config(command=lambda k=key: self._conectar(k))

        # Primeiro device vira target automaticamente
        if self._drawer_target is None:
            self._drawer_target = key

        self._update_row(g)

    def _set_target(self, key):
        self._drawer_target = key
        # Destaca o card selecionado
        for k, row in self._rows.items():
            row["frame"].config(
                highlightbackground="#1976d2" if k == key else "#3a3a3a")

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
                       activebackground="#1a3a1a", activeforeground="#4caf50")
            row["btn_conectar"].config(bg="#1976d2",
                                       activebackground="#1565c0")
        else:
            row["tem_gaveta"] = False
            row["modo_var"].set("wifi"); modo = "wifi"
            btn.config(text="Wi-Fi", state="disabled", cursor="",
                       bg="#1a2a3a", fg="#42a5f5",
                       activebackground="#1a2a3a", activeforeground="#42a5f5")
            row["btn_conectar"].config(bg="#00796b",
                                       activebackground="#00695c")

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

    # ── Conectar ──────────────────────────────────────────────
    def _conectar(self, key):
        self._set_target(key)
        row = self._rows.get(key)
        if not row: return
        g    = row["g"]
        modo = row["modo_var"].get()
        nome = self._get_name(key, row["modelo"])
        btn  = row["btn_conectar"]
        btn.config(state="disabled", text="Aguarde...")

        def run():
            try:
                target = g["wifi_serial"] if modo=="wifi" else g["usb_serial"]
                if not target:
                    self.after(0, messagebox.showerror,
                               "Erro", "Serial não disponível.")
                    return
                tipo = "WI-FI" if modo=="wifi" else "USB"
                cfg  = self.cfg
                cmd  = [SCRCPY, "-s", target,
                        "--window-title", f"{tipo} - {nome}",
                        "--gamepad=uhid",
                        "--print-fps",
                        f"--max-fps={cfg['fps']}",
                        "--stay-awake",
                        f"-m {cfg['res']}",
                        f"-b {cfg['bitrate']}"]
                if not cfg.get("audio", True):
                    cmd.append("--no-audio")
                env = os.environ.copy()
                env["SCRCPY_SERVER_PATH"] = os.path.join(BASE_DIR, "server")
                subprocess.Popen(cmd, cwd=BASE_DIR, env=env,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
            except FileNotFoundError:
                self.after(0, messagebox.showerror, "Erro",
                           f"Arquivo 'base' não encontrado em:\n{BASE_DIR}")
            finally:
                self.after(600, lambda: btn.config(
                    state="normal", text="Conectar"))

        threading.Thread(target=run, daemon=True).start()

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
        self.destroy()


if __name__ == "__main__":
    _mutex = ensure_single_instance()
    app    = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()
