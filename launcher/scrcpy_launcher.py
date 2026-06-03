import tkinter as tk
from tkinter import messagebox
import subprocess
import threading
import os, sys, json, re, time

# ─── Configuração ─────────────────────────────────────────────
SCRCPY_EXE = "scrcpy.exe"
ADB_EXE    = "adb.exe"
WIFI_PORT  = 9990
DATA_FILE  = "devices_data.json"
POLL_MS    = 2500
# ──────────────────────────────────────────────────────────────

def get_base_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

BASE_DIR  = get_base_dir()
SCRCPY    = os.path.join(BASE_DIR, SCRCPY_EXE)
ADB       = os.path.join(BASE_DIR, ADB_EXE)
DATA_PATH = os.path.join(BASE_DIR, DATA_FILE)

# ─── JSON ─────────────────────────────────────────────────────
# Estrutura:
# {
#   "<usb_serial>": {
#     "name": "Meu Celular",
#     "wifi_serial": "192.168.1.10:9990",
#     "_modelo": "M2102J20SG"
#   }
# }

def load_data():
    if os.path.exists(DATA_PATH):
        try:
            with open(DATA_PATH, "r", encoding="utf-8") as f:
                raw = json.load(f)
            # Remove chaves antigas que eram IPs (migração)
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
            creationflags=subprocess.CREATE_NO_WINDOW
        )
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
    out = run_adb("devices", "-l")
    devs = []
    for linha in out.splitlines()[1:]:
        linha = linha.strip()
        if not linha: continue
        partes = linha.split()
        if len(partes) < 2: continue
        serial = partes[0]
        status = partes[1]
        modelo = ""
        for p in partes:
            if p.startswith("model:"):
                modelo = p.replace("model:", "").replace("_", " ")
                break
        if not modelo:
            modelo = serial
        devs.append({"serial": serial, "modelo": modelo, "status": status})
    return devs

# ─── Lógica de agrupamento ────────────────────────────────────
def build_groups(raw_devs, data):
    """
    Regras:
    - Chave do grupo = sempre o serial USB (nunca IP)
    - Só exibe grupo se usb_on OR wifi_on
    - Wi-Fi serial órfão (sem USB dono na lista atual) só aparece
      se o USB dono não estiver ON na sessão atual
    """
    usb_map  = {}
    wifi_map = {}

    for d in raw_devs:
        if is_wifi_serial(d["serial"]):
            wifi_map[d["serial"]] = d
        else:
            usb_map[d["serial"]] = d

    groups    = {}
    used_wifi = set()

    # Grupos com USB presente
    for usb_serial, d in usb_map.items():
        usb_on      = d["status"] == "device"
        wifi_serial = data.get(usb_serial, {}).get("wifi_serial")
        wifi_on     = False

        if wifi_serial:
            wd = wifi_map.get(wifi_serial)
            if wd:
                wifi_on = wd["status"] != "offline"
                used_wifi.add(wifi_serial)

        if not usb_on and not wifi_on:
            continue

        groups[usb_serial] = {
            "key":         usb_serial,
            "usb_serial":  usb_serial,
            "usb_on":      usb_on,
            "wifi_serial": wifi_serial,
            "wifi_on":     wifi_on,
            "modelo":      d["modelo"],
        }

    # Wi-Fi órfãos — só inclui se o USB dono NÃO está ON agora
    for ws, wd in wifi_map.items():
        if ws in used_wifi: continue
        if wd["status"] == "offline": continue

        # Acha o USB dono pelo data
        usb_owner = None
        for uid, udata in data.items():
            if udata.get("wifi_serial") == ws:
                usb_owner = uid
                break

        # Se o USB dono está ON agora, ignora este Wi-Fi órfão
        # (significa que o IP mudou — o USB já está sendo tratado)
        if usb_owner and usb_owner in usb_map and usb_map[usb_owner]["status"] == "device":
            continue

        key    = usb_owner if usb_owner else ws
        modelo = data.get(usb_owner or "", {}).get("_modelo", ws)

        groups[key] = {
            "key":         key,
            "usb_serial":  usb_owner,
            "usb_on":      False,
            "wifi_serial": ws,
            "wifi_on":     True,
            "modelo":      modelo,
        }

    return list(groups.values())


