# JIANT 对照评审：FN ACID 能从它身上学什么

- 评审对象：JIANT FM v0.9.12（beta），commit `d8b93c7`（2026-10-19），本地副本
  `C:\Users\yunlo\AppData\Local\Temp\claude\D--claude\f9c65a58-f613-41fe-97f5-466c2b8374d1\scratchpad\repos\jiant`
  （下文路径都相对这个目录；我们自己的代码写 `fn:` 前缀，相对 `D:\claude\function-acid-fw`）。
- 我们：FN ACID 0.50（`fn:firmware/src/dinghy.c:17`），build 产物 `fn:build/syms.txt`（2026-10-25 15:09）。
- 只读调研，没有改任何代码。JIANT 的"设备上 CPU 百分比"它自己几乎没公布，下文凡是 JIANT 的 CPU 数字都注明是
  主机 callgrind 指令数还是设备观测。

---

## 0. 结论先行（按对我们的影响排序）

| # | 发现 | 对我们的意义 | 工作量 / 风险 |
|---|---|---|---|
| 1 | **代码放在 flash 的哪个位置会直接改变音频 CPU**：JIANT 0.9.10 只加了 6 KB UI 代码，音频代码一个字节没动，作者的 FM-1 就从正常变成负载 94 %、全输出爆音；原因是引擎代码被推后 6 KB，和效果器在 cache 里互相冲突（`README.md:674-685`）。修法：中断能到达的所有函数和表按名字排在映像最前面（`tools/build.py:205-229`、`firmware/app.ld:31-33`） | 证实了"我们的开销主要在 XIP 取指"这个判断；而且我们现在 UI 代码（`app_main` 18 KB）和音频代码交错排在一起，改 UI 就可能让声音变卡 | 低 / 极低 |
| 2 | **最热的音频代码复制到 RAM 里跑**（`.hot_text`，`firmware/app.ld:5-9,38-46`，`tools/build.py:188-202`，`README.md:2195-2202`）：两个音频块之间 UI 会把 cache 冲掉，下一块又得从慢 flash 重取 | 我们是"一个 ISR + 主循环 UI 同核"，正是这个场景；我们有 ~35 KB 空闲 `.bss`、~17 KB 空闲 pool | 中 / 中低 |
| 3 | **CPU 时钟 360 → 320 MHz 反而省约 11 % CPU**（`README.md:692-697`，`firmware/hal/fm1_clk320.h`，MIT）：320 MHz 那一档把 flash 控制器（SFC）调到 80 MHz，取指更快 | 再次说明瓶颈是 flash；这是一个现成、MIT 许可、可单独移植的头文件 | 低中 / 中 |
| 4 | **块处理 + 控制率**：每 32 帧一个控制块（`tools/gen_tables.py:14`），包络 / LFO / 音高 / 截止频率每块算一次、块内线性插值幅度（`firmware/src/dsp.c:113-129`，`voice.c:930-1073`），每个 voice 的状态先拉进局部变量再跑紧凑的内层循环（`eng_analog.c:162-226`） | 我们的 `app_audio` 是"每个采样跑一遍所有声部"的巨型循环（`fn:dinghy.c:1489-1689`），KEYS 8 个 float voice 每个采样都在 switch 分支、乘包络（`fn:dinghy.c:1094-1184`）。改成按 voice 分块渲染是我们 CPU 的最大结构性收益点 | 中 / 低 |
| 5 | **半采样率渲染**：DRUM-X 全部在 22 kHz 算再升回来（`drumx_voice.c:181,356`），混响预延迟、CLOUD、长延迟同样半速（`fx.c:25-27,1236-1274`） | 我们的 909/808（`d9_render` 8.9 KB、`d8_render` 10.9 KB，float，全速）和小混响可以这样做 | 中 / 中（音质要听） |
| 6 | **负载调速器**：超过半块预算 65 % 就不再加新 voice，先砍 -50 dB 以下的尾音；连续两块超过 80 % 就淡出一个 voice（`audio.c:58-131`，`voice.c:376-424`） | KEYS 目前没有任何负载保护；加一个"安静尾音剔除"几乎免费 | 低 / 低 |
| 7 | **半块中断里先 ack 再渲染**（`audio.c:143-147`）：0.8.8 之前在渲染之后 ack，渲染一旦超时就会吞掉下一个半块、重播旧音频 | 我们 `fn:firmware/src/platform.c:241-250` 正是渲染完才 `fm1_audio_ack_half()`，同一类 bug，平时 40 % 不触发，接近满载时会出现 | 极低 / 极低 |
| 8 | **演奏层**：MUTATE（围绕"锚点"每 N 小节自动变化、可撤回）、可量化可录进 lane 的 punch-in、FX LATCH、UNDO/REDO、旋钮加速、场景化的 song 行（`README.md:274-323`，`project.c:1833-1840`，`pfx.c:1-29`，`ui_input.c:415-447`） | MUTATE ORBIT 非常适合 acid：一条 303 线自己"呼吸"几分钟但不跑偏；量化 + 录进 lane 让我们的 FN 曲线 punch-in 变成可编曲的东西 | 各 0.5-2 天 / 低 |
| 9 | **母线响度保持器（leveler）+ 带补偿的 CLIP**（`README.md:338-343`） | acid 的共振和 accent 让音量剧烈起伏，一个慢速 2:1 rider 正好对症 | 低中 / 低 |
| 10 | **基础设施**：A/B 双扇区掉电安全存储（`storage.c:4-13`）、固件原样编译成 WASM 的浏览器模拟器（`BUILDING.md` Browser emulator，`web/emu/felucca_web.c`）、golden 哈希 + callgrind 每采样指令数回归（`tests/regress.c`，`tests/cpu_baseline_vg.txt`，`tests/target_budget.txt`）、脏块哈希刷屏（`gfx.c:320-330`） | 我们存储是单扇区先擦后写（`fn:platform.c:137-173`），掉电会丢全部设置；我们的 JS 原型和固件会漂移 | 各 1-3 天 / 低 |

