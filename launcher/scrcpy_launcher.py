import tkinter as tk
from tkinter import messagebox
import subprocess
import threading
import os, sys, json, re, time

# ─── Configuração ────────────────────────────────────────────
SCRCPY_EXE = "scrcpy.exe"
ADB_EXE    = "adb.exe"
WIFI_PORT  = 9990
DATA_FILE  = "devices_data.json"
POLL_MS    = 2500
# ─────────────────────────────────────────────────────────────

def get_base_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

BASE_DIR  = get_base_dir()
SCRCPY    = os.path.join(BASE_DIR, SCRCPY_EXE)
ADB       = os.path.join(BASE_DIR, ADB_EXE)
DATA_PATH = os.path.join(BASE_DIR, DATA_FILE)

# ─── Persistência ─────────────────────────────────────────────
def load_data():
    if os.path.exists(DATA_PATH):
        try:
            with open(DATA_PATH, "r", encoding="utf-8") as f:
                return json.load(f)
        except:
            pass
    return {}

def save_data(data):
    try:
        with open(DATA_PATH, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
    except:
        pass

# ─── ADB helpers ──────────────────────────────────────────────
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

def is_wifi_serial(serial):
    return bool(re.match(r'^\d+\.\d+\.\d+\.\d+:\d+$', serial))

def get_ip_from_device(serial):
    out = run_adb("-s", serial, "shell", "ifconfig wlan0")
    m = re.search(r'inet addr:([\d.]+)', out)
    if m: return m.group(1)
    out = run_adb("-s", serial, "shell", "ip addr show wlan0")
    m = re.search(r'inet ([\d.]+)/', out)
    if m: return m.group(1)
    return None

def fetch_all_devices():
    """
    Retorna lista de raw devices do adb devices -l
    Formato: [{"serial": ..., "modelo": ..., "status": ...}]
    """
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

def group_devices(raw_devs, data):
    """
    Agrupa dispositivos USB + Wi-Fi do mesmo aparelho.
    Usa devices_data.json para saber qual IP pertence a qual serial USB.

    Retorna lista de grupos:
    {
      "id":       serial USB (ou IP se só Wi-Fi),
      "modelo":   str,
      "usb_serial":  str | None,
      "usb_on":      bool,
      "wifi_serial": str | None,   (ex: "192.168.18.61:9990")
      "wifi_on":     bool,
    }
    """
    usb_devs  = {d["serial"]: d for d in raw_devs if not is_wifi_serial(d["serial"])}
    wifi_devs = {d["serial"]: d for d in raw_devs if  is_wifi_serial(d["serial"])}

    groups = {}

    # Processa USB primeiro
    for serial, d in usb_devs.items():
        ip = data.get(serial, {}).get("ip", "")
        wifi_serial = f"{ip}:{WIFI_PORT}" if ip else None
        wifi_on = wifi_serial in wifi_devs and wifi_devs[wifi_serial]["status"] != "offline"

        groups[serial] = {
            "id":          serial,
            "modelo":      d["modelo"],
            "usb_serial":  serial,
            "usb_on":      d["status"] == "device",
            "wifi_serial": wifi_serial,
            "wifi_on":     wifi_on,
        }
        # Marca esse wi-fi como "consumido"
        if wifi_serial and wifi_serial in wifi_devs:
            wifi_devs[wifi_serial]["_consumed"] = True

    # Wi-Fi não vinculado a nenhum USB conhecido
    for serial, d in wifi_devs.items():
        if d.get("_consumed"): continue
        # Tenta achar no data pelo IP
        ip_part = serial.rsplit(":", 1)[0]
        usb_owner = None
        for uid, udata in data.items():
            if udata.get("ip") == ip_part:
                usb_owner = uid
                break
        if usb_owner and usb_owner in groups:
            # Atualiza grupo existente
            groups[usb_owner]["wifi_serial"] = serial
            groups[usb_owner]["wifi_on"] = d["status"] != "offline"
        else:
            # Dispositivo só Wi-Fi (USB foi desconectado)
            nome = data.get(usb_owner or serial, {}).get("name", d["modelo"])
            groups[serial] = {
                "id":          usb_owner or serial,
                "modelo":      d["modelo"],
                "usb_serial":  None,
                "usb_on":      False,
                "wifi_serial": serial,
                "wifi_on":     d["status"] != "offline",
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
        self.data = load_data()
        self._rows = {}          # group_id → widgets
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

    # ── Polling ──────────────────────────────────────────────
    def _poll(self):
        if not self._polling: return
        threading.Thread(target=self._fetch, daemon=True).start()

    def _fetch(self):
        raw   = fetch_all_devices()
        # Atualiza IPs de dispositivos USB em background
        for d in raw:
            if not is_wifi_serial(d["serial"]) and d["status"] == "device":
                ip = get_ip_from_device(d["serial"])
                if ip:
                    entry = self.data.setdefault(d["serial"], {})
                    if entry.get("ip") != ip:
                        entry["ip"] = ip
                        save_data(self.data)
                        # Conecta automaticamente ao Wi-Fi para deixar o serial disponível
                        wifi_serial = f"{ip}:{WIFI_PORT}"
                        run_adb("-s", d["serial"], "tcpip", str(WIFI_PORT))
                        time.sleep(0.5)
                        run_adb("connect", wifi_serial)

        groups = group_devices(raw, self.data)
        self.after(0, self._update_ui, groups)

    def _update_ui(self, groups):
        ids_now = {g["id"] for g in groups}

        # Remove rows ausentes
        for gid in list(self._rows.keys()):
            if gid not in ids_now:
                self._rows[gid]["frame"].destroy()
                del self._rows[gid]

        # Adiciona ou atualiza
        for g in groups:
            if g["id"] not in self._rows:
                self._build_row(g)
            else:
                self._update_row(g)

        n = len(groups)
        self.lbl_status.config(
            text=f"{n} dispositivo{'s' if n!=1 else ''} encontrado{'s' if n!=1 else ''}" if n
                 else "Nenhum dispositivo conectado")

        self.after(POLL_MS, self._poll)

    # ── Row ──────────────────────────────────────────────────
    def _get_name(self, gid, modelo):
        return self.data.get(gid, {}).get("name", modelo)

    def _build_row(self, g):
        gid    = g["id"]
        modelo = g["modelo"]

        frame = tk.Frame(self.frame_lista, bg="#2a2a2a",
                         pady=8, padx=12,
                         highlightbackground="#3a3a3a",
                         highlightthickness=1)
        frame.pack(fill="x", pady=3)

        # ── Esquerda: ícone + info ──
        left = tk.Frame(frame, bg="#2a2a2a")
        left.pack(side="left", fill="x", expand=True)

        tk.Label(left, text="📱", font=("Segoe UI", 18),
                 bg="#2a2a2a").pack(side="left", padx=(0, 8))

        info = tk.Frame(left, bg="#2a2a2a")
        info.pack(side="left")

        nome = self._get_name(gid, modelo)
        lbl_nome = tk.Label(info, text=nome,
                            font=("Segoe UI", 11, "bold"),
                            bg="#2a2a2a", fg="#ffffff",
                            cursor="hand2", anchor="w")
        lbl_nome.pack(anchor="w")
        lbl_nome.bind("<Button-1>",
                      lambda e, gi=gid, mo=modelo: self._editar_nome(gi, mo))

        lbl_usb = tk.Label(info, text="", font=("Segoe UI", 8),
                           bg="#2a2a2a", anchor="w")
        lbl_usb.pack(anchor="w")

        lbl_wifi = tk.Label(info, text="", font=("Segoe UI", 8),
                            bg="#2a2a2a", anchor="w")
        lbl_wifi.pack(anchor="w")

        # ── Direita: gaveta + conectar ──
        right = tk.Frame(frame, bg="#2a2a2a")
        right.pack(side="right", anchor="center")

        # Modo selecionado: "usb" ou "wifi"
        modo_var = tk.StringVar(value="usb")

        # Botão de modo (mostra o modo atual, clique abre menu)
        btn_modo = tk.Button(right, text="USB ▾",
                             font=("Segoe UI", 8, "bold"),
                             bg="#333333", fg="#aaaaaa",
                             activebackground="#444444",
                             activeforeground="#ffffff",
                             relief="flat", cursor="hand2",
                             padx=8, pady=5)
        btn_modo.pack(side="left", padx=(0, 4))

        btn_conectar = tk.Button(right, text="Conectar",
                                 font=("Segoe UI", 9, "bold"),
                                 bg="#1976d2", fg="#ffffff",
                                 activebackground="#1565c0",
                                 activeforeground="#ffffff",
                                 relief="flat", cursor="hand2",
                                 padx=12, pady=5)
        btn_conectar.pack(side="left")

        self._rows[gid] = {
            "frame": frame, "lbl_nome": lbl_nome,
            "lbl_usb": lbl_usb, "lbl_wifi": lbl_wifi,
            "btn_modo": btn_modo, "btn_conectar": btn_conectar,
            "modo_var": modo_var, "modelo": modelo,
        }

        btn_modo.config(command=lambda gi=gid: self._abrir_menu_modo(gi))
        btn_conectar.config(command=lambda gi=gid: self._conectar(gi))

        self._update_row(g)

    def _update_row(self, g):
        gid  = g["id"]
        row  = self._rows.get(gid)
        if not row: return

        # Labels de serial
        if g["usb_serial"]:
            cor = "#4caf50" if g["usb_on"] else "#666666"
            st  = "ON" if g["usb_on"] else "OFF"
            row["lbl_usb"].config(
                text=f"🔌 {g['usb_serial']}  •  {st}", fg=cor)
        else:
            row["lbl_usb"].config(text="")

        if g["wifi_serial"]:
            cor = "#4caf50" if g["wifi_on"] else "#666666"
            st  = "ON" if g["wifi_on"] else "OFF"
            row["lbl_wifi"].config(
                text=f"📶 {g['wifi_serial']}  •  {st}", fg=cor)
        else:
            row["lbl_wifi"].config(text="")

        # Botão de modo
        modo = row["modo_var"].get()
        usb_ok  = g["usb_on"]  and g["usb_serial"]
        wifi_ok = g["wifi_on"] and g["wifi_serial"]

        # Se o modo atual ficou indisponível, muda automaticamente
        if modo == "usb" and not usb_ok and wifi_ok:
            row["modo_var"].set("wifi")
            modo = "wifi"
        elif modo == "wifi" and not wifi_ok and usb_ok:
            row["modo_var"].set("usb")
            modo = "usb"

        # Aparência do botão de modo
        if wifi_ok:
            # Tem Wi-Fi disponível → mostra gaveta
            lbl = "USB ▾" if modo == "usb" else "Wi-Fi ▾"
            row["btn_modo"].config(text=lbl, state="normal",
                                   fg="#aaaaaa", bg="#333333")
        else:
            # Só USB → mostra "USB" sem gaveta, desabilitado visualmente
            row["btn_modo"].config(text="USB", state="disabled",
                                   fg="#555555", bg="#2a2a2a")

        # Cor do botão conectar
        if modo == "wifi":
            row["btn_conectar"].config(bg="#00796b",
                                       activebackground="#00695c")
        else:
            row["btn_conectar"].config(bg="#1976d2",
                                       activebackground="#1565c0")

    # ── Menu de modo ─────────────────────────────────────────
    def _abrir_menu_modo(self, gid):
        row = self._rows.get(gid)
        if not row: return

        # Busca grupo atual
        g = self._get_group_by_id(gid)
        if not g: return

        menu = tk.Menu(self, tearoff=0, bg="#2d2d2d", fg="#ffffff",
                       activebackground="#1976d2",
                       activeforeground="#ffffff",
                       font=("Segoe UI", 9), bd=0)

        modo = row["modo_var"].get()

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

        if g["usb_on"] and g["usb_serial"]:
            menu.add_command(
                label="🔌  USB" + ("  ✓" if modo == "usb" else ""),
                command=set_usb)

        if g["wifi_on"] and g["wifi_serial"]:
            menu.add_command(
                label="📶  Wi-Fi" + ("  ✓" if modo == "wifi" else ""),
                command=set_wifi)

        btn = row["btn_modo"]
        menu.tk_popup(btn.winfo_rootx(),
                      btn.winfo_rooty() + btn.winfo_height())

    def _get_group_by_id(self, gid):
        """Reconstrói o grupo a partir dos dados salvos — usado só para o menu."""
        # Lê o último estado da row
        row = self._rows.get(gid)
        if not row: return None
        usb_txt  = row["lbl_usb"].cget("text")
        wifi_txt = row["lbl_wifi"].cget("text")
        return {
            "id": gid,
            "usb_serial":  gid if "ON" in usb_txt else None,
            "usb_on":      "ON" in usb_txt,
            "wifi_serial": self.data.get(gid, {}).get("ip", "") + f":{WIFI_PORT}"
                           if "ON" in wifi_txt else None,
            "wifi_on":     "ON" in wifi_txt,
        }

    # ── Conectar ─────────────────────────────────────────────
    def _conectar(self, gid):
        row = self._rows.get(gid)
        if not row: return
        modo  = row["modo_var"].get()
        nome  = self._get_name(gid, row["modelo"])
        btn   = row["btn_conectar"]
        btn.config(state="disabled", text="Aguarde...")

        def run():
            try:
                if modo == "wifi":
                    ip = self.data.get(gid, {}).get("ip")
                    if not ip:
                        self.after(0, messagebox.showerror, "Erro Wi-Fi",
                                   "IP não disponível.")
                        return
                    target = f"{ip}:{WIFI_PORT}"
                else:
                    target = gid   # serial USB

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

    # ── Editar nome ──────────────────────────────────────────
    def _editar_nome(self, gid, modelo):
        row = self._rows.get(gid)
        if not row: return
        lbl = row["lbl_nome"]
        nome_atual = self._get_name(gid, modelo)

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
            self.data.setdefault(gid, {})["name"] = novo
            save_data(self.data)
            entry.destroy()
            lbl.config(text=novo)
            lbl.pack(anchor="w")

        def cancelar(event=None):
            entry.destroy()
            lbl.pack(anchor="w")

        entry.bind("<Return>", salvar)
        entry.bind("<Escape>", cancelar)
        entry.bind("<FocusOut>", salvar)

    def on_close(self):
        self._polling = False
        self.destroy()

if __name__ == "__main__":
    app = App()
    app.protocol("WM_DELETE_WINDOW", app.on_close)
    app.mainloop()