# ─── App ──────────────────────────────────────────────────────
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SCRCPY Launcher")
        self.geometry("540x440")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")
        self.data     = load_data()
        self._rows    = {}
        self._polling = True
        self._build_ui()
        self._poll()

    def _build_ui(self):
        hdr = tk.Frame(self, bg="#111111", pady=12)
        hdr.pack(fill="x")
        tk.Label(hdr, text="SCRCPY Launcher",
                 font=("Segoe UI", 16, "bold"),
                 bg="#111111", fg="#ffffff").pack()
        tk.Label(hdr, text="Dispositivos detectados automaticamente",
                 font=("Segoe UI", 9), bg="#111111", fg="#555555").pack()

        self.frame_lista = tk.Frame(self, bg="#1e1e1e")
        self.frame_lista.pack(fill="both", expand=True, padx=20, pady=10)

        ftr = tk.Frame(self, bg="#1a1a1a", pady=6)
        ftr.pack(fill="x")
        self.lbl_status = tk.Label(ftr, text="Aguardando...",
                                   font=("Segoe UI", 8),
                                   bg="#1a1a1a", fg="#444444")
        self.lbl_status.pack(side="left", padx=16)

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

            ip = get_ip_from_device(usb)
            if not ip: continue

            new_wifi_serial = f"{ip}:{WIFI_PORT}"
            entry           = self.data.setdefault(usb, {})
            old_wifi_serial = entry.get("wifi_serial")

            # Salva modelo
            entry["_modelo"] = d["modelo"]

            if old_wifi_serial != new_wifi_serial:
                # IP mudou — desconecta o antigo e conecta o novo
                if old_wifi_serial:
                    run_adb("disconnect", old_wifi_serial)
                entry["wifi_serial"] = new_wifi_serial
                save_data(self.data)
                run_adb("-s", usb, "tcpip", str(WIFI_PORT))
                time.sleep(0.6)
                run_adb("connect", new_wifi_serial)
            else:
                # Mesmo IP — garante que está conectado
                run_adb("connect", new_wifi_serial)

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
                         pady=8, padx=12,
                         highlightbackground="#3a3a3a",
                         highlightthickness=1)
        frame.pack(fill="x", pady=3)

        left = tk.Frame(frame, bg="#2a2a2a")
        left.pack(side="left", fill="x", expand=True)

        tk.Label(left, text="📱", font=("Segoe UI", 18),
                 bg="#2a2a2a").pack(side="left", padx=(0, 8))

        info = tk.Frame(left, bg="#2a2a2a")
        info.pack(side="left")

        nome = self._get_name(key, modelo)
        lbl_nome = tk.Label(info, text=nome,
                            font=("Segoe UI", 11, "bold"),
                            bg="#2a2a2a", fg="#ffffff",
                            cursor="hand2", anchor="w")
        lbl_nome.pack(anchor="w")
        lbl_nome.bind("<Button-1>",
                      lambda e, k=key, m=modelo: self._editar_nome(k, m))

        lbl_usb = tk.Label(info, text="", font=("Segoe UI", 8),
                           bg="#2a2a2a", anchor="w")
        lbl_usb.pack(anchor="w")

        lbl_wifi = tk.Label(info, text="", font=("Segoe UI", 8),
                            bg="#2a2a2a", anchor="w")
        lbl_wifi.pack(anchor="w")

        right = tk.Frame(frame, bg="#2a2a2a")
        right.pack(side="right", anchor="center")

        modo_var = tk.StringVar(value="usb")

        btn_modo = tk.Button(right, text="USB",
                             font=("Segoe UI", 8, "bold"),
                             bg="#2a2a2a", fg="#555555",
                             relief="flat", padx=8, pady=5,
                             state="disabled")
        btn_modo.pack(side="left", padx=(0, 4))

        btn_conectar = tk.Button(right, text="Conectar",
                                 font=("Segoe UI", 9, "bold"),
                                 bg="#1976d2", fg="#ffffff",
                                 activebackground="#1565c0",
                                 activeforeground="#ffffff",
                                 relief="flat", cursor="hand2",
                                 padx=12, pady=5)
        btn_conectar.pack(side="left")

        self._rows[key] = {
            "frame": frame, "lbl_nome": lbl_nome,
            "lbl_usb": lbl_usb, "lbl_wifi": lbl_wifi,
            "btn_modo": btn_modo, "btn_conectar": btn_conectar,
            "modo_var": modo_var, "modelo": modelo, "g": g,
        }

        btn_modo.config(command=lambda k=key: self._abrir_menu(k))
        btn_conectar.config(command=lambda k=key: self._conectar(k))

        self._update_row(g)

    def _update_row(self, g):
        key = g["key"]
        row = self._rows.get(key)
        if not row: return
        row["g"] = g

        # Label USB
        if g["usb_serial"]:
            cor = "#4caf50" if g["usb_on"] else "#666666"
            row["lbl_usb"].config(
                text=f"🔌 {g['usb_serial']}  •  {'ON' if g['usb_on'] else 'OFF'}",
                fg=cor)
        else:
            row["lbl_usb"].config(text="")

        # Label Wi-Fi — só mostra se wifi_serial conhecido
        if g["wifi_serial"]:
            cor = "#4caf50" if g["wifi_on"] else "#666666"
            row["lbl_wifi"].config(
                text=f"📶 {g['wifi_serial']}  •  {'ON' if g['wifi_on'] else 'OFF'}",
                fg=cor)
        else:
            row["lbl_wifi"].config(text="")

        usb_on  = g["usb_on"]
        wifi_on = g["wifi_on"]
        modo    = row["modo_var"].get()

        # Ajuste automático
        if modo == "usb" and not usb_on and wifi_on:
            modo = "wifi"; row["modo_var"].set("wifi")
        elif modo == "wifi" and not wifi_on and usb_on:
            modo = "usb";  row["modo_var"].set("usb")

        btn = row["btn_modo"]

        if usb_on and wifi_on:
            lbl = "USB ▾" if modo == "usb" else "Wi-Fi ▾"
            btn.config(text=lbl, state="normal", cursor="hand2",
                       fg="#aaaaaa", bg="#333333",
                       activebackground="#444444",
                       activeforeground="#ffffff")
        elif usb_on:
            btn.config(text="USB", state="disabled",
                       cursor="", fg="#555555", bg="#2a2a2a")
            row["modo_var"].set("usb"); modo = "usb"
        else:
            btn.config(text="Wi-Fi", state="disabled",
                       cursor="", fg="#555555", bg="#2a2a2a")
            row["modo_var"].set("wifi"); modo = "wifi"

        if modo == "wifi":
            row["btn_conectar"].config(bg="#00796b",
                                       activebackground="#00695c")
        else:
            row["btn_conectar"].config(bg="#1976d2",
                                       activebackground="#1565c0")

    # ── Menu gaveta ───────────────────────────────────────────
    def _abrir_menu(self, key):
        row = self._rows.get(key)
        if not row: return
        g    = row["g"]
        modo = row["modo_var"].get()

        menu = tk.Menu(self, tearoff=0, bg="#2d2d2d", fg="#ffffff",
                       activebackground="#1976d2",
                       activeforeground="#ffffff",
                       font=("Segoe UI", 9), bd=0)

        def set_usb():
            row["modo_var"].set("usb")
            row["btn_modo"].config(text="USB ▾")
            row["btn_conectar"].config(bg="#1976d2",
                                       activebackground="#1565c0")

        def set_wifi():
            row["modo_var"].set("wifi")
            row["btn_modo"].config(text="Wi-Fi ▾")
            row["btn_conectar"].config(bg="#00796b",
                                       activebackground="#00695c")

        if g["usb_on"]:
            menu.add_command(
                label="🔌  USB" + ("  ✓" if modo == "usb" else ""),
                command=set_usb)
        if g["wifi_on"]:
            menu.add_command(
                label="📶  Wi-Fi" + ("  ✓" if modo == "wifi" else ""),
                command=set_wifi)

        btn = row["btn_modo"]
        menu.tk_popup(btn.winfo_rootx(),
                      btn.winfo_rooty() + btn.winfo_height())

    # ── Conectar ──────────────────────────────────────────────
    def _conectar(self, key):
        row = self._rows.get(key)
        if not row: return
        g    = row["g"]
        modo = row["modo_var"].get()
        nome = self._get_name(key, row["modelo"])
        btn  = row["btn_conectar"]
        btn.config(state="disabled", text="Aguarde...")

        def run():
            try:
                target = g["wifi_serial"] if modo == "wifi" else g["usb_serial"]
                if not target:
                    self.after(0, messagebox.showerror, "Erro",
                               "Serial não disponível.")
                    return
                subprocess.Popen(
                    [SCRCPY, "-s", target,
                     "--window-title", f"USB - {nome}"],
                    cwd=BASE_DIR,
                    creationflags=subprocess.CREATE_NO_WINDOW
                )
            except FileNotFoundError:
                self.after(0, messagebox.showerror, "Erro",
                           f"scrcpy.exe não encontrado em:\n{SCRCPY}")
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
        entry = tk.Entry(lbl.master,
                         font=("Segoe UI", 11, "bold"),
                         bg="#3a3a3a", fg="#ffffff",
                         insertbackground="#ffffff",
                         relief="flat", width=22)
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
    app = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()