**不建议现在做**：第二核。JIANT 的双核要靠 check word、超时、输出越界检测来防"有些 FM-1 的第二核偶尔把 RAM 读错"
（`hal/fm1_multicore.h:17-28`，`fx.c:2195-2273`），0.9.12 还因为部分机器崩溃把"崩溃后永久关双核"设为默认
（`README.md:686-691`）。我们 40 % 负载，先把上面 1-7 做完，大概率根本不需要第二核。

---

## 1. JIANT 在音乐上是什么

- 定位："Bio-synthetic operating system"，为现场演奏和在机器里做完整歌曲设计（`README.md:1-16`）。Felucca 1.1.5.1 的 fork，
  歌曲系统取自 SLOOP（`README.md:10-12`）。"TONIC" 是作者给这个 fork 的产品计划代号（FELUCCA TONIC），不是声音概念：
  核心是"更少但更深的引擎、可演奏可变形、每首歌内部能编排变化，而不是一个只拧 cutoff 的无限 loop"
  （`FELUCCA-TONIC-SPEC.md` FILOSOFÍA，`FELUCCA-TONIC-VISION.md` §1）。
- 规模：6 条轨，每轨一个合成部件，任意引擎上任意轨；8 个共享 voice（双核时 12），64 步
  （`firmware/src/core.h:11-14`，`voice.c:284,316`）。
