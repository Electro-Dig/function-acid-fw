# FN ACID 编辑器：设计稿

状态：**设计 + 离线原型**（2026-10-19，对应固件 0.32）。原型在 `web/editor/index.html`，网站上是 `editor/`。
原型不连 FM-1；本文第 4、5 节是"要连上 FM-1 需要固件做什么"，**这些都还没有实现**。

---

## 1. 别人的编辑器做了什么（参照）

看的是同一家族的四个固件（源码在 SLOOP / Felucca / JIANT / X0X 仓库）：

| | 编辑器能做什么 | 和 FM-1 怎么说话 |
|---|---|---|
| **Felucca** `web/editor.html` | 每轨参数（DESC 描述、DUMP 全读）、16 步（音 / 连音 / 休止、重音、滑音、概率、ratchet、nudge）、用户预设 32 格、采样上传、旋钮动作（MOTION）、整机备份 / 恢复、菜单设置、MIDI Learn | SysEx `F0 7D 46 4C <cmd> … F7`（7D = 非商业 ID，"FL"）。一问一答，回复带同一个 cmd；数值是 v14（两字节，+8192）；大块数据 pack7（7 个字节前面放一个高位字节）。`WATCH` 打开后机器主动推 `CHANGED / RELOAD / STEP_CHANGED`，编辑器每秒 `PING` 一次，3 秒没请求就停推 |
| **SLOOP** `web/editor.html` | Felucca 的全部 + 鼓 16 道、parameter lock、fill 条件、FM6 音色库、工程备份 / 恢复（BK_LIST / BK_GET / BK_PUT，分块 + CRC-32）、**浏览器里试听**（`preview.js` + `sloop-preview.wasm`：固件声音代码编成 WebAssembly） | 同 Felucca 的 "FL" 协议，版本号往后加，只在回复末尾追加字节，旧编辑器不受影响 |
| **JIANT** | Felucca 的分支，6 轨 | 同上 |
| **X0X** `web/backup/` `web/emu/` | 没有完整编辑器：一个**备份 / 恢复页**，一个**整机模拟器**（host 模拟器编成 wasm，在 AudioWorklet 里跑整个固件：屏幕、灯、声音） | 备份走升级通道的帧格式 `F0 pack7(00 59 cmd len24 body ~sum) F7`，cmd 0x41–0x43；机器一帧一帧推镜像，恢复时机器自己用读请求拉镜像、校验每段 CRC，**屏幕上问一句 RESTORE FROM USB? 按键确认**才提交，断电也只有"全改或全不改" |

几条值得照搬的规矩（Felucca `EDITOR_PROTOCOL.md` 的 Notes）：

- **一次只发一个请求**，等回复（10–50 ms）再发下一个：机器只有一个 SysEx 接收缓冲。
- 推送随时可能到，按 cmd 区分，推送的 cmd 永远不是回复。
- **绝不发升级用的 SysEx**（`F0 22 24 35 …`、`00 59 …` 帧），那是安装器的。
- 只往回复**末尾追加**字节来升级协议；编辑器读得懂的前几个字节永远在原位。
- 写闪存的命令只在停止播放时做（擦扇区会关中断几十毫秒，声音会断）。

## 2. FN ACID 编辑器应该让人做什么

按优先级排：

**P0（第一个能连机器的版本）**
1. **在大格子上写 ACID 的 16 步**：钢琴卷帘（横 16 步、竖三个八度），每步 音 / 连音 / 休止、重音、滑音；长度 1–16、移调、摇摆。机器上要"按住白键 + 拧四个旋钮"才能改一步，电脑上一笔画过去。
2. **8 条预设旋律**一键载入（就是 `PATS`），在上面改。
3. **主要音色旋钮**：HOME 四个（CUTOFF RESO ENV DECAY）+ ACCENT GATE GLIDE DRIVE + 波形 / 次低音 / 八度 / 滤波器类型 + 延时四个 + 混响 + 底鼓压 ACID + 镲的节奏 + 速度、根音。名字和范围跟机器一样（`PM[]` 表）。
4. **从机器读回来 / 发过去**，并且**跟着机器走**：在 FM-1 上拧旋钮、改步，编辑器上同步动；播放头跟着走。
5. **存成文件 / 读文件**（一条线 = JSON，见第 6 节），可以分享。

**P1**
6. **整机备份 / 恢复**：把 `params_t` 整块（468 字节）存成文件，刷别的固件之前先备份，回来再恢复。
7. **旋钮动作**（4 条 × 64 步的 `mot[]`）画成曲线，可以用鼠标画、清掉一条。
8. **用户存档 U1–U8**（机器上还没有，TEST-ZH 里写着"已知的局限"）。

