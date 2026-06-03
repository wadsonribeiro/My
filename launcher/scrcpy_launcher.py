import tkinter as tk
from tkinter import messagebox
import subprocess
import threading
import os
import sys
import json
import re
import time

# ─── Configuração ────────────────────────────────────────────
SCRCPY_EXE  = "scrcpy.exe"
ADB_EXE     = "adb.exe"
WIFI_PORT   = 9990
DATA_FILE   = "devices_data.json"   # nomes e IPs salvos
POLL_MS     = 2000                  # intervalo de polling (ms)
# ─────────────────────────────────────────────────────────────

def get_base_dir():
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

BASE_DIR = get_base_dir()
SCRCPY   = os.path.join(BASE_DIR, SCRCPY_EXE)
ADB      = os.path.join(BASE_DIR, ADB_EXE)
DATA_PATH= os.path.join(BASE_DIR, DATA_FILE)

# ─── Persistência ────────────────────────────────────────────
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

# ─── ADB helpers ─────────────────────────────────────────────
def run_adb(*args, timeout=6):
    try:
        result = subprocess.run(
            [ADB, *args],
            capture_output=True, text=True,
            timeout=timeout, cwd=BASE_DIR,
            creationflags=subprocess.CREATE_NO_WINDOW
        )
        return result.stdout.strip()
    except:
        return ""

def listar_dispositivos():
    out = run_adb("devices", "-l")
    dispositivos = []
    for linha in out.splitlines()[1:]:
        linha = linha.strip()
        if not linha:
            continue
        partes = linha.split()
        if len(partes) < 2:
            continue
        serial = partes[0]
        status = partes[1]
        modelo = ""
        for p in partes:
            if p.startswith("model:"):
                modelo = p.replace("model:", "").replace("_", " ")
                break
        if not modelo:
            modelo = serial
        dispositivos.append({"serial": serial, "modelo": modelo, "status": status})
    return dispositivos

def get_ip(serial):
    """Tenta obter o IP Wi-Fi do dispositivo via ifconfig ou ip addr."""
    # Tenta ifconfig wlan0
    out = run_adb("-s", serial, "shell", "ifconfig wlan0")
    m = re.search(r'inet addr:([\d.]+)', out)
    if m:
        return m.group(1)
    # Tenta ip addr (Android mais novo)
    out = run_adb("-s", serial, "shell", "ip addr show wlan0")
    m = re.search(r'inet ([\d.]+)/', out)
    if m:
        return m.group(1)
    return None

def is_wifi_serial(serial):
    """Seriais no formato IP:PORT são conexões Wi-Fi."""
    return bool(re.match(r'^\d+\.\d+\.\d+\.\d+:\d+$', serial))

def conectar_wifi(serial, ip, callback_status):
    """Habilita tcpip e conecta via Wi-Fi. Retorna o novo serial (IP:PORT) ou None."""
    callback_status("Habilitando tcpip...")
    run_adb("-s", serial, "tcpip", str(WIFI_PORT), timeout=8)
    time.sleep(1)
    callback_status("Conectando Wi-Fi...")
    out = run_adb("connect", f"{ip}:{WIFI_PORT}", timeout=8)
    if "connected" in out.lower():
        return f"{ip}:{WIFI_PORT}"
    return None

