---
name: Muse Gadget 新板部署
description: >-
  当要为 Waveshare/社区 ESP32 板移植 muse-gadget-sdk、烧录配对 Muse、在国内网上用 WireGuard 出网、接入
  TTS/STT/电量计，或做熄屏保活与 OTA 时使用。
---
# Muse Gadget 新板部署

可复用流程：把一块新硬件跑通 Muse（本地工具 → 编译烧录 → 配对 → 国内出网 → 语音/电量 → OTA）。不要把具体 token、私钥、VPS 密码、Azure 密钥写进本 skill。

## 本地要装（Windows 开发机 + 手机）
1. **EIM**：安装 ESP-IDF **v6.0.1**，勾选目标芯片（常用 `esp32s3`）。日常用 **PowerShell** 开 IDF 环境，避免 Git Bash。
2. **USB**：能识别 Espressif（VID_303A）的 COM；必要时装官方 USB 串口驱动。
3. **仓库**：克隆 `muse-gadget-sdk`；读 `devices/README`、`AGENTS.md`。
4. **手机 Muse App**：与 SDK 账号一致；配对时打开 **设置 → 设备 → 开发者模式**。
5. **手机 WireGuard**：先单独测通节点（测时关掉 Shadowrocket），再把参数写入板端本地 sdkconfig/NVS。
6. **可选**：SSH / FinalShell 管 VPS（TTS / OTA 托管时需要）。
7. 用户还需提供：SDK token（`CONFIG_GADGET_SDK_TOKEN`）、USB 串口、WG 配置、（若用 Azure TTS）Speech 区域与密钥。

## 1. 板级移植
1. 以最接近的官方/已有 board 为模板（如 1.75 → 1.85B）。
2. 核对 LCD（如 ST77916 QSPI）、触摸、音频、BOOT GPIO、背光、**PA 使能脚极性**、电池/电量计。
3. LCD：上电先做硬件 RST，再 RDID；按 ID 选 init 表；读失败时用该板正确默认表（勿盲目 v1）。
4. **1.85B 电量**：板上有 **BQ27220**（I2C `0x55`，SCL/SDA 与触摸等同总线）。在 `board_waveshare_s3_185b.c` 的 `read_power()` 读 Voltage/Flags/SOC；芯片缺失时软失败，仅报 USB。
5. 编译目标 board 的 sdkconfig / `idf.py -B build-…`，确认 bin 产出。

## 2. 烧录与退出下载模式
1. 识别 Espressif USB（VID_303A）COM 口；杀掉占用 monitor 的进程。
2. 日常升级优先 **只 flash、勿 erase-flash**（保留配对 NVS）。需重新配对时再 erase。
3. 若串口 `waiting for download`：用户**短按 RESET、勿按住 BOOT**，或拔插 USB。
4. 少做反复烧录/DTR 复位，避免 Muse 会话掉线；开串口常会 USB-UART 复位 S3。

## 3. Muse App 配对
1. 橙/蓝呼吸 → App：**设置 → 设备 → 开发者模式** → Add Device → `MuseGadget-XXXXXX` → 短按 BOOT 确认 → 绿灯。
2. 配 Wi‑Fi；配对后 BLE 常关闭，列表要开开发者模式刷新。
3. 日志出现 `muse no token` / access token rejected：先 RESET；仍离线则 App **Reset pairing**，勿先 erase-flash。

## 4. 国内网络出网（WireGuard 优先）
1. 板子需访问 `api.muse.ai`；中国公网常 TLS 超时 → 固件内嵌 **WireGuard**。
2. 用户先用手机测通 WG（关 Shadowrocket）。
3. 固件：Wi‑Fi GOT_IP → SNTP → WG handshake → `AllowedIPs` 含 `0.0.0.0/0`（提高 `CONFIG_WIREGUARD_MAX_SRC_IPS`）→ DNS 经隧道。
4. 密钥放 **gitignore 的本地 sdkconfig / NVS**，勿提交仓库。
5. 成功心跳类似：`wifi=up wg=up ws=up raw=up`。