**P2**
9. **完全一样的试听 + 屏幕**：像 X0X / SLOOP 一样把 `dinghy.c` 的 host 版本（`tests/host.c` 已经能跑出 WAV 和截图）编成 wasm，在浏览器里跑整个固件。这样试听就是机器的声音，屏幕也是真屏幕，不需要固件改动。
10. 公式模式（AA 的 N / OFFSET / DENSITY 和各项开关）、BIO / KICK / PERC 的音色页、FX 层的说明。

**不打算做**：实时把 240×240 屏幕从机器传过来（一帧 115 KB，SysEx 太慢，见 4.4）；编辑器里直接刷固件（那是安装页的事）。

## 3. 现在的固件：没有编辑器通道

查了 0.32 的代码，结论是**编辑器现在完全连不上**，原因：

- `usb.c` 的 `sysex_byte()` 把收到的 SysEx 拼成一帧（`sx_frame[640]`，一次只放一帧），交给主循环。
- 主循环里 `platform_poll()` → `ota_service()` → `ota_take()`：按升级协议解（`00 59 …` + 校验和），**不是升级帧就直接丢掉**。只认 cmd 0x11（握手，回答身份 `FM-1_956`）。
- `docs/API.md` 也写明："SysEx is the platform's (the updater): it does not reach the app"。`dinghy.c` 只能用 `midi_get()` 收音符、CC、时钟。
- 机器的 USB MIDI 端口名叫 **"Felucca"**（`usb.c` STR2），身份号 `FM-1_956`。编辑器要按名字找端口，再用握手确认是 FN ACID（安装页的 `find()` 就是这么做的）。

## 4. 协议提案："FA" v1

照 Felucca 的 "FL" 协议的形状（一问一答、v14、pack7、WATCH/PING），但用自己的头，免得和 SLOOP / Felucca 编辑器互相误认：

```
请求 / 回复：F0 7D 46 41 <cmd> <args…> F7        7D = 非商业 ID，46 41 = "FA"（Function Acid）
```

| 编码 | |
|---|---|
| v14 | 两个 7 位字节，低位在前，存 value + 8192（-8192..8191），和 Felucca 一样 |
| u35 | 五个 7 位字节，低位在前（长度、CRC-32 用） |
| pack7 | 每 7 个字节前面加一个字节放它们的最高位（bit j = 第 j 个字节的 bit 7），和 Felucca 一样 |
| 字符串 | ASCII，0 结尾 |

### 4.1 命令

| cmd | 请求 | 回复 |
|---|---|---|
| 0x01 INFO | — | 协议版本 1；APP_VER（"0.32"）；身份（"FM-1_956"）；`sizeof(params_t)` v14（468）；布局号 v14（见 5.6）；参数个数 NPM（`PM[]` 的长度）；预设条数 8；状态字节（bit0 在播放，bit1 ACID 在用公式） |
| 0x02 DESC | idx | idx，标签（"CUTOFF"），min v14，max v14，格式（NUM / LIST / ST / PCT / NOTE / OCT），列表项名字（LIST 时）。直接来自 `PM[]` |
| 0x03 GET | idx | idx，v14 |
| 0x04 SET | idx，v14 | idx，v14（钳过之后的值，`pset()` 的规则） |
| 0x05 LINE_GET | — | sLen，sTrans v14，16 × (状态 0 休止 / 1 音 / 2 连音，音高 0..127)，accM（3 个 7 位字节），sldM（3 个 7 位字节）：一整条线一帧 |
| 0x06 LINE_SET | 同上 | 同上（写入后的样子）。音高钳到 12..96，第 1 步的连音照机器的规则处理 |
| 0x07 STEP_SET | i，状态，音高，重音，滑音 | 同 |
| 0x08 PATTERN | k（0..7） | k；然后推一个 RELOAD。就是 `load_pattern(k)` |
| 0x09 TRANSPORT | op（0 查询，1 播放，2 停止） | 在播放？，当前步 |
| 0x0A BLOB_INFO | — | 长度 u35（468），CRC-32 u35，magic（0xA7），布局号 v14。同时拍一张快照给 BLOB_GET 用 |
| 0x0B BLOB_GET | 偏移 v14，个数（1..64） | 偏移，个数，pack7 数据（64 字节 → 74 字节在线上） |
| 0x0C BLOB_PUT | op 0 开始：长度 u35，CRC u35 · op 1 数据：偏移 v14，pack7（≤ 64 字节，按顺序）· op 2 提交 · op 3 放弃 | op，rc：0 好，1 参数错，2 校验 / 范围不对，3 先停止播放，5 没有开始（或超过 15 秒） |
| 0x0D MOTION | m（0..3），op（0 读，1 写 64 字节 pack7，2 清掉） | m，64 字节 pack7（255 = 这一步没有动作） |
| 0x0E SAVE | — | rc（0 好，3 先停止，4 闪存错）。就是 SAVE 键的 `settings_save()` |
| 0x0F WATCH | on（0/1） | on。打开后推送 0x20–0x23；3 秒没收到请求自动关 |
| 0x10 PING | — | 0 |
| 0x20 CHANGED（推） | — | idx，v14：机器上拧了一个参数（每个 idx 最多 20 ms 一次，只发最新值；编辑器自己 SET 的不推） |
| 0x21 STEP_CHANGED（推） | — | i（0xFF = 整条线变了：掷新线、变异、录音） |
| 0x22 RELOAD（推） | — | 原因（0 预设，1 章节，2 别的）：编辑器重新 LINE_GET + 读参数 |
| 0x23 POS（推） | — | 步（0..15），在播放？：播放头。每步一次，只在 WATCH 时 |

