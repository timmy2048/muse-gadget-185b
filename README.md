# muse-gadget-185b

Public fork of [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk) for **Waveshare ESP32-S3-Touch-LCD-1.85B** (device `MuseGadget-8794E0`).

Current firmware version: **999.1.5** (`esp32/version.txt`).

## What's in this repo

- `esp32/` — firmware sources with local features already applied (WireGuard, screen-off keep-link, BQ27220 fuel gauge, OTA check-update menu + progress, home-page type + pinyin IME, Azure TTS via VPS proxy URL).
- `patches/` — historical patch files (reference / re-apply on a clean upstream tree).
- `ota/publish_github.sh` — **preferred**: publish built `.bin` + `latest.json` as a **public GitHub Release** (no download token).
- `ota/publish.sh` — **optional** legacy publish to VPS WG-only host `10.0.0.1:8790`.
- `esp32/devices/sdkconfig.wg.local.example` — template for WireGuard keys (**real keys stay local**).
- `esp32/devices/sdkconfig.ota.local.example` — optional manifest URL override (no secrets for public Releases).

## Clone & build (Windows + ESP-IDF v6.0.1)

```powershell
git clone https://github.com/timmy2048/muse-gadget-185b.git
cd muse-gadget-185b
```

1. Install ESP-IDF **v6.0.1** (EIM / Espressif installer).
2. Copy WireGuard secrets (never commit):

```powershell
copy esp32\devices\sdkconfig.wg.local.example esp32\devices\sdkconfig.wg.local
# edit sdkconfig.wg.local — fill CONFIG_MUSE_WG_* keys / endpoint
```

3. Build for 185B (see `build_flash_185b_wg.ps1` or your IDF env):

```powershell
cd esp32
idf.py -B build-muse-waveshare-s3-185b -D PROJECT_VER=999.1.5 build
```

First flash over USB (keeps NVS / pairing if you do **not** erase):

```powershell
idf.py -B build-muse-waveshare-s3-185b -p COMx flash
```

## WireGuard config location

| Where | What |
|-------|------|
| `esp32/devices/sdkconfig.wg.local` | **Local only** (gitignored). Board private key, peer pubkey, endpoint, address `10.0.0.2`. |
| `esp32/devices/sdkconfig.wg.local.example` | Safe template in git. |
| `esp32/main/muse_wg.c` | Runtime WG bring-up. |
| VPS peer | `10.0.0.1` (do **not** touch x-ui / other inbounds). |

## OTA flow (GitHub Releases — public, no token)

Board manifest URL (999.1.5+):

`https://github.com/timmy2048/muse-gadget-185b/releases/latest/download/latest.json`

1. Bump `esp32/version.txt` and build with matching `-D PROJECT_VER=…`.
2. Publish:

```bash
./ota/publish_github.sh path/to/muse-gadget.bin 999.1.5 "notes"
```

This creates/replaces GitHub Release `v999.1.5` with assets:
- `muse-gadget-185b-999.1.5.bin`
- `muse-gadget-185b-latest.bin`
- `latest.json` (`version`, `url`, `sha256`, `size`, `notes`)

Stable download URLs (unauthenticated):
- Manifest: `…/releases/latest/download/latest.json`
- Firmware: `…/releases/latest/download/muse-gadget-185b-latest.bin`

3. On device: **SETTINGS → 检查更新 → confirm**. Progress bar during download; new image must reconnect to Muse within ~300s or it rolls back.

**Bridge from 999.1.4:** that build still points at the VPS manifest. Publish 999.1.5 to VPS once (`ota/publish.sh`) so the menu can pull the GitHub-capable image; afterwards OTA is GitHub-only. VPS `muse-ota` may stay running but is optional.

Chat/`device.ota` invoke still works; menu path is preferred.

## TTS

Board calls `http://10.0.0.1:8787/tts` over WireGuard (VPS proxy → Azure Speech). Default voice **`zh-CN-XiaoxiaoNeural`** with **`style=friendly`** (set in VPS `muse-tts.env`; see `tools/tts_wg_proxy.py`). Azure keys live only on the VPS, **not** in this repo. Changing voice/style needs VPS restart only — no board flash.

## Do not commit

- `sdkconfig.wg.local`, `sdkconfig.ota.local`, `.sdk-token`, `muse-tts.env`, Azure keys, VPS passwords
- `*.pem` signing keys
- Do not change x-ui / xray on the VPS from this workflow

## Patches (optional)

If rebuilding from upstream clean tree, apply under `patches/` in dependency order. The `esp32/` tree here already contains the merged result for 999.1.5.
