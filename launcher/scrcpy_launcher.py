import tkinter as tk
from tkinter import ttk, messagebox
import subprocess
import threading
import os
import sys

# ─── Configuração ────────────────────────────────────────────
SCRCPY_EXE = "scrcpy.exe"   # deve estar na mesma pasta
ADB_EXE    = "adb.exe"      # deve estar na mesma pasta
# ─────────────────────────────────────────────────────────────

def get_base_dir():
    """Retorna a pasta onde o executável/script está."""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))

BASE_DIR   = get_base_dir()
SCRCPY     = os.path.join(BASE_DIR, SCRCPY_EXE)
ADB        = os.path.join(BASE_DIR, ADB_EXE)

def listar_dispositivos():
    """Retorna lista de (serial, modelo, status)."""
    try:
        result = subprocess.run(
            [ADB, "devices", "-l"],
            capture_output=True, text=True, timeout=5,
            cwd=BASE_DIR
        )
        linhas = result.stdout.strip().splitlines()
        dispositivos = []
        for linha in linhas[1:]:  # pula "List of devices attached"
            linha = linha.strip()
            if not linha:
                continue
            partes = linha.split()
            if len(partes) < 2:
                continue
            serial = partes[0]
            status = partes[1]
            # Tenta pegar o model
            modelo = ""
            for parte in partes:
                if parte.startswith("model:"):
                    modelo = parte.replace("model:", "").replace("_", " ")
                    break
            if not modelo:
                modelo = serial
            dispositivos.append((serial, modelo, status))
        return dispositivos
    except Exception as e:
        return []

def conectar(serial, modelo, btn):
    """Abre o scrcpy para o dispositivo selecionado."""
    btn.config(state="disabled", text="Conectando...")
    def run():
        try:
            subprocess.Popen(
                [SCRCPY, "-s", serial, "--window-title", f"USB - {modelo}"],
                cwd=BASE_DIR
            )
        except FileNotFoundError:
            messagebox.showerror(
                "Erro",
                f"scrcpy.exe não encontrado em:\n{SCRCPY}\n\n"
                "Coloque o launcher na mesma pasta do scrcpy.exe"
            )
        finally:
            btn.config(state="normal", text="Conectar")
    threading.Thread(target=run, daemon=True).start()

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("SCRCPY Launcher")
        self.geometry("520x400")
        self.resizable(False, False)
        self.configure(bg="#1e1e1e")
        self._build_ui()
        self._atualizar()

    def _build_ui(self):
        # ── Cabeçalho ──
        header = tk.Frame(self, bg="#111111", pady=12)
        header.pack(fill="x")
        tk.Label(
            header, text="SCRCPY Launcher",
            font=("Segoe UI", 16, "bold"),
            bg="#111111", fg="#ffffff"
        ).pack()
        tk.Label(
            header, text="Selecione um dispositivo e clique em Conectar",
            font=("Segoe UI", 9),
            bg="#111111", fg="#888888"
        ).pack()

        # ── Área de dispositivos ──
        self.frame_lista = tk.Frame(self, bg="#1e1e1e")
        self.frame_lista.pack(fill="both", expand=True, padx=20, pady=10)

        # ── Rodapé com botão Atualizar ──
        footer = tk.Frame(self, bg="#1e1e1e", pady=8)
        footer.pack(fill="x")
        self.lbl_status = tk.Label(
            footer, text="", font=("Segoe UI", 8),
            bg="#1e1e1e", fg="#555555"
        )
        self.lbl_status.pack(side="left", padx=20)
        btn_refresh = tk.Button(
            footer, text="⟳  Atualizar",
            font=("Segoe UI", 9, "bold"),
            bg="#2d2d2d", fg="#ffffff",
            activebackground="#3d3d3d", activeforeground="#ffffff",
            relief="flat", cursor="hand2", padx=12, pady=4,
            command=self._atualizar
        )
        btn_refresh.pack(side="right", padx=20)

    def _limpar_lista(self):
        for widget in self.frame_lista.winfo_children():
            widget.destroy()

    def _atualizar(self):
        self._limpar_lista()
        self.lbl_status.config(text="Buscando dispositivos...")
        self.update()

        dispositivos = listar_dispositivos()

        self._limpar_lista()

        if not dispositivos:
            tk.Label(
                self.frame_lista,
                text="Nenhum dispositivo encontrado.\n\n"
                     "Verifique se o USB Debugging está ativado\n"
                     "e o cabo está conectado.",
                font=("Segoe UI", 10),
                bg="#1e1e1e", fg="#888888",
                justify="center"
            ).pack(expand=True, pady=40)
            self.lbl_status.config(text="0 dispositivos encontrados")
            return

        for serial, modelo, status in dispositivos:
            self._add_device_row(serial, modelo, status)

        count = len(dispositivos)
        self.lbl_status.config(
            text=f"{count} dispositivo{'s' if count > 1 else ''} encontrado{'s' if count > 1 else ''}"
        )

    def _add_device_row(self, serial, modelo, status):
        """Adiciona uma linha de dispositivo na lista."""
        row = tk.Frame(
            self.frame_lista,
            bg="#2a2a2a", pady=10, padx=14,
            highlightbackground="#3a3a3a",
            highlightthickness=1
        )
        row.pack(fill="x", pady=4)

        # Ícone + info
        info = tk.Frame(row, bg="#2a2a2a")
        info.pack(side="left", fill="x", expand=True)

        tk.Label(
            info, text="📱",
            font=("Segoe UI", 18),
            bg="#2a2a2a"
        ).pack(side="left", padx=(0, 10))

        txt = tk.Frame(info, bg="#2a2a2a")
        txt.pack(side="left")

        tk.Label(
            txt, text=modelo,
            font=("Segoe UI", 11, "bold"),
            bg="#2a2a2a", fg="#ffffff",
            anchor="w"
        ).pack(anchor="w")

        cor_status = "#4caf50" if status == "device" else "#ff9800"
        status_texto = "Pronto" if status == "device" else status
        tk.Label(
            txt, text=f"{serial}  •  {status_texto}",
            font=("Segoe UI", 8),
            bg="#2a2a2a", fg=cor_status,
            anchor="w"
        ).pack(anchor="w")

        # Botão Conectar
        btn = tk.Button(
            row,
            text="Conectar",
            font=("Segoe UI", 9, "bold"),
            bg="#1976d2", fg="#ffffff",
            activebackground="#1565c0", activeforeground="#ffffff",
            relief="flat", cursor="hand2",
            padx=14, pady=6
        )
        btn.config(command=lambda s=serial, m=modelo, b=btn: conectar(s, m, b))
        btn.pack(side="right")

if __name__ == "__main__":
    app = App()
    app.mainloop()