- 9 个引擎，全部实时合成、无采样（`README.md:221-257`）：
  - **DRUM-X**：Microtonic 式，每个声音 = 带音高包络和 FM 的振荡器 + 滤波噪声 + 指数衰减，A/B 两套参数 + MORPH 实时插值；
    8 个声道 BD SD CP CH OH TM RS CB，FOLD 折叠、PM.TYPE（DECAY / LONG 808 式 / NOISE / SINE）（`drumx_voice.c:1-31`，`README.md:258-272`）。
    "键盘每个八度都是鼓组"：中央以上 ratchet x2/x4/x8，以下 SHORT / GHOST / FLAM（`README.md:267-269`）。
  - **ANALOG**：两个 polyBLEP 振荡器（SAW SQR TRI SIN PWM SYNC RING SAW3），每 voice 一个梯形 SVF（Simper，Q13 系数）
    （`eng_analog.c:1-21,160-226`）。它的 "ACID" 只是一个 2 极 SVF 预置（`eng_analog.c:234`），**没有真正的 303 梯形滤波、accent、slide 逻辑**——
    这一块我们比它专精。
  - **FM6**：Dexed/msfa 引擎，可导入 DX7 .syx，32 音色库（`README.md:226-236`）。
  - **VOICE**：klattsch 式共振峰，能按音节唱 ES/EN/JA 歌词（`README.md:242-244`）。
  - **ANIMAL**：物理模型动物叫声，5 种源：CMOS（40106/4093 施密特触发 RC 振荡器"鸟机"）、PULSE（擒纵脉冲进二极共振器：蟋蟀、蝉、青蛙）、
    WHSTL（Hopf 振荡器：鸟鸣随气压越过阈值诞生）、WING（翅膀嗡声 + 多普勒）、GLOTT（声门脉冲 + 两个共振峰 + 混沌），
    TEMP 按 Dolbear 定律让昆虫随温度变快（`eng_animal.c:1-45`，`README.md:245-252`）。
  - LOFI（1/4/8 bit、bytebeat 32 公式、floatbeat）、PHASE（相位失真 + 反馈）、WHEEL（拉杆风琴 + 转子）、NOISE。
  - 所有引擎都有 EDIT > FILTER：LP/HP/BP/**COMB**（调到当前音高的反馈梳状）（`README.md:253-256`，`voice.c:47-88`）。
- 效果与母线：每轨 DIST（SOFT HARD FOLD CRUSH RECT + TONE）；delay（同步/自由，0.74 s 以上半速、1.49 s 以上 1/4 速，
  grain delay）；5 种混响 ROOM / SPRING / SHIMR / RESO（四根按调式调好的弦）/ CLOUD（颗粒）；chorus；
  母线 CLIP（带电平补偿）、PUNCH、DUCK、常开 leveler（慢速 2:1、-9…+6 dB 自动补偿）、限幅器、DJ FILTER 双极旋钮
  （`README.md:330-343`）。

### 整数还是浮点
全部定点：Q15 信号、32 位乘积，`dsp.c:1-5`（"all fixed point"），TONIC 审计也写明 DSP 全 Q15（`docs/TONIC-AUDIT.md` §2 FPU 行）。
DRUM-X、ANIMAL 注释都强调"Only 32-bit products"（`drumx_voice.c:20`，`eng_animal.c:36`）。唯一的 float 是 LOFI 的 floatbeat 公式。

### 过采样 / 抗混叠
没有过采样。靠：polyBLEP（`dsp.c:25-48`）、正弦表线性插值（`dsp.c:15-21`）、tanh 表插值软削波（`dsp.c:57-68`）、
FM6 的抗混叠处理（`eng_fm6.c:213` 附近）、LOFI 固定一阶低通去顶（`eng_lofi.c:266-268`）、WHEEL 顶部折回一个八度（`eng_wheel.c:10`）。
反而是**降采样**用得多（见 §5）。

### CPU 数字
- 主机 callgrind（x86-64 gcc -O2，每采样指令数，不代表芯片）：单引擎 1300-2300，6 轨 + 全效果的 `song/cputest` 3046，
  双核 12 voice 时核 0 只占 1949（`tests/cpu_baseline_vg.txt`，`README.md:712-720`）。
- 目标机静态预算：反汇编里每个函数循环内的指令数 ×4/嵌套层，允许 +10 %（`tests/target_budget.txt`），例如
  `analog_render 581`、`animal_render 731`、`dly_lo 2949`、`fm1_timer5_irq 33439`。
- 设备：只有定性观测。0.9.9 干净、0.9.10/0.9.11 负载到 94 % 爆音（`README.md:674-685`）；320 MHz 约省 11 %（`README.md:692-697`）。
  菜单 MENU › ABOUT 显示 AUDIO LOAD / PEAK / LATE / SHED（`audio.c:178-183`）。

---

## 2. 双核、RAM vs XIP、链接脚本、块大小

### 块大小与中断
- I2S 半缓冲 128 帧 = 2.9 ms（`core.h:15`），中断里再切成 CTL = 32 帧（0.73 ms）的控制块逐块 `mix_block`（`audio.c:155-161`，
  `tools/gen_tables.py:14`）。我们是 256 帧 = 5.8 ms 一次中断（`fn:platform.c:63`），控制率每 16 采样一次 `control()`（`fn:dinghy.c:1522`）。
- **先 ack 再渲染**（`audio.c:143-147` 注释：0.8.8 之前在渲染后 ack，一次超时会把下一个半块也吞掉）。
- 负载计：每半块用 TIMER4 计时、扣掉嵌套进来的 TIMER5（按键/USB）时间（`audio.c:83-99`）。

### 第二核
- 何时：0.6.71 起（实验）（`README.md:152-180`）。
- 怎么分：每 32 帧块，按各轨最近几块实测耗时（TIMER4，`fx.c:2124-2140`）贪心分给两个核（`mc_split`，`fx.c:2147-2190`）；
  鼓轨捆在一起；核 1 同时跑效果总线，用的是上一块的 send（晚 0.7 ms，`fx.c:2192-2197`）；核 0 渲完自己的再 `mc_wait` 合并
  （`fx.c:2317-2350`）。结果与单核逐位相同，有测试保证（`README.md:162-163`）。
- 安全：job 带 check word（函数地址 ^ 参数 ^ 序号 × 黄金比），读错、超时、fault 立刻停核 1、由核 0 接手，至多丢一个 0.7 ms 块
  （`hal/fm1_multicore.h:17-28,66-100`）；效果输出超出 2^20 就静音该块并清空所有延迟线（`fx.c:2199-2273`）；
  核 1 的等待循环在 RAM（`.c1_text`），干活时**从 flash 跑**——因为它在部分机器上会读错 RAM（`core.h` FELUCCA_HOTRAM 注释，约 40-46 行）。
- 收益：voice 预算 8 → 12（`voice.c:316-318`）。代价：0.9.12 已把"崩溃过一次就永久单核"设为默认（`README.md:686-691`）。

### 代码与数据放在哪
- 链接脚本（`firmware/app.ld`）：
  - `XIP` 0x02000120 起，代码和常量就地执行。
  - `.text` 里**先放 `.text.a.*` / `.rodata.a.*`（中断可达的 350 个函数、244 张表，按名字排序），再放 UI**（`app.ld:31-33`，
    `tools/build.py:205-229`，`README.md:674-685`）。
  - `RAMTEXT` 0x01C6FC00、25 KB（在 pool 顶部）：`.ram_text`（flash 驱动）、`.c1_text`（核 1 循环）、`.hot_text`（核 0 最热的音频代码，22.8 KB）
    （`app.ld:5-9,22-27,34-46`，`README.md:768`）。
  - 关键细节：pi32v2 的 call 只能到 ±4 MiB，**RAM 代码放在 0x01C00000 根本够不到 XIP**，所以它们把 RAMTEXT 挪到 pool 顶部，
    并且因为音频代码排在映像最前，RAM 里的热代码调用它们一定在范围内（`app.ld:6-9`）。
  - `.hot_text` 的函数清单：`mix_block, mix_part_m0, fx_run0, perf_begin, perf_pre, perf_block, perf_master, slicer_bus`（`tools/build.py:190`）。
    实现方法：先生成优化后的 LLVM IR，用正则给这些函数改 section，再只跑代码生成（`tools/build.py:193-202,264-271`），
    因为"这个 clang 遇到 section 属性就不跨 section 内联，连 static inline 都不内联"（`core.h` FELUCCA_HOTRAM 注释）。
  - 大缓冲在 `.pool`（delay 128 KB、画布等），`.noinit` 存崩溃信息。
- 320 MHz：开机在 RAM 里、关中断、核 1 未启动时切换；在 `.noinit` 里放"切换中"标志，切换中复位就下次不再切（`hal/fm1_clk320.h`，
  `hal/fm1_multicore.h:55-60`）。

---

## 3. 音序器与演奏：为什么它"好玩、上手快"

| 机制 | 做法 | 出处 |
|---|---|---|
| 六条导航规则 | SELECT 走页，分区键跳组，PRESETS 只加载预置，KNOB 1 永远是"选哪个"，按住 = 该层，屏幕上下永远告诉你在哪 | `README.md:76-99` |
| 骰子无处不在 | 按住某键 + SELECT 掷该部分的骰子；显示 BEFORE / NOW 让你保留或取消；从工厂预置出发、只在其附近变（"随机是邀请"） | `README.md:138-141,274-288` |
| GEN 写整首 | HOME + SELECT：EXPLORE（无流派、打破所有规则）、RANDOM 或 43 个流派；按功能和声、贝斯对着底鼓写、动机发展的旋律 | `README.md:280-288` |
| MUTATE | 每轨 SOUND 和 PATTERN 每 1-32 小节自己变，在上一小节最后一个 16 分音符算好、落在 1 拍；ORBIT 围绕锚点变（几分钟都还是它自己），DRIFT 一路漂；可 REVERT | `project.c:1833-1840`，`README.md:38` |
| UNDO / REDO | GLO + G4 撤销上一个大改动（骰子、清空、载入） | `README.md:289-290` |
| 宏 M1-M4 | 每次开机按各轨声音掷两条路由；F3 只对正在响的轨重掷 | `README.md:291-294` |
| Punch-in FX | 按住 FX：REPEAT 1/8 1/16 1/32、LPF、HPF（音频）；OCT± 1/2 TEMPO DEC± STUTTER ATK+ ARP RANDOM（作用在音符上）；**全部量化到下一个 16 分**；REC 时录进每段 64 步的 lane；A#4 选作用于全部/合成/鼓；FX LATCH 给手不够用的人 | `README.md:296-305`，`perform.c:1-25`，`pfx.c:1-29` |
| 步进细节 | p-lock、chance、ratchet、slide、HUMAN（力度/时间/门随机）、REC 以 1/64 步记录离格、ROTATE / PITCH 平移整条序列且可自动化、全曲一个调式 | `README.md:306-317`，`core.h` P_HVEL 等注释 |
| 歌曲 | 8 个项目 × A-D 四段；ARRANGE 16 行按小节；live sections 下一小节切入；quick chain；SONG REC；每行可带"场景"（mutes、鼓组 mute、宏、punch-in） | `README.md:319-323`，`docs/TONIC-AUDIT.md` §4-5 |
| 旋钮加速 | 按每个刻度的毫秒数算倍率（40 ms ×2 … 5 ms ×16），上限随参数范围；反向、首两个刻度、单个抖动都只走 1 步 | `ui_input.c:415-447` |
| 组 mute | GLO 按住：KICK / SNARE / HAT / PERC 四组 mute，DRUM-X 每个声音也能单独 mute | `README.md:270,395` |

**它好玩的根本原因**：任何时候按住一个键就能"推一把"，推完总落在能用的地方（从预置附近随机、BEFORE/NOW、UNDO、MUTATE ORBIT 的锚点）；
而且推的动作本身可以录下来（punch-in lane、旋钮动作、SONG REC）。

### 屏幕
- 硬件：ST7789 240×240，SPI 12 MHz，轮询 DMA，无整帧缓冲，画到离屏条带再一次性 DMA（`lcd.c:1-11`，`gfx.c:4-11`）。
  条带最大 240×150（`gfx.c:81`）。
- 帧率：主循环最多 ~60 帧/秒（每帧至少 15 ms，`main.c:389`）；整屏 SPI 要 ~77 ms，所以 0.9.0 起每条带切成 8×4 的格子、
  每格存 32 位哈希，只发变了的格子（每帧 ~22 500 像素而不是 57 600，约 30 ms）（`gfx.c:320-330`，`README.md:1179-1183`）。
  负载 LITE / ECO 时 HUD 首页限到 50 / 70 ms 一帧（`ui_draw.c:2396-2410`）。
- 画法：抗锯齿 4 bit alpha 字体（构建时栅格化）、亚像素 Q4 抗锯齿线与 Bezier（`ui_organic.c`，`docs/TONIC-UI.md` "todo dibujado por código"），
  一帧约 400 条线（`ui_being.c:5-7`）。因为 20 张概念图做成位图会超出 app 区 113 KB，作者决定全部用代码画（`docs/TONIC-UI.md`）。
- 内容："热成像"调色（数值越高越热）；每个引擎画自己的"生物"，形状由四个旋钮决定（CUT 长刺、RES 膜振动、DTN 两个核分开、NOIS 毛边），
  随自己的输出电平变热膨胀，有脸、敲击时闭眼、嘴随电平张开；技术页画真实曲线：LFO 按速率跑、包络上有点跟着发声 voice 走、
  DIST 传递曲线、延迟回声、混响冲激响应（`README.md:122-149,182-219`）。数据都来自 UI 本来就读的 scope 缓冲和 voice 状态，
  音频侧不多做事（`README.md` 0.9.4 / 0.9.6 记录，见提取段落 325-348 行附近）。
- 动画：页面切换 9 帧整数 ease-out（`ui_draw.c:92-101`）。

---

## 4. 存储、预置、MIDI/USB、编辑器、Android

- **存储**：每个对象一对 A/B 扇区；写到不是当前的那份，先擦、写 payload、**最后写 32 字节头**（magic、seq、长度、payload CRC、头 CRC），
  加载时取 seq 最新的有效副本——任何时刻掉电都还有上一份（`storage.c:4-13`）。8 首歌 × 4 段 × A/B ≈ 66 扇区（`docs/TONIC-AUDIT.md` §5）。
  停止 10 s 无改动自动存（`README.md:356`）；擦 flash 时声音会断 ~50 ms，所以只在停止时写（`docs/TONIC-AUDIT.md` §4 "Guardado"）。
- **预置**：32 个命名用户预置；工厂预置带类型 BASS LEAD PAD KEYS PLUCK BELL VOX DRUM FX，可按类型跨引擎浏览（`README.md:356,722-726`）。
- **MIDI/USB**：class-compliant USB MIDI + USB 音频输入（44.1/48 kHz，48k 重采样放在核 1）、TRS MIDI、内/USB/TRS 时钟、MIDI LEARN 10 个、
  **STEMS**：每轨一个声道 8 声道（T1-T6 + FX L R）进 DAW，STEMS+IN 10 声道（`README.md:348-358`）。
- **网页工具**：安装器（Web MIDI）、编辑器（SysEx 协议 v1-v7：参数、步、motion、整机备份，`web/EDITOR_PROTOCOL.md`）、交互手册、
  **浏览器模拟器**：把固件源码按主机测试的方式编译成 WASM，AudioWorklet 出声，键盘鼠标可弹，flash 存在浏览器里（`BUILDING.md` "Browser emulator"，`web/emu/`）。
- **Android**：只有一个诊断 APK（JIANT Probe），看手机怎么处理 FM-1 的 USB 多声道音频（`android/README.md`），对我们无直接价值。
- **崩溃处理**：崩溃屏显示版本、哪个核、短码、五个寄存器；最后一次崩溃存进设置；`tools/crash_decode.py` 用照片上的数字反查函数（`README.md:698-704`）。

---

## 5. 能直接降 CPU 的工程技巧（JIANT 做了的）

1. **控制率 + 块内线性插值**：幅度 `amp_at` 用移位代替除法（注释特别说 -Os 下常量除法仍是硬件除法）（`dsp.c:113-129`）；
   包络每块一步、Q24 指数表（`voice.c:874-925`）；SVF 系数每块一次（`eng_analog.c:179`）；MORPH 每块限速再在块内插值（`voice.c:965-969`）。
2. **状态进局部变量**：`analog_render` 先把相位、滤波状态读到局部，循环完再写回（`eng_analog.c:174-177,222-226`）；分支放在循环外面
   （WAVE 不同走不同函数，`analog_dispatch`，`eng_analog.c:229-241`）。
3. **不响就不算**：sustain 为 0 的保持音不渲染（`voice.c:1032-1033`）；DRUM-X 衰减到 -84 dB 停算写零（`drumx_voice.c:19-20,370`）；
   效果层空闲时每级都跳过、输出逐位相同（`perform.c:23`）；DJ FILTER 居中时不进链（`README.md:341-343`）。
4. **半采样率**：DRUM-X 整体 22 kHz（`drumx_voice.c:181`），CLAP 的"手"每半速块一次（`drumx_voice.c:145`），混响预延迟和 CLOUD 半速、
   CLOUD 的颗粒每隔一个采样算（`fx.c:25-27,1052-1053`），长延迟半速 / 1/4 速并用 half-band 抽取滤波器（`fx.c:1236-1274`）。
5. **查表**：正弦 1024 点插值、tanh 表、SVF 的 g 表、1/16 半音的相位增量表、包络 LIN/EXP 表（`dsp.c`，`tools/gen_tables.py`）。
6. **voice 预算与偷音**：偷音顺序 = 最老的已释放 → 多余的 UNISON → 非最低音的保持音，永远不偷贝斯；被偷的 voice 用 6 块半余弦淡出（~4.4 ms）防爆音
   （`voice.c:1-14,340-386,893-918`）；负载紧时只砍 -50 dB 以下的尾音（`voice.c:386-405`）；超载连续两块才淡出一个（`audio.c:58-131`）。
7. **代码布局**：音频可达代码排最前（§2）、最热函数进 RAM（§2）、320 MHz 让 SFC 到 80 MHz（§2）。
8. **回归门禁**：golden 渲染哈希、callgrind 每采样指令数 +8 % 报警、目标机反汇编循环指令数 +10 % 报警（`BUILDING.md` Tests，`tests/`）。

---

## 6. FN ACID 现在是什么（为了对照）

- 4 条轨：T1 ACID（16/32 步 303 写法：音符 / tie / rest、accent、slide；或 AA 公式）、T2 ZOO（第二台 303 + 动物叫声预置；旧 BIO 波表动物）、
  T3 KICK / 909 / 808（X0X 移植的 9W9 / 8W8，float）、T4 KEYS（8 voice float：PAD PLUCK GUITAR EPIANO BELL PIANO DRONE）
  （`fn:dinghy.c:1-14,975-977`，`fn:dinghy.c:22-27`）。
- 303：Q15 四极梯形 + 反馈软削波 + 输出补偿，或 Chamberlin SVF（`fn:dinghy.c:1548-1576`）；第二台 303 同结构（`fn:dinghy.c:1577-1590`）。
- 效果：点八分乒乓 delay（int16 32K × 2）、4 comb + 2 allpass 的小房间（`fn:dinghy.c:866-909`）、母线 drive、kick duck；
  FX 按住 + 键：REPEAT / REVERSE / TAPE STOP / LP、HP 一小节扫频 / CRUSH，以及曲线运算 y+12、镜像、x/2、2x（`fn:dinghy.c:1631-1680`，README）。
- 音序 / 演奏：OCT+ 生成新线、OCT- 变异（TB-3PO 思路，`fn:dinghy.c:1963-2008,2088-2130`）；旋钮动作 4 小节 lane（`fn:dinghy.c:57`）；
  8 首内置 acid 歌曲与开机 demo（`fn:dinghy.c:31,1918-1960`）；6 个存储槽（`fn:dinghy.c:75-77`）。
- 屏幕："一线一色"浅色风格，16 步环 + 右侧 303 波形/谐波模拟 + scope，约 30 帧/秒，顶部 100 ms 一次（`fn:dinghy.c:2466-2890,3120-3132`）；
  GLO 第 1 页有 FULL / LITE / OFF 屏幕测试档和逐模块 CPU 剖析（`fn:dinghy.c:1891,2780-2796`）。
- 结构：音频全部在一个 ISR 函数里（`app_audio` 内联进 `fm1_alnk0_irq`，8.6 KB），每个采样依次算 kick、ACID、V2、riser、BIO、KEYS、delay、reverb、
  punch-in；909/808 按步沿分段块渲染（`fn:dinghy.c:1502-1516`）。
- 内存布局（`fn:firmware/app.ld`，`fn:build/syms.txt`）：RAMTEXT 在 0x01C00000、24 KB，只用了 2.8 KB（flash 驱动）；`.bss` 结束于 0x01C174C0，
  **RAM 还空 ~35.6 KB**；pool 用到 0x01C6FC9C，上限 0x01C74000，**还空 ~17 KB**。映像 287 KB。
- 音频代码在 flash 中的位置（`fn:build/syms.txt`）：`eval_acid0` 0x02002BB4、`control` 0x02003D9E、`d8_render` 0x02007E62、`d9_render` 0x0200ABB6、
  `fm1_alnk0_irq` 0x0200D31C……而 UI 的 `app_main`（18 KB）在 0x02014170，flash 驱动、USB 等夹在中间。音频热路径合计约 26-38 KB
  （ISR 8.6 + control 2.4 + eval_* ~4 + 909 或 808 渲染 9-11 + 杂项），**已经接近或超过 32 KB I-cache**，再加上 UI 每 33 ms 把 cache 冲一遍。

---

## 7. 差距

| 方面 | JIANT | FN ACID | 判断 |
|---|---|---|---|
| 303 专精度 | 无真 303（ANALOG 里一个 SVF 预置） | 梯形滤波、accent/slide/tie、两台 303、曲线运算 | 我们领先，别丢 |
| 渲染结构 | 每 voice 每 32 帧块，控制率 + 插值 | 每采样跑全部声部；KEYS float 每采样 switch | 我们的主要 CPU 浪费 |
| 代码位置 | 音频代码排前 + 22.8 KB 热代码在 RAM + 320 MHz | 全部 XIP，音频/UI 交错 | 最大、最便宜的收益 |
| 负载保护 | 预算 65 %/80 %、尾音剔除、淡出偷音、ECO 档 | 无（KEYS 偷"最安静的"但不看负载） | 便宜补上 |
| 鼓 | DRUM-X：整数、半速、MORPH、八度 ratchet/ghost/flam | 909/808 原样 X0X（float、全速、代码大）+ 函数 kick | 909 是 acid 正统，保留；只做半速/限 voice |
| 演奏层 | MUTATE、骰子 BEFORE/NOW、UNDO、量化 + 可录 punch-in、LATCH、场景、组 mute、加速 | OCT± 生成/变异、punch-in（不量化、不录）、motion lane | 可挑几个移植 |
| 母线 | CLIP 补偿、PUNCH、DUCK、leveler、DJ FILTER | drive、duck | leveler / DJ FILTER 对 acid 很值 |
| 存储 | A/B 掉电安全、8 歌 × 4 段、32 预置、自动存 | 单扇区，6 槽，20 s 无改动存 | 先做 A/B |
| 屏幕 | 脏格哈希只发变化、负载降帧、生物 + 技术图 | 每 33 ms 整条带重发 | 可借脏格 + 负载降帧 |
| 工具 | WASM 模拟器、golden、callgrind 预算 | host 模式出 WAV/PNG、JS 原型 | 借 golden + 指令预算 |

---

## 8. 建议（含工作量与风险）

### A. 性能（先做，按顺序）

**A0. 先测一个数**（0.5 天，无风险）
用现有 GLO 第 1 页：同一首歌分别在屏幕 FULL / OFF 下记 AUDIO CPU 平均与峰值，再看 `pfAcc` 各模块（`fn:dinghy.c:1417-1421,2780-2796`）。
FULL 比 OFF 明显高 = UI 冲 cache 的成本，那么 A1/A2 一定有效；差不多 = 瓶颈在音频代码自身体积，更该做 A2/A4。

**A1. 把音频可达代码排在 flash 最前**（1-2 小时，极低风险）
我们是单编译单元，已有 `-ffunction-sections`（`fn:tools/build.py:40`），不需要 JIANT 的 IR 改名那套：在 `fn:firmware/app.ld` 的 `.text` 里，
`KEEP(*(.text.crt0))` 之后先列 `*(.text.fm1_alnk0_irq) *(.text.control) *(.text.eval_acid0) *(.text.zoo_eval) *(.text.eval_bio) *(.text.call_shape)
*(.text.sg_lanes) *(.text.sg_lane) *(.text.midi_step) *(.text.on_step) *(.text.d9_*) *(.text.d8_*) *(.text.hat_tick) *(.text.dk_render) …`
和音频用到的表（`acid_tables.h` 里的 const、`SINT`、`INC64`、`BIO_WT`），再放 `*(.text*)`。这样改 UI 永远不会再挪动声音（JIANT 0.9.12 的教训）。
可选：写个小脚本从 `dinghy.dis` 算 ISR 的调用闭包，自动生成清单（JIANT `tools/size_fns.py isr_reach` 的思路）。

**A2. 最热代码进 RAM**（1-2 天，中低风险；失败模式多为链接期报错，USB 救援不受影响）
- 位置：pi32v2 call 只有 ±4 MiB，RAM 代码必须在 ≥ 0x01C46300 才能调用到我们整个 287 KB 映像（0x02000120 + 0x461A8 − 4 MiB）。现在的
  RAMTEXT（0x01C00000）调不到任何 XIP 代码，只能放完全自包含的函数。照 JIANT 把一个新 `HOTTEXT` 区放到 pool 顶部附近。
- 腾地方：把 `ksBuf`（16 KB）、`rvBuf`（11.9 KB）从 `.pool` 改到 `.bss`（`.bss` 还空 35.6 KB），pool 顶部就有 ~45 KB 给代码。
  （不要简单减半 `capL/capR`：PB_L4 在 80 BPM 要 33 075 帧，`fn:dinghy.c:925-932`。）
- 放什么：`fm1_alnk0_irq`（含内联的 app_audio）、`control`、`eval_acid0`、`zoo_eval`、`call_shape`、`kv_run` 所在函数（目前内联在 ISR 里）、
  当前用到的 909 或 808 渲染（两者合计 ~20 KB，可以只放 `d9_render`，808 留 XIP）。
- 怎么做：clang 不跨 section 内联，所以要么像 JIANT 在优化后 IR 里改 section（`tools/build.py:193-202,264-271`），要么只给"已经是 noinline 的大函数"
  加 `__attribute__((section(".hot_text")))`（它们本来就不内联，没有损失）。crt0 已有 `.ram_text` 的拷贝逻辑可照抄（JIANT `app.ld:46` 的 `_hot_load`）。
- 注意：flash 擦写期间 RAM 代码仍可运行，但不能调用 XIP——我们只在停止时擦写、擦写时关中断，ISR 不会在那时运行，这点和 JIANT 一样安全。

**A3. 320 MHz（SFC 80 MHz）**（0.5-1 天，中风险）
移植 `firmware/hal/fm1_clk320.h`（MIT，curist/fm1-clk320-probe）：开机、音频和 USB 起来之前、关中断、在 RAM 里调用 `fm1_clk320_switch`；
`fm1_clk320_check` 不返回 0 就不切。照 JIANT 在 `.noinit` 记"正在切换"，切换中复位过一次就以后都不切（`hal/fm1_multicore.h:55-60`）。
JIANT 报告约 -11 % CPU，并提供 MENU 开关。先做 A1/A2 再做它，便于分别测量。务必确认 USB 枚举、OTA、救援路径在 320 下都正常
（`fm1_clk320_check` 第 7 条：USB 在 PLL2 上时拒绝切换）。

**A4. KEYS 改为按 voice 分块渲染**（1-2 天，低风险，可在 host 模式 A/B 听）
- 结构：`kv_run` 现在每采样遍历 8 voice、每个都 switch（`fn:dinghy.c:1094-1184`）。改成每 16 或 32 采样一块：对每个活跃 voice 进它自己类型的紧凑循环、
  状态读进局部变量、写进一个 float 块缓冲；ISR 主循环里只加 `kbuf[i]`。
- 控制率：包络每块算一次、块内线性插值（JIANT `dsp.c:113-129`）；EPIANO/BELL/PIANO 的 `mi *= mdk`、`t2 *= …` 也改每块。
- FM 改整数相位 + 正弦表（PAD/DRONE 已经这么做了，`fn:dinghy.c:1124-1158`）：`fsin(float)` 每次一个 float→int 转换和取小数，
  EPIANO 每采样 3 次、PIANO 4 次。
- 预期：KEYS 是 float 部分里最重的；分块后分支和内存往返大幅减少，具体靠 `pfAcc[PF_KEYS]` 测。

**A5. 负载保护**（0.5 天，低风险）
- KEYS 的 voice 分配加"负载紧时不加新 voice / 先砍 -50 dB 以下的尾音"（JIANT `voice.c:386-424`）；用 `cpu_avg8` 做门限，65 % 进、60 % 出。
- 偷音时淡出 4-6 ms 而不是直接 `memset`（`fn:dinghy.c:1023-1035` 现在直接重置被选中的 voice，会有爆音）。

**A6. 先 ack 再渲染**（10 分钟，无风险）
`fn:firmware/src/platform.c:236-250`：把 `fm1_audio_ack_half()` 移到取得 `fm1_audio_free_half()` 之后、`app_audio` 之前（JIANT `audio.c:141-147`）。
平时不影响，满载时避免"吞掉一个半块再重播旧音频"的二次故障。

**A7. 半速渲染**（各 0.5-1 天，中风险：要听）
- 小混响半速：输入两两平均、混响每隔一个采样算一次、输出线性插值（JIANT CLOUD 的做法，`fx.c:1052-1053`）。混响本来就暗，损失几乎听不出。
- 909/808：只对 BD / SD / TOM 这类低频声部半速，HH / CY / RIM 留全速（22 kHz 的 Nyquist 11 kHz 会伤到镲片）。这需要动 X0X 的代码，
  要在 host 模式和原版逐个对比 RMS / 频谱。

**A8. 主循环里别再每帧跑 `fpu_sum(64)`，ISR 里的 `fpu_sum(256)` 改成每 N 块一次**（10 分钟，无风险；收益很小但白拿）
（`fn:dinghy.c:1681-1684,3112`）。0.34 的 FPU 检查已经得到结论，可以降频保留。

**不做：第二核**（3-5 天，高风险）。见 §0 末尾。如果以后真要做，照抄 JIANT 的三道保险：check word、超时、输出越界，并且核 1 只从 flash 跑。

### B. 声音（acid 为主，"还有些别的能用的声音"）

**B1. 母线 leveler + CLIP 电平补偿**（1 天，低风险）
慢速 2:1 rider、-9…+6 dB 自动补偿，放在限幅之前（JIANT `README.md:338-340`）。acid 的 RES 和 accent 一拧就差 6-10 dB，这个让现场音量稳定；
CLIP 加补偿后变成"加个性不加音量"，可以给 303 的 `mDrive` 用。

**B2. DJ FILTER：一个双极旋钮**（0.5 天，低风险）
左低通、右高通、居中完全旁路（不耗 CPU），可自动化（`README.md:341-343`）。我们已有 punch-in 的 LP/HP 一小节扫频（`fn:dinghy.c:1656-1673`），
把同一个 SVF 接到一个旋钮上即可，这是 acid 现场最常用的动作之一。

**B3. 303 的 A/B MORPH**（1 天，低风险）
借 DRUM-X 的思路（`drumx_voice.c:1-20`）：ACID 存两套声音（cut res env dec acc drive wave），一个 MORPH 旋钮在两者间逐值插值、每块读一次，
可录进 motion lane。我们参数本来就在控制率，几乎不加 CPU；它把"一个 loop 里只拧 cutoff"升级为"一个旋钮扫过整个音色"。

**B4. ZOO 加 ANIMAL 的 PULSE / CMOS 源**（1-2 天，低风险）
PULSE = 脉冲列进二极共振器（蟋蟀、蝉、青蛙），CMOS = 两个施密特 RC 振荡器互相门控扫频（鸟机），都是几行整数运算（`eng_animal.c:6-18`）。
放进 ZOO 的叫声预置，让"动物"更像真的、也更便宜；GPL-3.0，与我们同许可。

**B5. DRIFT**（0.5 天）：每 voice 慢速值噪声推音高 ±27 ct 和滤波（`voice.c:1038-1041`），给 KEYS 的 PAD / EPIANO 一点模拟漂移。

**B6. 909 的"八度 = 变奏"**（1 天）：按住鼓页时高八度键 = ratchet x2/x4/x8，低八度 = SHORT/GHOST/FLAM，录进步里（`README.md:267-269`）。
不改 X0X 的声音，只改触发。

### C. 演奏与音序

**C1. punch-in 量化到下一个 16 分 + 录进 lane**（1-1.5 天，低风险）
现在 REPEAT 已经按网格起（`punch_start`，`fn:dinghy.c:920-932`），但 FN 曲线运算（y+12、镜像、x/2、2x）是立即的。照 JIANT：按下等到下一个 16 分，
REC 时把按住的效果写进一条 64 步 nibble lane（`pfx.c:22-26`），存进槽位。这样 FN ACID 最有特色的"对曲线做运算"变成可编排的段落。

**C2. MUTATE ORBIT**（1-2 天，低风险）
每 1/2/4/8/16 小节、在上一小节最后一个 16 分算好，让 ACID 的音符沿调式上下、accent / slide 增删一两个，**围绕锚点**（开启时的线）变，
不累积跑偏；关掉时问"回到变异前？"（`project.c:1833-1840`，`README.md:38`）。我们已有 `mutate_line` / `mutate_steps`（`fn:dinghy.c:1991,2115`），
加一个定时器和锚点副本就行。对 acid 这是"一条线自己活几分钟"。

**C3. UNDO / REDO 一步**（0.5 天）：OCT+ 生成、OCT- 变异、清空、载入之前存一份 `params_t`（~500 B），GLO + 某键来回切（`README.md:289-290`）。
骰子类功能有了撤销，用户才敢按。

**C4. 旋钮加速**（0.5 天）：直接照抄 `ui_input.c:415-447` 的算法（按每刻度毫秒数、上限随范围、反向和首刻度不加速），替换我们鼓页里临时的 ×2/×3
（`fn:dinghy.c:2394,2432`）和其它旋钮。

**C5. FX LATCH**（0.5 天）：一个设置项，FX + 键 = 开 / 再按 = 关（`perform.c:19-22`）。单手演奏时很需要。

**C6. 歌曲行带场景**（视现有 songs8 结构，1-2 天）：每行存 mute、宏位置、punch-in（`README.md:319-323`，`docs/TONIC-AUDIT.md` §5 估每行 ~8 B）。
我们的 8 首 demo 歌已经有 lane 和段落（`fn:dinghy.c:1283-1328`），把"用户自己的歌"也做成这种结构是下一步。

### D. 基础设施

**D1. A/B 掉电安全存储**（1 天，低风险）：照 `storage.c:4-13`，两个扇区轮流写、头最后写、带 seq，加载取最新有效份。
现在 `settings_save` 先擦唯一扇区（`fn:platform.c:161-173`），擦完到写完之间掉电 = 全部设置和 6 个槽丢失。需要在 flash 白名单里多开一个扇区（`fn:platform.c:73`）。

**D2. golden + 指令预算回归**（1 天）：host 模式渲染每个预置/歌曲，存哈希；再用 callgrind 记每采样指令数，超 +8 % 报警
（JIANT `tests/regress.c`，`tests/cpu_vg.c`，`tests/target_budget.py`）。A1-A7 每一步都能用它确认"声音没变、指令少了"。

**D3. 脏格刷屏 + 负载降帧**（1 天，低风险）：8×4 格哈希只发变了的（`gfx.c:320-330`）；AUDIO CPU 高于某值时自动把 30 帧降到 15-20 帧
（`ui_draw.c:2396-2410`），等于把现在手动的 FULL/LITE 变成自动。注意它省的主要是 SPI 时间和 UI 代码冲 cache 的频率，A2 做完后后者就不重要了。

**D4. 固件编译成 WASM 的浏览器版**（2-3 天，低风险）：我们已有 host 模式（同一份 `dinghy.c` 出 WAV/PNG），离 JIANT 的 `web/emu/felucca_web.c` + AudioWorklet
只差一层胶水。好处是 `visual-interaction-lab/04-function-acid` 的 JS 原型不再和固件漂移，安装站可以直接"先在浏览器里弹"。

**D5. 崩溃信息持久化**（0.5 天）：最后一次崩溃 PC 存进 `.noinit` 或设置，开机提示（JIANT `README.md:698-704`）。

---

## 9. 建议的执行顺序

1. A6、A8（几分钟）→ A0 测量 → A1（小时级）→ 再测。
2. A2 → 测；A4 → 测；A5。
3. A3（单独一个版本，带菜单开关，方便用户退回 360）。
4. B1、B2、C1、C2、C3（声音和玩法上最"值"的五个）。
5. D1、D2 随时插入；A7、B3-B6、C4-C6、D3-D5 按需。

每一步都在 GLO 第 1 页记录 AUDIO 平均 / 峰值和各模块占比，版本号 +0.01，方便和上一版对比。