## 4b. WireGuard 配置放哪里
1. **板端密钥与端点**：`esp32/devices/sdkconfig.wg.local`（gitignore，勿提交）。编译时并入 `SDKCONFIG_DEFAULTS` / 合并进 `build-…/sdkconfig`。字段：`CONFIG_MUSE_WG_*`（私钥、对端公钥、`10.0.0.2`、endpoint/port、DNS 等）。
2. **固件逻辑**：`esp32/main/muse_wg.c`（Wi‑Fi GOT_IP → SNTP → `esp_wireguard`）；`wifi_mgr.c` 回调上下线。补丁：`/workspace/muse-185b-wireguard.patch`（不含私钥）。
3. **VPS 对端**：内核/服务端 WG，地址常为 `10.0.0.1`。**板端 endpoint 端口用 `51820`**（999.2.3+，避开家宽对高位 UDP 回程丢包）；VPS 仍监听 **33535**（手机客户端），并用 `iptables REDIRECT udp/51820→33535`。勿删 33535、勿重启/乱动 x-ui、勿抢 udp/443。OTA/TTS 仅绑 WG 地址。

## 5. 熄屏与连接保活
1. 默认电池场景：熄屏后再闲置一段时间，固件会 **wifi nap** 关 Wi‑Fi 省电 → WG/Muse 断，解锁要重连。
2. 随身终端建议开 **`CONFIG_MUSE_SCREEN_OFF_KEEP_LINK`**（1.85B 默认开）：熄屏只关背光/显示，Wi‑Fi 保持；电池会掉得更快。
3. 插 USB 供电时本就不会 nap。

## 6. 语音链路（999.1.7+：Azure STT + 更有感情的 TTS）
1. **上行 STT（推荐）**：按住 BOOT 录音 → 松手后固件 POST WAV 到 **`http://10.0.0.1:8788/stt`**（WG）→ VPS 用同一把 Azure Speech key（eastus）短音频识别 **`zh-CN`** → 板子 `muse_voice_send_text` 发给 Muse。失败则回退 Muse 云听写。密钥只在 VPS `muse-tts.env`（600），不上板。
2. **下行 TTS**：Speaker On 时拉 HTTP MP3；`http://10.0.0.1:8787/tts?q=`。
3. **台妹腔 + 感情**：音色 **`zh-TW-HsiaoChenNeural`**（台湾女声）。该音色 **无 StyleList**（`mstts:express-as` 无效）；感情靠 SSML **prosody**（默认 `rate=+8%` `pitch=+4%`）和中英标点处 `<break>`。可选 env：`AZURE_SPEECH_RATE` / `AZURE_SPEECH_PITCH` / `AZURE_SPEECH_STYLE`（一般勿开）。服务：`muse-tts-proxy` 同进程监听 **8787 TTS + 8788 STT**，只绑 `10.0.0.1`。只重启该 unit，**勿动 x-ui**。
4. **长回复**：订阅 NDJSON 行缓冲要够大；**CJK** 字体打开。
5. **F0 注意**：Speech 免费档并发 1，识别时尽量别叠 TTS。