规矩照 Felucca：一次一个请求；不认识的命令不回；机器回复的帧不超过 ~180 字节（USB 发送环 `SXQ` = 64 个包 × 3 字节），所以大块数据都切成 64 字节。

### 4.2 一次典型的连接

1. 找名字像 `Felucca` / `FM-1` 的端口，发升级握手（`00 59 11`，安装页的 `HS_QUERY`），身份是 `FM-1_956` 才继续。
2. `INFO` → 看协议版本、布局号；`DESC` 0..NPM−1 建立旋钮表（以后加参数不用改编辑器）。
3. `LINE_GET` + 每个参数 `GET`（或 `BLOB_GET` 整块读）→ 画出来。
4. `WATCH 1`，每秒 `PING`；收到推送就更新。
5. 用户改了：步 → `STEP_SET` / `LINE_SET`；旋钮 → `SET`（拖动时限到每 20 ms 一次）。
6. 用户按"存进机器" → `SAVE`（播放时机器回 rc 3，编辑器提示先停止）。

### 4.3 备份 / 恢复

备份文件 = INFO 里的版本、布局号 + `params_t` 的 468 字节（base64）+ CRC-32。恢复：`BLOB_PUT` 开始 → 数据 → 提交。
机器在提交时**先整块校验**（magic、长度、CRC、每个字段在 `PM[]` 的范围里、`sLen` 1..16、`lane` 只有 NONE 或 12..96 加连音位、`mot` 是 0..127 或 255），合格才换进 `P`，不合格一个字节都不改。
布局号对不上（文件来自别的版本）→ 编辑器改用按标签逐个 `SET`（标签在 `DESC` 里），丢掉不认识的字段。

### 4.4 屏幕

不建议从机器传屏幕：240×240×2 = 115 KB，pack7 后 132 KB，USB MIDI 的 SysEx 实际几十 KB/s，一帧要好几秒，还会挤掉音符。
做法是：编辑器根据 LINE / 参数 / POS 自己画（原型里的卷帘就是机器 STEPS 页放大的样子），要"真屏幕"就走 P2 的 wasm 模拟。

## 5. 固件要改的（诚实清单，都还没做）