# ─── App principal ────────────────────────────────────────────
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SCRCPY Launcher")
        self.geometry("540x420")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")
        self.data = load_data()          # {serial_id: {name, ip}}
        self._device_rows = {}           # serial → widgets dict
        self._last_serials = set()
        self._polling = True
        self._build_ui()
        self._poll()

    # ── UI ──────────────────────────────────────────────────
    def _build_ui(self):
        header = tk.Frame(self, bg="#111111", pady=12)
        header.pack(fill="x")
        tk.Label(header, text="SCRCPY Launcher",
                 font=("Segoe UI", 16, "bold"),
                 bg="#111111", fg="#ffffff").pack()
        tk.Label(header, text="Dispositivos detectados automaticamente",
                 font=("Segoe UI", 9),
                 bg="#111111", fg="#666666").pack()

        self.frame_lista = tk.Frame(self, bg="#1e1e1e")
        self.frame_lista.pack(fill="both", expand=True, padx=20, pady=10)

        footer = tk.Frame(self, bg="#1a1a1a", pady=6)
        footer.pack(fill="x")
        self.lbl_status = tk.Label(footer, text="Aguardando dispositivos...",
                                   font=("Segoe UI", 8),
                                   bg="#1a1a1a", fg="#555555")
        self.lbl_status.pack(side="left", padx=16)

    # ── Polling ──────────────────────────────────────────────
    def _poll(self):
        if not self._polling:
            return
        threading.Thread(target=self._fetch_devices, daemon=True).start()

    def _fetch_devices(self):
        devs = listar_dispositivos()
        self.after(0, self._update_ui, devs)

    def _update_ui(self, devs):
        serials_now = {d["serial"] for d in devs}

        # Remove rows de dispositivos que saíram
        for s in list(self._device_rows.keys()):
            if s not in serials_now:
                self._device_rows[s]["frame"].destroy()
                del self._device_rows[s]

        # Adiciona/atualiza rows
        for d in devs:
            serial = d["serial"]
            modelo = d["modelo"]
            status = d["status"]

            # Atualiza IP em background se USB
            if not is_wifi_serial(serial):
                threading.Thread(
                    target=self._update_ip, args=(serial,), daemon=True
                ).start()

            if serial not in self._device_rows:
                self._add_row(serial, modelo, status)
            else:
                self._refresh_row(serial, status)

        count = len(devs)
        txt = f"{count} dispositivo{'s' if count != 1 else ''} encontrado{'s' if count != 1 else ''}" if count else "Nenhum dispositivo conectado"
        self.lbl_status.config(text=txt)
        self._last_serials = serials_now

        # Agenda próximo poll
        self.after(POLL_MS, self._poll)

    def _update_ip(self, serial):
        ip = get_ip(serial)
        if ip:
            entry = self.data.setdefault(serial, {})
            if entry.get("ip") != ip:
                entry["ip"] = ip
                save_data(self.data)
                self.after(0, self._refresh_ip_label, serial, ip)

    def _refresh_ip_label(self, serial, ip):
        row = self._device_rows.get(serial)
        if row and "lbl_ip" in row:
            row["lbl_ip"].config(text=f"IP: {ip}")
            # Atualiza visibilidade do botão gaveta
            self._update_gaveta_btn(serial)

    # ── Row de dispositivo ───────────────────────────────────
    def _get_display_name(self, serial, modelo):
        return self.data.get(serial, {}).get("name", modelo)

    def _add_row(self, serial, modelo, status):
        frame = tk.Frame(self.frame_lista, bg="#2a2a2a",
                         pady=8, padx=12,
                         highlightbackground="#3a3a3a",
                         highlightthickness=1)
        frame.pack(fill="x", pady=3)

        # Coluna esquerda: ícone + info
        left = tk.Frame(frame, bg="#2a2a2a")
        left.pack(side="left", fill="x", expand=True)

        tk.Label(left, text="📱", font=("Segoe UI", 18),
                 bg="#2a2a2a").pack(side="left", padx=(0, 8))

        info = tk.Frame(left, bg="#2a2a2a")
        info.pack(side="left")

        nome_display = self._get_display_name(serial, modelo)

        # Label do nome — clicável para editar
        lbl_nome = tk.Label(info, text=nome_display,
                            font=("Segoe UI", 11, "bold"),
                            bg="#2a2a2a", fg="#ffffff",
                            cursor="hand2", anchor="w")
        lbl_nome.pack(anchor="w")
        lbl_nome.bind("<Button-1>", lambda e, s=serial, m=modelo: self._editar_nome(s, m))

        cor_status = "#4caf50" if status == "device" else "#ff9800"
        status_txt = "Pronto" if status == "device" else status
        lbl_serial = tk.Label(info, text=f"{serial}  •  {status_txt}",
                              font=("Segoe UI", 8),
                              bg="#2a2a2a", fg=cor_status, anchor="w")
        lbl_serial.pack(anchor="w")

        ip_salvo = self.data.get(serial, {}).get("ip", "")
        lbl_ip = tk.Label(info,
                          text=f"IP: {ip_salvo}" if ip_salvo else "",
                          font=("Segoe UI", 7),
                          bg="#2a2a2a", fg="#555555", anchor="w")
        lbl_ip.pack(anchor="w")

        # Coluna direita: botão gaveta + botão conectar
        right = tk.Frame(frame, bg="#2a2a2a")
        right.pack(side="right")

        btn_conectar = tk.Button(
            right, text="Conectar",
            font=("Segoe UI", 9, "bold"),
            bg="#1976d2", fg="#ffffff",
            activebackground="#1565c0", activeforeground="#ffffff",
            relief="flat", cursor="hand2", padx=12, pady=5
        )
        btn_conectar.pack(side="right")

        # Botão gaveta (▼) — só aparece se tiver IP disponível
        btn_gaveta = tk.Button(
            right, text="▼",
            font=("Segoe UI", 8),
            bg="#333333", fg="#aaaaaa",
            activebackground="#444444", activeforeground="#ffffff",
            relief="flat", cursor="hand2", padx=6, pady=5
        )

        # Salva widgets no dict
        self._device_rows[serial] = {
            "frame": frame,
            "lbl_nome": lbl_nome,
            "lbl_serial": lbl_serial,
            "lbl_ip": lbl_ip,
            "btn_conectar": btn_conectar,
            "btn_gaveta": btn_gaveta,
            "modelo": modelo,
            "modo": tk.StringVar(value="usb"),  # "usb" ou "wifi"
        }

        # Configura comandos dos botões
        btn_conectar.config(
            command=lambda s=serial: self._conectar(s)
        )
        btn_gaveta.config(
            command=lambda s=serial: self._toggle_gaveta(s)
        )

        self._update_gaveta_btn(serial)

    def _refresh_row(self, serial, status):
        row = self._device_rows.get(serial)
        if not row:
            return
        cor = "#4caf50" if status == "device" else "#ff9800"
        txt = "Pronto" if status == "device" else status
        row["lbl_serial"].config(fg=cor,
            text=f"{serial}  •  {txt}")

    def _update_gaveta_btn(self, serial):
        """Mostra ou esconde o botão gaveta dependendo se há IP disponível."""
        row = self._device_rows.get(serial)
        if not row:
            return
        ip = self.data.get(serial, {}).get("ip", "")
        btn_gaveta = row["btn_gaveta"]
        if ip and not is_wifi_serial(serial):
            btn_gaveta.pack(side="right", padx=(0, 4), before=row["btn_conectar"])
        else:
            btn_gaveta.pack_forget()

    # ── Gaveta de modo de conexão ────────────────────────────
    def _toggle_gaveta(self, serial):
        row = self._device_rows.get(serial)
        if not row:
            return
        ip = self.data.get(serial, {}).get("ip", "")
        if not ip:
            return

        # Cria menu popup
        menu = tk.Menu(self, tearoff=0, bg="#2d2d2d", fg="#ffffff",
                       activebackground="#1976d2", activeforeground="#ffffff",
                       font=("Segoe UI", 9), bd=0)
        modo_atual = row["modo"].get()

        def set_usb():
            row["modo"].set("usb")
            row["btn_gaveta"].config(text="▼ USB")
            row["btn_conectar"].config(bg="#1976d2")

        def set_wifi():
            row["modo"].set("wifi")
            row["btn_gaveta"].config(text="▼ Wi-Fi")
            row["btn_conectar"].config(bg="#00796b")

        menu.add_command(label="🔌  USB" + (" ✓" if modo_atual == "usb" else ""),
                         command=set_usb)
        menu.add_command(label="📶  Wi-Fi" + (" ✓" if modo_atual == "wifi" else ""),
                         command=set_wifi)

        btn = row["btn_gaveta"]
        menu.tk_popup(btn.winfo_rootx(), btn.winfo_rooty() + btn.winfo_height())

    # ── Conectar ─────────────────────────────────────────────
    def _conectar(self, serial):
        row = self._device_rows.get(serial)
        if not row:
            return
        modo = row["modo"].get()
        btn = row["btn_conectar"]
        modelo = row["modelo"]
        nome = self._get_display_name(serial, modelo)

        btn.config(state="disabled", text="Aguarde...")

        def run():
            try:
                if modo == "wifi":
                    ip = self.data.get(serial, {}).get("ip")
                    if not ip:
                        self.after(0, messagebox.showerror, "Erro",
                                   "IP não disponível para conexão Wi-Fi.\nConecte via USB primeiro.")
                        return

                    def status_cb(msg):
                        self.after(0, btn.config, {"text": msg})

                    wifi_serial = conectar_wifi(serial, ip, status_cb)
                    if not wifi_serial:
                        self.after(0, messagebox.showerror, "Erro Wi-Fi",
                                   f"Não foi possível conectar via Wi-Fi em {ip}:{WIFI_PORT}")
                        return
                    target = wifi_serial
                else:
                    target = serial

                subprocess.Popen(
                    [SCRCPY, "-s", target, "--window-title", f"USB - {nome}"],
                    cwd=BASE_DIR,
                    creationflags=subprocess.CREATE_NO_WINDOW
                )
            except FileNotFoundError:
                self.after(0, messagebox.showerror, "Erro",
                           f"scrcpy.exe não encontrado em:\n{SCRCPY}")
            finally:
                self.after(500, lambda: btn.config(state="normal", text="Conectar"))

        threading.Thread(target=run, daemon=True).start()

    # ── Editar nome ──────────────────────────────────────────
    def _editar_nome(self, serial, modelo):
        row = self._device_rows.get(serial)
        if not row:
            return

        nome_atual = self._get_display_name(serial, modelo)
        lbl = row["lbl_nome"]

        # Substitui label por Entry
        lbl.pack_forget()
        entry = tk.Entry(lbl.master,
                         font=("Segoe UI", 11, "bold"),
                         bg="#3a3a3a", fg="#ffffff",
                         insertbackground="#ffffff",
                         relief="flat", width=20)
        entry.insert(0, nome_atual)
        entry.pack(anchor="w")
        entry.focus_set()
        entry.select_range(0, tk.END)

        def salvar(event=None):
            novo_nome = entry.get().strip()
            if not novo_nome:
                novo_nome = modelo
            self.data.setdefault(serial, {})["name"] = novo_nome
            save_data(self.data)
            entry.destroy()
            lbl.config(text=novo_nome)
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