## 7. OTA 远程升级（GitHub Releases，公开仓）
1. Muse UI 板（含 1.85B）已开 `CONFIG_HOMEHUB_OTA_ENABLED` + 双分区 + 回滚。仓库 **[muse-gadget-185b](https://github.com/timmy2048/muse-gadget-185b)** 已设为 **Public**，Release 资源无需下载 token。
2. **板端菜单（推荐，999.1.5+）**：设置 → **检查更新** → HTTPS 拉 `https://github.com/timmy2048/muse-gadget-185b/releases/latest/download/latest.json` → 「已是最新」或「发现新版本…确认升级？」→ 确认后 `ota_start`（`force=true`）；下载有进度条。开机约 20s 静默检查，设置页可有「新 x.y.z」角标。
3. **对话触发（仍可用）**：Muse `link.invoke` → **`device.ota`**（`url` + 可选 `force`）。
4. **发版（首选）**：抬 `version.txt` / `PROJECT_VER` → 编译 → `ota/publish_github.sh <bin> <ver> [notes]` → 创建/覆盖 Release `v<ver>`，资源含 `muse-gadget-185b-latest.bin`、`latest.json`（url 指向 `…/releases/latest/download/muse-gadget-185b-latest.bin`）、版本化 bin。清单默认：`CONFIG_MUSE_OTA_MANIFEST_URL`。
5. **VPS `muse-ota`（可选）**：`ota/publish.sh` / `10.0.0.1:8790` 仍可作桥接或备份；**999.1.4 及更早**仍读 VPS 清单，升到 999.1.5 后改走 GitHub。勿动 x-ui。
6. **约 300s 内** Muse 控制通道要起来，否则回滚；USB 兜底。细节 `/workspace/muse-ota-runbook.md`。

## 10. OTA 进度条与屏上打字 / 拼音 IME（999.1.6+）
1. **OTA 进度**：设置→检查更新→确认升级后，更新页显示百分比进度条（下载阶段有 %；校验写入约 100%）。
2. **打字发 Muse（主页图标）**：**999.1.3+** 在 Muse **主页右上**（扬声器对角）有键盘小图标；设置里的「打字」行已移除。发送后回到主页看回复。
3. **拼音 IME（999.1.6 重写）**：
   - 默认 **全键（26 键）**：逐字母累积无调拼音（如 `nihao`），候选栏实时刷新；点选后按 `consume` 吃掉对应字母，可继续打下一词。
   - **九键（T9）**：按键累积数字 `2–9`（如台湾=`824926`，你好=`64426`），按数字前缀匹配词/音节，**不再**用多击字母冒充拼音（旧版按 TUV 就出「台湾/讨厌」、再按 JKL 候选清空的问题已修）。
   - 模式键循环：`全键 → 九键 → abc → 123 → #+=`（按钮文案为下一模式名：九键/abc/…/全键）。
   - 词库：`muse_pinyin.c` 约 566 常用词 + 398 音节常用字；生成脚本 `esp32/tools/gen_muse_pinyin.py`；宿主单测 `esp32/tests/test_muse_pinyin.c`。
   - 候选/输入走已有 CJK Unifont。
4. **Azure STT / 感情 TTS（999.1.7+）**：见 §6；发版抬版本后 `publish_github.sh`；板子检查更新。
5. 发版：抬 `version.txt` → 编译 → `ota/publish_github.sh`（首选；板子 999.1.5+ 读 GitHub `latest.json`）→ 板上检查更新。≤999.1.4 仍可读 VPS 桥接。**勿 USB erase**；勿动 x-ui。

## 8. 新板验收清单
- [ ] 冷启动不花屏
- [ ] App 可见且 online（`ws=up`）
- [ ] 国内 Wi‑Fi 下 `wg=up`
- [ ] BOOT 语音：松手后有 RECOGNIZING/转写字，回复有声（Azure STT+TTS）；熄屏后唤醒不必重连（若开 KEEP_LINK）
- [ ] 电量百分比/电压合理（接电池时）
- [ ] 设置→检查更新 能拉到 GitHub latest.json（WG up，HTTPS）；升级时有进度条
- [ ] 主页键盘图标能打开打字；全键拼音/九键 T9 可切换；候选随输入刷新；发完回主页看回复
- [ ] 改 TTS/OTA 后手机 VLESS 等仍正常

## 9. 排障速查
| 现象 | 优先查 |
|------|--------|
| 花屏 | RDID / init 表 / LCD RST |
| App 离线 / muse no token | RESET；App Reset pairing；勿盲目 erase |
| API TLS 超时 | WG/DNS/AllowedIPs |
| 熄屏后必重连 | KEEP_LINK；是否电池 nap |
| 电量总是 USB | BQ27220 接线/驱动；电池是否接入 |
| 有字无声 | TTS `8787`；Speaker；ID3 |
| OTA 回滚 | 300s 内 Muse WS；版本/force；URL 可达 |
| OTA 确认后 0% 回主页 / 仍旧版本 | `latest.json` 标签与 bin 内 `esp_app_desc.version` 不一致 → `force=false` SKIPPED；用 `publish.sh`（已校验 embed==label）发正确 `PROJECT_VER` |
| 语音转写跑偏 / STT drift | 999.1.7+ 看 `8788` STT 与 Azure；失败会回退 Muse 听写；查 WG、`muse-tts-proxy`、麦增益 |
| 有字无声 / TTS 平淡 | `8787`；Speaker；prosody env；HsiaoChen 无 style |
| 手机 VLESS 超时 | 勿误重启 x-ui；查孤儿 xray |