1. **让 SysEx 到达应用**（平台代码 `platform.c` / `ota.c`，Felucca 的部分，要小心）：`ota_service()` 在丢弃非升级帧之前，先看原始帧开头是不是 `7D 46 41`，是就交给应用的钩子 `app_sysex(p, n)`（没定义钩子的应用照旧丢弃）。帧最大 640 字节、一次一帧，这些限制不变。
2. **回复的发送**：用平台已有的 `ota_wire_send()`（主循环里调用，会等发送环有空位，500 ms 超时）。
3. **`dinghy.c` 里的处理函数** `ed_service()`：上面每个命令一个分支。字段表直接用现有的 `PM[]`（标签、偏移、范围、列表名都在），`SET` 走 `pset()` 钳值；改完调用 `count_lanes()`、`mot_count()`、`changed()`（会在停止 20 秒后自动存）。
4. **和声音中断的并发**：`P` 被音频中断读。单个字节的 SET 没问题；`LINE_SET` / `BLOB_PUT` 提交要先放进一块暂存区、校验，再在两次音频块之间一次性拷进 `P`（或拷贝时短暂关音频中断，468 字节约几微秒），避免半条线被播出来。
5. **推送**：旋钮、步编辑、掷线 / 变异、录音、载入预设这些地方要记一下"变了什么"（现在只有 `changed()` 一个出口），主循环在 WATCH 时按 20 ms 合并发出；POS 在 `E.s` 变的时候发。
6. **布局号**：`params_t` 现在只有 magic 0xA7 和长度，没有版本号。加一个 `#define PARAMS_LAYOUT`（0.32 = 2），每次往结构体里加字段就加一，INFO 报给编辑器。
7. **SAVE 只在停止时**：`settings_save()` 擦扇区要关中断几十毫秒（API.md），播放时回 rc 3。
8. **用户存档 U1–U8**（P1）：要新的闪存位置。设置扇区 4 KiB 里只用了 1 KiB；8 条线 × 约 60 字节可以放进同一扇区的第二条记录，或者另开一个扇区（要在 `platform.c` 放宽 `FL_RANGE_OK`）。
9. **测试**：`tests/host.c` 可以加一个"喂 SysEx 帧"的脚本命令，像 Felucca 的 `tests/editor_test.c` 那样把每个命令、坏帧、超时都跑一遍；网页那边用一个模拟设备测编辑器（Felucca 的 `web/test_web.mjs` 的做法）。
10. **（P2，不改固件）wasm 模拟**：`dinghy.c` 的 host 版本用 emscripten 编成 wasm，参照 X0X 的 `web/emu/`，编辑器的试听换成它。

估计工作量：1–7 是一个版本（0.33 或 0.34）的事，平台改动约 20 行，`dinghy.c` 约 300 行；8 另算。

## 6. 一条线的 JSON（原型已经在用）

```json
{
 "format": "fn-acid-line", "version": 1, "firmware": "0.32", "name": "FILM",
 "steps": [ {"gate": "note", "note": "A1", "midi": 33, "accent": true, "slide": false},
            {"gate": "tie", "note": null, "midi": null, "accent": false, "slide": false}, … 16 个 ],
 "params": {"root": 33, "sLen": 16, "sTrans": 0, "bpm": 120, "swing": 0, "cut": 80, "res": 100, "env": 68,
            "aDec": 18, "aAcc": 110, "aGate": 50, "aGlide": 0, "aDrive": 30, "aWave": 0, "aSub": 0, "aTune": 0,
            "aLvl": 100, "aFilt": 0, "dTime": 2, "dFb": 54, "dMix": 52, "dTone": 64, "rMix": 40, "mDrive": 20,
            "kDuck": 40, "kLvl": 110, "hPat": 1, "hLvl": 64},
 "terms": {"kick": true, "four": true, "perc": true},
 "raw": {"lane0": [33, 33, 45, …], "accM": 16961, "sldM": 2180}
}
```

- `params` 的名字就是 `params_t` 的字段名，数值就是机器里的数（0..127 这类），以后可以原样 `SET` 过去。
- 读的时候以 `steps` 为准（`note` 可以写 "C2" 也可以写 `midi`），`raw` 只是给人对照固件用的；每个值都按 `PM[]` 的范围钳一遍。
- 连音不带音高：它延续前一个音（和机器一样）。

## 7. 原型做到了什么、没做什么

做到（`web/editor/index.html`，一个文件，离线可用）：
- 钢琴卷帘（根音下一个八度到上两个八度），点 = 写音，点同一个音 = 休止，拖动一笔画；下面三排：GATE（音 / 连 / 休）、ACC、SLIDE。
- 8 条预设（照 `load_pattern()` 载入音色）、OCT+ 写新线 / OCT− 变异（照 `gen_steps()` / `mutate_steps()` 的规则）、左右移、移调、撤销。
- 20 个音色旋钮，名字 / 范围 / 显示（DECAY 显示毫秒）照 `PM[]`。
- 试听：AudioWorklet 里照固件重写的 ACID 声部（`eval_steps` 的连音 / 滑音 / 门限、`cutoff_at` 的 303 式 DECAY 和重音、4 层梯形 / 2 层 SVF、`sat()` 失真）、附点八分乒乓延时（反馈里低通）、Freeverb 式小混响、4/4 底鼓（压 ACID）和镲的 6 种节奏。
- JSON 导出 / 导入 / 直接改；浏览器记住最后的状态。
- 明确标出"发送到 FM-1 需要以后的固件"（按钮是灰的）。

没做：连机器（没有协议）、BIO 动物轨、公式模式、旋钮动作、FX 层、和机器逐样本一致的声音（要 P2 的 wasm）。
