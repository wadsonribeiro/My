# scrcpy — Painel Lateral Recolhível

Modificação do [scrcpy](https://github.com/Genymobile/scrcpy) que adiciona uma **gaveta lateral deslizante** inspirada no LDPlayer, com controles rápidos diretamente na janela de transmissão.

---

## Funcionalidades do Painel

| Botão | Ação |
|---|---|
| 📸 **Print** | Captura a tela via `adb exec-out screencap -p` → salva em `Prints/` |
| 🔊 **Audio** | Alterna áudio para PC (`--no-audio`) — reinicia a conexão |
| 🔉 **Vol+** / **Vol-** | Ajusta volume do celular via `adb shell input keyevent` |
| ← **Back** | Envia KEYCODE_BACK ao dispositivo |
| 🏠 **Home** | Envia KEYCODE_HOME |
| ⊞ **Apps** | Envia KEYCODE_APP_SWITCH (recents) |
| ⚙️ **Config** | Abre janela de configurações |

### Janela de Configurações

- **FPS**: 30 / 60 / 120 / 240
- **Resolução**: 540p / 720p / 1080p / 1440p
- **Bitrate**: 4M / 8M / 16M / 32M
- **Áudio para PC**: Sim / Não
- **Salvar e Reiniciar**: salva em `devices_data.json` e reinicia a conexão com os novos parâmetros

### Comando gerado

```
scrcpy -s <DEVICE_ID> --gamepad=uhid --print-fps --stay-awake --max-fps=60 -m 1080 -b 8M
```
(flags `--stay-awake --gamepad=uhid --print-fps` são sempre incluídas)

---

## Como aplicar o patch e compilar

### Pré-requisitos

```bash
# Linux/Ubuntu
sudo apt install gcc meson ninja-build pkg-config \
    libsdl2-dev libsdl2-ttf-dev \
    libavcodec-dev libavformat-dev libavutil-dev \
    libswresample-dev libavdevice-dev libusb-1.0-0-dev
```

### Passos

```bash
# 1. Clone seu fork do repositório
git clone https://github.com/wadsonribeiro/My.git
cd My

# 2. Copie os arquivos desta pasta para dentro do repositório
cp -r /caminho/para/scrcpy-panel/app/src/panel   app/src/
cp /caminho/para/scrcpy-panel/apply_panel_patch.py .
cp /caminho/para/scrcpy-panel/.github/workflows/build.yml .github/workflows/

# 3. Execute o script de patch
python3 apply_panel_patch.py

# 4. Compile
meson setup build --buildtype=release
cd build && ninja
```

### Via GitHub Actions (recomendado)

1. Faça push do seu fork com os arquivos deste projeto
2. O workflow `.github/workflows/build.yml` será acionado automaticamente
3. Baixe os artefatos gerados em **Actions → seu build → Artifacts**
4. Para gerar um Release, crie uma tag: `git tag v1.0 && git push --tags`

---

## Estrutura de arquivos

```
app/src/panel/
├── device_config.h / .c    — leitura e escrita do devices_data.json
├── side_panel.h / .c       — gaveta lateral animada (SDL2)
└── settings_dialog.h / .c  — janela modal de configurações (SDL2)

devices_data.json           — configs por dispositivo (na mesma pasta do .exe)
Prints/                     — capturas de tela salvas aqui
apply_panel_patch.py        — script que modifica screen.h/c e meson.build
.github/workflows/build.yml — CI/CD para Windows e Linux
```

---

## devices_data.json

O arquivo é atualizado automaticamente ao clicar em **Salvar** na janela de configurações. Ele pode ser editado manualmente:

```json
{
  "8a20a960": {
    "name": "POCO",
    "fps": "60",
    "res": "1080",
    "bitrate": "8M",
    "audio": true
  },
  "RQ8N308S9YV": {
    "name": "Samsung S20",
    "fps": "60",
    "res": "1080",
    "bitrate": "8M",
    "audio": true
  }
}
```

A chave é o **serial USB** ou **IP:porta** do dispositivo.

---

## Padrões

| Parâmetro | Padrão |
|---|---|
| FPS | 60 |
| Resolução | 1080p |
| Bitrate | 8M |
| Áudio | Sim |
