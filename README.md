# muse-gadget-185b

Private fork of [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk) for **Waveshare ESP32-S3-Touch-LCD-1.85B** (device `MuseGadget-8794E0`).

Current firmware version: **999.1.4** (`esp32/version.txt`).

## What's in this repo

- `esp32/` — firmware sources with local features already applied (WireGuard, screen-off keep-link, BQ27220 fuel gauge, OTA check-update menu + progress, home-page type + pinyin IME, Azure TTS via VPS proxy URL).
- `patches/` — historical patch files (reference / re-apply on a clean upstream tree).
- `ota/publish.sh` — publish a built `.bin` + `latest.json` manifest to the OTA host (no secrets in script).
- `esp32/devices/sdkconfig.wg.local.example` — template for WireGuard keys (**real keys stay local**).

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
# merge device + wg local into sdkconfig, then:
idf.py -B build-muse-waveshare-s3-185b @devices/sdkconfig.muse-waveshare-s3-185b build
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

## OTA flow (GitHub → build → VPS → board)

1. Bump `esp32/version.txt` (and matching app version in build).
2. Build `.bin` on the Windows IDF machine.
3. On the VPS OTA host (`10.0.0.1:8790`, WireGuard-only), run `ota/publish.sh` with the new bin so it writes:
   - `firmware/muse-gadget-185b-latest.bin`
   - `firmware/latest.json` (version, url, size, sha256)
4. On device: **SETTINGS → 检查更新 → confirm**. Download shows progress; new image must reconnect to Muse within ~300s or it rolls back.

Chat/`device.ota` invoke still works; menu path is preferred.

## TTS

Board calls `http://10.0.0.1:8787/tts` over WireGuard (VPS proxy → Azure Speech). Azure keys live only on the VPS (`muse-tts.env`), **not** in this repo.

## Do not commit

- `sdkconfig.wg.local`, `.sdk-token`, `muse-tts.env`, Azure keys, VPS passwords
- `*.pem` signing keys, large `*.bin` blobs (optional GitHub Release later)
- Do not change x-ui / xray on the VPS from this workflow

## Patches (optional)

If rebuilding from upstream clean tree, apply under `patches/` in dependency order (full / wireguard / keepalive / bq27220 / ota-menu / ota-progress / type-home-pinyin). The `esp32/` tree here already contains the merged result for 999.1.4.
