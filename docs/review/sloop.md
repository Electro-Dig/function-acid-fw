# SLOOP 固件评审：FN ACID 能学什么

日期：2026-10-25。只读调研。SLOOP 源码快照：`scratchpad/repos/sloop`（`main` 分支，提交 `4ec1ac8` = SLOOP 2.7.1，已并入 sloop-lab 的 `dualcore` / `ram` 两个分支），下文写作 `sloop/…`；Felucca 底层写作 `felucca/…`；FN ACID（`D:\claude\function-acid-fw`，0.50）写作 `fa/…`。
旁证用了 X0X 的设计文档（`x0x/docs/plans/…`），因为我们的 909 / 808 就是 X0X 的移植。三者都是 GPL-3.0，代码可以带署名直接搬。

我们自己的数字（从 `fa/build/dinghy.elf` 读出）：镜像 287 KB / 约 580 KB；`.bss` 70,560 B / 98,304 B；`.pool` 326,812 B / 344,064 B；`.ram_text` 2,848 B / 24 KB；`.noinit` 76 B / 15.7 KB。音频热代码：`fm1_alnk0_irq`（app_audio 内联进去）8.6 KB、`d9_render` 8.9 KB、`d8_render` 10.9 KB、`control` 2.4 KB、`eval_acid0` 1.5 KB，再加 zoo / bio / kv_start 等约 4 KB，一块要跑的代码约 25–28 KB，贴着 32 KB I-cache。

---

## 0. 先给结论（按对 CPU 负载 / 可用性的影响排序）

| # | 建议 | 影响 | 工作量 | 风险 |
|---|---|---|---|---|
| 1 | **第二核心**：照 SLOOP 2.7.1 的 HAL 和启动顺序，把 909 / 808 交给 core 1（以后再 KEYS）。先把音频块拆成"各声部进缓冲 → 等 core 1 → 总线 / 母线"两段 | CPU 最大的一项：SLOOP 实测 31 %→23 %，X0X 鼓上 core 1 后 43→32 %、83→53 % | 2–4 天 + 实机 | 中（坑都已被踩过并写明） |
| 2 | **按键、MIDI 时钟进音频 ISR**（SLOOP `keyboard_block` / `mclk_event` 每 0.73 ms 跑一次），LCD SPI 从 12 MHz 提速 | 可用性最大：现在按键、时钟都在主循环里，主循环每帧被 LCD DMA 卡 30 ms 以上，按键到出声 / 时钟跟随有 30–60 ms 级延迟和抖动 | 1 天 | 低–中 |
| 3 | **存储照 SLOOP 的规矩**：擦除时把 DMA 缓冲清零（不嗡）；A/B 双扇区；播放中按 SAVE 只存 RAM（放 `.noinit`），停下且安静时才写 flash；更新 / UBOOT 前先落盘 | 消除"播放中保存卡一下 + 嗡"，断电不再丢掉全部 6 个槽 | 0.5–1 天 | 低 |
| 4 | **CPU 读数和过载保护**：扣掉嵌套的 TIMER5 时间、加"真丢块"计数（DMA 跑过去了）、连续两块 >85 % 就淡出最不重要的声部；去掉 ISR 里每块的 `fpu_sum(256)` | 读数可信；过载时不爆音 | 半天 | 低 |
| 5 | **按声部、按块渲染**（SLOOP 的 `engine_t.render(…, n=CTL=32)`），不再在一个 256 次的逐采样大循环里把所有声部交织 | 寄存器 / I-cache 局部性更好，也是 #1 的前提 | 2–3 天 | 中（要对声音做 host 对比） |
| 6 | 热表进 RAM（SINT / INC64 / EXPT / `x0x_tanh_tab`，约 12 KB），确认 I-cache 是瓶颈后再把 ISR 热路径放进空着的 RAMTEXT + "lowram"（约 21 + 8 KB） | 视实测而定 | 0.5 天（表）/ 2 天（代码） | 低 / 中 |
| 7 | 界面：按区域签名只重画变了的部分（SLOOP `draw_head` / `graph_signature`），UI 函数 `minsize` | 主循环更闲；镜像更小 | 1 天 | 低 |
| 8 | 编辑器 / 备份通道：沿用 Felucca/SLOOP 的 `F0 7D 46 4C` 帧规矩，先做 store 的整块备份 / 恢复（恢复要在机器上确认） | 用户数据安全，刷别的固件前能备份 | 2 天 | 低 |

建议的顺序：#4（先把尺子校准）→ 用现成的 GLO 第 1 页 SCREEN FULL / LITE / OFF 对比一次负载（判断 I-cache 被 UI 挤掉有多严重）→ #3、#2（小而确定）→ #5 + #1（大头）→ #6 视结果。

---

## 1. 第二核心（DUALCORE.md）

### SLOOP 怎么做

- **谁在哪跑**：一个音频块（CTL = 32 帧）里，core 0 先跑事件（`events_block`：音序器、按键、MIDI）和 DUCK，然后决定本块的分工 `units_split`，把"最后一个合成轨 + 鼓轨"交给 core 1，自己同时渲染其余合成轨；等 core 1 完成后把两套累加器相加，再串行做 TEXTURE、FREEZE、发送总线、DUST、PUNCH、DJ 滤波、母线（`sloop/firmware/src/fx.c:1167-1215`）。每个"单元"把干声 L/R 和三个发送写进自己的累加器组 `mixacc_t acc[2]`，scratch 也是每核一份 `part_buf[2][CTL]`（`fx.c:310-317`）。整数加法可交换，所以无论怎么分，输出逐比特相同。
- **同步**：X0X 的 HAL（`sloop/firmware/hal/fm1_cpu1.h`）。一次一个 job：core 0 写函数指针和参数，`csync`，`job+1`；core 1 在 RAM 里的等待循环（`.c1_text`，`fm1_cpu1.h:38-53`）看到 job 变化就执行（**job 本身从 flash 跑**），完成后写 `done`。每个方向只有一个写者，两核共享 cache，只需 `csync` 不需 flush。`fm1_cpu1_wait` 自旋等 `done`，**8 ms 超时**（`fm1_cpu1.h:109-122`）。
- **超时回退**：core 1 没按时完成 → 把它 hold 住，这一块 core 1 的单元在 core 0 上重算一次，此后整机单核（`fx.c:1190-1200`，`cpu1.c:24-31`）。
- **必须留在 core 0 的东西**：共享随机数的状态（两个 FORMANT 轨共用一个噪声 xorshift；PHYS 模型变化时的 `rng()`；LFO 本块要回绕的轨），留在 core 0 保证抽数顺序不变（`fx.c:895-920`）。2.6 合并时又审计出两处跨轨共享状态（ENV SYNC 的步长、预设淡变的 scratch），改成每轨 / 每核一份（DUALCORE.md "Status" 11/10/2026 一段）。
- **开机**：在音频和定时器之前启动 core 1（X0X 发现晚启动就永远到不了入口），启动期间打开 PC 限制（它要经过芯片 ROM），启动后跑一个自检 job（64 步 LCG，`cpu1.c:38-65`），ABOUT 显示 `2 CORES` / `1 CORE`；菜单 2ND CORE 开关，下次开机生效（`cpu1.c:46-66`，`main.c:226`）。
- **踩过的坑**（都有文档）：
  1. **core 1 的栈**：rti 之后跑在 usp 上，要给真栈；X0X 起初给 256 B，渲染跑穿进了全局变量（`x0x/docs/plans/2026-10-04-dual-core-and-clock.md` 开头 "Done" 一节）。SLOOP 给 2,816 B，开机填标记，控制台报 `cpu1_stack_free`（`cpu1.c:93-100`）；X0X 实测最深 828 B。
  2. **`.c1_text` 必须进镜像**：X0X 起初漏了，core 1 跑进擦空的 flash（0xFFFFFFFF）。SLOOP 的 `tools/build.py:203,212` 专门 objcopy 它并拼到镜像里。
  3. **hold core 1 时的 bus-invalid 陷阱**（2.7.1，`sloop/firmware/hal/fm1_sys.h:134-165`）：hold 会切断 core 1 的总线访问，芯片几条指令后报 bus-invalid（DBG_MSG bit 21），陷阱开着时 core 0 吃这个异常，表现为 mix_block 里 FELUCCA CRASH + 重启。修法：关陷阱 → hold → 空转 256 次等标志 → 清标志 → 恢复陷阱。重启、进 UBOOT、进更新前都走这个 `fm1_cpu1_halt`。
  4. **供电**（2.7，`fm1_sys.h:79-99`，`main.c:369`）：bootloader 留下 SYSVDD 1.26 V / VDC14 1.40 V，两核同时忙时有的机器读 flash / SRAM 出错；开机第一件事把它们抬到 M-VAVE 原厂固件用的 1.38 V / 1.60 V（逐级、每级 100 µs）。
  5. flash 写：core 1 空闲时在 RAM 循环里，只在音频中断里有活，而写 flash 时中断全关，所以写 flash 不需要"停车"（`fm1_cpu1.h` 文件头注释）。
- **测试**：`cpu1.h` 的三个调用在 host 上有替身（`tests/cpu1_host.c`）：内联、POSIX 线程、随机延迟（CHAOS）、故意丢 job（TIMEOUT），加 ThreadSanitizer；182 个 golden 渲染在所有模式下哈希一致。设备只需回答"时间"。
- **换来了什么**：作者的一个 pattern，**2ND CORE OFF 31 % → ON 23 %**（core 0 省了 26 %）；空闲 `cpu_pct 9`、`audio_max_us 1273`（DUALCORE.md "Status"）。SLOOP.md 2.7 说"处理器用完前的余量约翻倍，多声部 + 多轨 INSERT 不再爆音"。X0X（鼓上 core 1）：出厂 43→32 %，五部全开 83→53 %，最坏 115→81 %；core 1 成了关键路径后重新分工到 39 / 54 / 68 %（X0X 文档同上）。

### FN ACID 现状

单核。`fa/firmware/hal` 没有 `fm1_cpu1.{h,S}`，`fm1_sys.h` 没有 `fm1_cpu1_halt` 和供电函数，`app.ld` 没有 `.c1_text`，`build.py` 只拼 `.ram_text` 和 `.data`。`app_audio`（`fa/firmware/src/dinghy.c:1489-1690`）是一个 256 次的逐采样循环，里面把 ACID、第二只 303、BIO、KICK、KEYS（float，8 声部）、延迟、房间混响、punch-in、母线全部交织；只有 909 / 808 是先按块渲染进 `drBuf`（按本块的步边界切段，`dinghy.c:1502-1515`），这一点已经很接近能交出去的形状。

### 差距与建议

1. **移植 HAL（S，低风险）**：从 SLOOP 2.7.1 原样拿 `hal/fm1_cpu1.h`、`hal/fm1_cpu1.S`、`fm1_sys.h` 的 `fm1_cpu1_halt` / `fm1_supplies_stock` / `fm1_core_reset` 改动、`fm1_guard.h` 的 `fm1_guard_pc_open` / `fm1_guard_top_locked`；`app.ld` 加 `.c1_text` 并让 `_rt_end = _c1_end`（PC 守卫窗口要覆盖它）；`build.py` objcopy `.c1_text` 并拼进镜像；`fm1_cstart` 里先抬供电，`cpu1_boot()` 放在 `fm1_audio_init` 之前。GLO 第 1 页显示 2 CORES / 1 CORE 和 core 1 负载。注意我们的 `mmio_check` 只允许寄存器访问出现在 `hal/`：SLOOP 的写法本来就满足。
2. **先拆块（M，中风险，本身也有收益）**：把 `app_audio` 拆成"第 1 遍：各声部 → 本块的 mix / wet 缓冲"和"第 2 遍：加鼓 → sat / drive → punch-in → 母线"。现在鼓在第一时间就加进 `mix`（`dinghy.c:1531-1534`），之后要过 `sat()`、`md`、punch 捕获，所以 core 0 必须拿到鼓缓冲后才能做第 2 遍。
3. **鼓交给 core 1（M）**：core 0 先扫本块的步边界，把 `drum_step` 拆成"算出这一步要打哪些鼓"（有副作用的部分：`sg_map`、`duckEnv`、静音、歌曲鼓谱，全部留在 core 0）和"触发"（`drum909_trigger` / `drum808_trigger`）。交给 core 1 的 job：按 (偏移, 鼓, 力度) 列表触发并渲染进 `drBuf`，再做歌曲里的鼓高通。`drum_apply`（改参数）只在交出去之前做。每半缓冲只交一次（256 帧），同步开销比 SLOOP 每 32 帧一次还低。我们的 float 加法顺序不变（core 1 写自己的缓冲，core 0 在同一位置相加），所以声音与单核逐比特相同。
4. **KEYS 第二步（L）**：KEYS 现在在 `control()`（每 16 帧）里接收主循环的按键队列、在 `on_step` 里被 `perc_hit` 启动，`kv_run` 还吃每采样的 `duckEnv`（歌曲 pad 的泵）。要上 core 1 必须先做带采样偏移的事件列表，并把 pad 的泵改成在 core 0 混音时乘（或 pad 单独一个缓冲）。X0X 的经验是先测清 core 0 / core 1 谁是关键路径再分。
5. 照抄 SLOOP 的保护：8 ms 超时回退 + 之后单核；`cpu1_stack_free`；`2ND CORE` 开关；2.7.1 的关陷阱 hold。

---

## 2. RAM 与 XIP（RAM.md）

### SLOOP 怎么做

- **内存图**（`sloop/firmware/app.ld`）：代码和常量全部 XIP（0x02000120 起）；RAMTEXT 0x01C00000–0x01C07F00 放 `.ram_text`（flash 驱动，flash 关着时要跑的代码，构建检查它**不得有任何 call**）、`.c1_text`（core 1 等待循环，60 B），以及 sloop-lab 新开的 **`.lowram`**（`*(.bss.c1stack) *(.lowram*)`）；RAM 96 KiB 放 `.data/.bss`；POOL 0x01C20000 起 336 KiB 放大缓冲（`.pool`）；NOINIT 15.7 KiB 跨复位保留；两个栈 24 KiB + 8 KiB，带 256 B 守卫带。
- **`.lowram`**：Felucca 给"RAM 里跑的调试版"留的 0x01C00BD0–0x01C07F00 约 29.5 KB，SLOOP 没有那种构建，就拿来放混响延迟线、core 1 的两个栈、分核用的第二套累加器 / 鼓缓冲 / scratch。开机逐字写入取反地址再读回验证、再清零（`main.c:18-33`），控制台报 `lowram 1/2`。注意 crt0 在 0x01C07F20 写 SPL 状态，所以止于 0x01C07F00。
- **热代码不进 RAM**：SLOOP 没有把 DSP 放 RAM，`.ram_text` 只有 flash 驱动；它的 DSP 全是整数（FM6、PHYS 都改写成定点，`fm6_core.c:4,23`、`phys_dsp.c:35`），构建不开 FPU。为了镜像装得下，2.7 把所有 UI 函数标成 `UI_SMALL = __attribute__((minsize))`（`gfx.c:6-15`），音频路径保持 `-Os`。
- **省 RAM 的手法**（RAM.md 表格）：`midi_sel_on` 压成半字节；**`scratch.h`**：主循环里从不同时活着的短命缓冲（存储载荷、可视化快照、示波器、OTA 帧）共用一块 union；**用户预设库直接从 flash 的明文 XIP 窗口读**（`st_xip`），不再在 RAM 里留镜像；DRAWBAR 状态放进引擎 arena（GRAIN / FM6 / PHYS 每轨共用一块，一轨同时只有一个引擎）。RAM 区从 96,524 B 降到 58,220 B。
- **栈实测**（`main.c:35-62` 开机涂色，控制台 `stack_free`）：主循环在 24 KB 主栈上最深 1,364 B，中断在 8 KB 监督栈上最深 856 B。
- 区段的规矩写在 `scratch.h`：只能主循环用、一次调用内用完、绝不在 ISR / TIMER5 里碰；flash 访问不在绘制里、绘制不在 flash 访问里。

### FN ACID 现状

`.bss` 70.5 KB（还剩约 27 KB），`.pool` 327 KB（剩约 17 KB），RAMTEXT 只用了 2.8 KB（0x01C00B20–0x01C07F00 约 29 KB 空着，我们的 `crt0.S:15` 也在 0x01C07F00 写状态，边界相同）；NOINIT 只用 76 B。大块：`dbufL/R` 各 64 KB（立体声延迟）、`capL/R` 各 64 KB（punch 捕获，立体声）、`cv_px` 52.8 KB、`ksBuf` 16 KB、`rvBuf` 11.9 KB、`PADT` 8.2 KB、`D9` 7.5 KB；**store 有三份**：`ST`（4,026 B）、`settings_load.buf`（4,084 B）、`oldRaw`（4,084 B），后两者只在开机用。热表都是 `const`，在 XIP：`SINT`（2 KB）、`INC64`（3 KB）、`EXPT`、`BIO_WT`（2 KB）、`x0x_tanh_tab`（4 KB，909 每采样的驱动）、`x0x_pot_exp`；909 的镲 / 踩镲 μ-law 采样（约 110 KB）也在 XIP，顺序读。

### 差距与建议

1. **热表搬进 RAM（S，低）**：开机把 `SINT / INC64 / EXPT / x0x_tanh_tab` 拷进 `.bss`（约 12 KB），音频代码用 RAM 副本。不改声音。用 GLO 第 1 页前后对比负载。
2. **合并开机缓冲（S，低）**：`settings_load.buf` 和 `oldRaw` 合成一块 scratch（SLOOP `scratch.h` 的办法），省 8 KB。
3. **证实 I-cache 是瓶颈后再搬代码（M，中）**：用现成的 SCREEN FULL / LITE / OFF（`dinghy.c:2398-2402`）对比音频负载；差得多就说明 UI 代码在两块之间把音频代码挤出 I-cache。那时把 `fm1_alnk0_irq` + `control` + `kv_run` / `reverb` 这类不调用 XIP 的叶子放进 RAMTEXT（约 13 KB，空间够），规矩同 `.ram_text`：RAM↔XIP 超出 23 位调用范围，只能整棵调用树搬，或用 `FL_FAR` 间接调用；PC 守卫窗口要扩到覆盖它；`build.py` 已有的"`.ram_text` 不得有 call"检查可以照用到新段。注意：这并不能让音频在擦 flash 时继续响（909 采样和查表还在 flash 上），别往这个方向走。
4. **ST 放进 `.noinit`（S，低）**，见第 4 节。
5. 可选：`stack_paint` / `stack_free` 照搬（S），给以后挪栈、扩 pool 留依据。

---

## 3. 音频引擎

### SLOOP 怎么做

- **结构**：4 轨 = 3 个合成轨（12 种引擎，共享 8 个声部预算）+ 鼓轨（16 音、6 声部）。引擎是一张函数表 `engine_t`，`render(track, voice, out, n, vmod)` **按声部、按块**渲染（`core.h:155-181`）；控制速率在块边界算一次 `vmod_t`（相位增量、Q15 振幅从块头到块尾的斜坡、截止、形状），`CTL = 32` 帧（0.73 ms），一个 256 帧半缓冲（5.8 ms）分 8 个控制块（`audio.c:66-67`）。
- **全整数**：Q15 / Q24，没有 float；延迟 65,536 个 int16（1.49 s）、合唱 2,048、混响是 4 条线的 FDN + 2 个全通（int16），每轨 DIST / INSERT / SLICER / 一钮滤波，三条发送总线，母线：TEXTURE、FREEZE、DUST、PUNCH、DJ 滤波、**看前一块的限幅器**（`fx.c:55-80`），最后软削波。
- **CPU 读数**（`audio.c:46-96`）：每半缓冲计时；**扣掉嵌套进来的 TIMER5 时间**（`t5_nested_ticks`，按键扫描 / USB 音频）当作负载；截止判定则不扣；1/16 平滑成 `song.cpu_q8`；记最大值；**DMA 在渲染期间已经走过这半块就记一次 `late`**（真丢块）。
- **过载**：连续两个半块超过 85 % → 下一块开头 `shed_voice()`：先淡出最安静的、正在释音的声部，再别的，**永不砍贝斯和主旋律**；单个慢块（USB 突发、写 flash）不砍（`audio.c:47-86`，`voice.c:139-175`）。用户手册里写成"音符淡出 = 处理器到顶了"。
- **测试**：`tests/regress.c` 渲染每个引擎和预设，对比哈希，并用 callgrind 数**指令数**当 CPU 预算（`tests/cpu_baseline.txt`、`target_budget.txt`，BUILDING.md "Tests"）。

### FN ACID 现状

- 逐采样大循环：每个采样依次跑所有声部、延迟、混响、punch、母线；`control()` 每 16 采样一次（`dinghy.c:1522`），一块 16 次；KEYS 在 `kv_run` 里每采样遍历 8 个声部并按类型 switch（`dinghy.c:1094-1183`）；909 / 808 是按块的 float。
- CPU 读数 `cpu_us`（`dinghy.c:1685`）从 `app_audio` 开始到结束，**包含**嵌套的 TIMER5（10 kHz 按键扫描 + 1 kHz USB 音频服务）；**每块还跑一次 `fpu_sum(256)` 自检**（`dinghy.c:1682`），它也算进负载。没有丢块计数，没有过载保护（过载 = 爆音）。我们有 SLOOP 没有的分项剖面（GLO 第 1 页：DRUM / CTL / KEYS / FX），这很好。

### 差距与建议

1. **校准读数（S，低）**：TIMER5 嵌套时把时间累计到一个变量（SLOOP `main.c:94-100`），`cpu_us` 减掉；`fpu_sum` 只在开机后几秒跑或挪到主循环；加 `late` 计数（渲染前后比较 `fm1_audio_free_half()`），和 CPU 一起显示。
2. **过载保护（S，低）**：连续两块 >85 % 时先淡出最老的 KEYS 声部，再关混响输入（尾巴照响），ACID 永不砍。
3. **按声部 / 按块渲染（M–L，中）**：把每个声部写成"对 n 个采样跑一个紧循环、状态放局部变量、末尾写回"的函数，控制量在块边界算（ACID 的梯形滤波系数本来就是在 `control()` 里每 16 采样更新一次，正好对应 16 或 32 帧的子块）。好处：状态留在寄存器里，不再每采样读写 `E.*`；每段代码连续跑 16–256 次，I-cache 命中好；也是第 1 节分核的前提。要用 host 渲染逐采样对比（定点部分可以做到逐比特一致）。
4. 控制速率从 16 改 32 帧会改变声音（包络台阶），先看剖面里 CTL 占多少再决定，不建议盲改。

---

## 4. 存储：保存不卡声音

### SLOOP / Felucca 怎么做

- **A/B 双扇区**（`sloop/firmware/src/storage.c:1-12`）：每个对象两份；写到"不是当前那份"的扇区：擦 → 从偏移 256 写载荷 → **最后写 32 字节头**（magic、类型、copy、seq、长度、载荷 CRC、头 CRC）；读时取头有效、seq 最大的一份，载荷 CRC 不对就退到另一份。写一半断电，旧的那份还在。
- **擦除时静音**（`sloop/firmware/src/felucca.c:96-112`）：关中断、**在同一个关中断窗口里把 DMA 双缓冲清零**、再擦。否则 DMA 会把最后渲染的半块循环播放整个擦除期间（几十 ms 的嗡声）。读 flash 分成 256 B 的关中断小窗（`felucca.c:62-76`），音频跟得上。
- **播放中从不写 flash**：
  - 段落（A–D）SAVE + 键：立刻存进 RAM 的 `proj_slot[4]`，**这个数组在 `.noinit`**（`project.c:152`），复位也不丢；标脏，等**停止 + 没有声部在响 + 1.5 s 没碰面板**才写 flash，失败 5 s 后重试并提示 SAVE ERROR: RETRYING（`project.c:1088-1186`）。
  - 自动保存：停止 + 2.5 s 无输入 + 距上次至少 20 s + 内容哈希变了 + 没有声部在响（`project.c:670-708`）。
  - 用户预设 / 段落的 LOAD/SAVE 页面：播放中直接说 STOP BEFORE SAVE；编辑器写 flash 前也要求先停（2.4 修过"编辑器播放中写 flash，掉音约 50 ms"）。
  - **会复位的动作之前先落盘**：更新、UPDATE MODE、主机发 UBOOT 都先 `persist_flush_now()`（`project.c:1125-1136`，`main.c:301, 318, 329`）。
- **备份 / 恢复**（编辑器协议 v6，`editor.c:402-620`）：`BK_LIST` 先拍快照并列出每个对象的长度和 CRC-32；`BK_GET` 分块读；`BK_PUT` 分块写进 RAM 暂存（借用项目缓冲），整体 CRC 对上才提交；新版本的备份恢复进旧固件"在写任何东西之前就停下"（SLOOP.md 2.6 "Safer backups"）。X0X 的做法更保守：恢复时机器自己拉镜像、校验每段 CRC，屏幕上问 RESTORE FROM USB? 按键确认才提交（我们的 `docs/editor-design.md` 第 1 节已记录）。
- flash 还有大片空间：SLOOP 用 0x97000–0x9FFFF 放段落和自动保存、0xA0000–0xDBFFF 放用户采样（`storage.c:22-27`）。

### FN ACID 现状

一个 4 KiB 扇区（0xFC000）放 `store_t`（当前 + 6 槽），头最后写（这点对），但**没有 A/B**：写到一半断电，头无效 → 开机回到默认，6 个槽全丢。`settings_save`（`fa/firmware/src/platform.c:161-172`）擦除时**没有清 DMA 缓冲**，所以"卡一下"其实是约 50 ms 的 5.8 ms 循环嗡声。**播放中也会写**：SAVE 单击（`dinghy.c:2378`）和 SAVE + 白键存槽（`dinghy.c:2309`）都立刻 `save_all()`；只有自动保存是"停止 + 20 s 无改动"（`dinghy.c:3141`）。`ST` 在 `.bss`，崩溃 / 看门狗复位会丢掉还没写的改动；进更新、UBOOT 前不落盘。版本迁移靠"按几种旧尺寸逐个试读"（`dinghy.c:3002-3024`），新版忘了加一条就会静默回到默认。

### 差距与建议

1. **擦除静音（S，低）**：照 `felucca.c:96-112` 在 `platform.c` 加 `audio_silence()` + `fl_erase4k_quiet()`，`settings_save` 用它。嗡声变成干净的短静音。
2. **播放中只存 RAM（S，低）**：SAVE / 存槽在播放中只改 `ST` 并提示"SAVED, FLASH WHEN STOPPED"之类；写 flash 的条件改成 SLOOP 的：停止 + ACID / KEYS 振幅都到底 + 1.5 s 无输入。
3. **`ST` 放 `.noinit` 并带 CRC（S，低）**：开机先看 noinit 副本是否有效且比 flash 新，是就采用并标脏。崩溃 / 看门狗之后改动不丢。在 `platform_poll` 的 `ota_session` 和 UBOOT 分支前调用一个 app 提供的 `app_flush()`。
4. **A/B（S，低）**：0xFC000 / 0xFD000 两份（Felucca 的 globals 区 0xFC000–0xFEFFF 本来就是给设置的），头里加 seq；`FL_RANGE_OK` 放开第二个扇区。
5. **真正在播放中写也不卡（M，中，可选）**：A/B 的另一份在"停止时"提前擦好；播放中保存只做页编程（16 页 × 256 B），**每次在一个音频半块刚结束后写一页**（主循环看半块计数器变化），每页关中断约 0.5–3 ms（`fl_prog_ram` 会回报 `took_us`），在 5.8 ms 双缓冲里放得下。上机前用 `took_us` 实测最坏值。
6. **版本化（S，低）**：头里存版本和长度，加载时拷 `min(len, sizeof)`，其余字段补默认值；`params_t` 只许在末尾追加（SLOOP 的"按数量映射"规矩，`core.h:62-63` 注释）。
7. 更多槽 / 歌曲：0x97000–0xDFFFF 对我们完全空着，以后可以一槽一对扇区。

---

## 5. 音序器手感

### SLOOP 怎么做（GUIDE.md §3、§8、§9、§13、§14、§18）

- **单击 = 翻页，按住 = 层**：按住任一功能键，16 个白键和 4 个旋钮换工作，0.14 s 后屏幕画成 4×4 格；**按住 + HOME 锁定层**（两手都空出来：一手按键一手拧旋钮），PLAY / REC / OCT 在锁定层里照常；键 1、5、9、13 暗亮当"盲摸路标"；层里 ALGORITHM / PRESETS 旋钮先等着，松开时不跳值。
- **录音**：播放中按 REC 立即叠录；停着按 REC 待命，第一个音符就是第 1 步；空工程是"自由拍"（按下一小节的 1 收尾，循环长度决定速度，3 % 以内保留原速度）；1 小节预备拍；**录音补偿按键延迟**：落点往前挪 `REC_LAT = 512` 采样（约 11.6 ms，`seq.c:475-490`）；QUANT off 把偏离量存成该步的 nudge。按住 REC 2 s 清轨（0.7 s 后出现进度环），EDIT + OCT−/+ 撤销 / 重做一层。
- **Punch-in**：27 个，白键 16 + 黑键 11；循环类对齐网格，开关都交叉淡化 64 采样；单声道环形缓冲 0.74 s（`punch.c:1-60`）；FX + OCT− / OCT+ = 混响 / 回声 THROW（发送量约 12 ms 滑到满，松开滑回，不改存档值）；FX 层里 KNOB 1–4 = 母线滤波 / DUST / DUCK / 当前轨滤波；2.7 还能从一个 MIDI 通道弹这 27 个效果。
- **混音层**：GLO + 1–4 静音、5–8 独奏、9 按住加花、10 下一小节加花、16 拍速；静音 / 解除静音约 6 ms 淡变（`fx.c:318-326`），不咔哒。
- **段落与歌曲**：SAVE + 1–4 在**下一小节**切到 A–D；按住 SAVE 连点几个段落 = 快速链（最多 8 个，循环）；SAVE + 14 边弹边录歌曲结构；歌曲模式 PLAY 播完回到循环。
- 其它：旋钮加速（快拧跨度大，2.7）；收藏音色；编辑器的 LIVE 页（整场演出用鼠标 / 平板控制）。

### FN ACID 现状

已有：按住层（`has_layer`，0.16 s 后画键位图）、punch-in（loop 1/4–1/32、反转、磁带停、LP/HP 扫、crush、对曲线本身的运算）、回声 THROW（`dinghy.c:2207-2236`，是把 `P.dMix` 直接设成 127）、旋钮动作录制（4 钮 × 64 步）、32 步、6 个槽、8 首演示歌、KEY LOCK。按键、编码器都在主循环处理（见第 7 节，延迟大）。

### 差距与建议（按现场价值）

1. **按键延迟**：见第 7 节，先解决它，后面的手感建议才有意义。
2. **录音延迟补偿（S，低）**：`key_down` 写 lane 时按"实际延迟"往前挪（SLOOP 用 512 采样；我们按键进 ISR 之前主循环延迟更大，先测）。
3. **锁定层（S，低）**：按住 FX + HOME 锁住，两手空出来同时弹 punch 和拧旋钮。
4. **THROW 做成滑动 + 加混响 THROW（S，低）**：现在 `P.dMix` 一步跳到 127，可能有咔哒；改成 ~12 ms 斜坡，FX + OCT− 给混响。
5. **槽在下一小节切换 + 快速链（M，低）**：现有 6 个槽相当于 SLOOP 的段落；播放中按住 SAVE 点白键 = 下一小节切过去，连点 = 链。
6. **一层撤销（M，低）**：步编辑 / 清除 / 生成新旋律之前存一份 `P` 的步数据，EDIT + OCT− 恢复。

---

## 6. MIDI、USB、编辑器 / 安装协议

### SLOOP 怎么做

- **MIDI 输入在音频 ISR 里处理**：`events_block` 每个控制块（0.73 ms）清空 USB / TRS 的 MIDI 队列（`seq.c:2824` 起，`:2985-3010`）。**时钟**（`seq.c:2707-2788`）：音序器按收到的脉冲推进，在两个脉冲之间按上一间隔插值、但**绝不越过下一个脉冲**；落后时以 2 倍速追，不突跳；每 24 个脉冲量一次速度（40–240 BPM）；0.5 s 没脉冲就回到内部时钟；START 从头、CONTINUE 接着、STOP 停；SYNC 选 USB 或 TRS；IN = CLOCK 只跟时钟不收音符。
- **MIDI 输出从 ISR 直接入队**：`midi_out_event()` 本来就写着"from the audio ISR"（`usb.c:162-169`），TIMER5 的 2 kHz `usb_poll` 发出去；2.7 的**时钟输出**每块检查，"落在哪块就在哪块发，晚不到 1 ms"（`seq.c:2791-2822`），停着也发，让别的设备先知道速度。
- 每轨独立通道、CC 表（7/10/71–75/74/91/93/94）、MIDI LEARN（16 个）、Program Change、延音踏板、弯音、FX 通道、REMOTE（CC 102–115 按 14 个按钮）、**TRS MIDI IN**（UART，`midi_uart.c`，TIMER5 每 0.5 ms 轮询）。
- **USB 音频**：与我们同一份 `usb.c`（44.1 / 48 kHz，48 kHz 用 160/147 多相 FIR）；2.7 加了 **USB RATE** 菜单（AUTO / 44.1 / 48，给录音失真的手机）和 USB AUDIO = MASTER / FULL（FULL 走自己的限幅器 `lim_usb`）。
- **编辑器协议**（`editor.c:1-50`，v12）：`F0 7D 46 4C cmd …  F7`；14 位值（两字节 +8192）；大块用 pack7；一问一答、同 cmd 回复；WATCH 打开后机器主动推 CHANGED / RELOAD / STEP_CHANGED，编辑器每秒 PING；只往回复末尾追加字节来升级。帧经 `usb.c` 的 `sx_frame` 进来，主循环里 `ed_service()` **必须在 `ota_service()` 之前**取帧（`main.c:312-313`）。编辑器还能把固件的 DSP 编成 WASM 在浏览器里试听（BUILDING.md "Browser audio preview"）。
- **安装器**：写入前校验包的 SHA-256；loader 启动新固件前再校 CRC；只续装自家 loader；OCT− 开机救援；能刷回官方 V15。

### FN ACID 现状

USB-MIDI 进出；**时钟输入在主循环里处理**（`dinghy.c:3094-3107`），用 `fm1_ms` 打时间戳估速度（80–170 BPM，`P.bpm` 是 uint8），音序器"不越过收到的 tick"的思路和 SLOOP 一样（`dinghy.c:1196-1208`），但 tick 要等主循环来取；**MIDI 输出**（音符、CC、时钟）由 ISR 进 `mqOut`，**再由主循环 `midi_send`**（`dinghy.c:3090`）。主循环一帧被 LCD 卡 30 ms 以上（第 7 节），所以时钟跟随和时钟输出都有几十 ms 的延迟和抖动。没有 CC 输入、没有 TRS（`FELUCCA_UART 0`）、没有编辑器通道（`docs/editor-design.md` §3 已写明）。安装器是 SLOOP 2.7.1 的代码。

### 差距与建议

1. **MIDI 输入进 ISR（S，低）**：`app_audio` 开头清空 `midi_in_q`（ISR 成为唯一消费者），F8 / FA / FB / FC 在那里处理（可直接搬 `mclk_event` / `mclk_adv` 的思路：按脉冲推进、插值不越界、2 倍速追），其它消息转进一个小环给主循环。
2. **MIDI 输出直接 `midi_out_event`（S，低）**：ISR 是唯一生产者；主循环如果还要发，就也走 ISR 那条队列（或关中断包住），保证单写者。时钟输出从 30–50 ms 抖动降到 ≤1 块。
3. **TRS MIDI IN（S–M，低）**：搬 `midi_uart.c`，`FELUCCA_UART 1`，TIMER5 里加 `owed & 2` 那一路（SLOOP `main.c:85-108`）。
4. **编辑器通道（M，低）**：按 `docs/editor-design.md` §4 的 "FA" v1，但帧格式、14 位值、pack7、一问一答、只追加字节这些规矩照 Felucca；`platform_poll` 里在 `ota_service()` 之前放 app 的钩子。第一版只做 INFO / GET / SET / 步数据 + 整块 store 的备份 / 恢复（恢复先整体 CRC，再在机器上按键确认）。
5. USB RATE / FULL 菜单（S）：用户录手机时有用，排后面。

---

## 7. 界面绘制

### SLOOP 怎么做

- **一块小画布**（240×124，`.pool`），每个区域画进画布再一次 DMA 发出；DMA 发着的时候主循环可以干别的，下一次碰画布前才 `lcd_sync` 等（`lcd.c` 文件头）。
- **签名缓存，只画变了的**：标题行、四列旋钮、图形区、底栏各算一个签名（播放状态、数值、消息、颜色……），和上次相同就整块跳过（`ui_draw.c:40-55, 124-165, 680-760, 817`）；切屏时 `ui.force` 全画一次。
- **主循环节拍**：一帧最多约 60 次 / 秒，剩余时间里反复 `ui_input()` 和 `ed_service()`（`main.c:356-361`）；而**按键本身在音频 ISR 里处理**（`seq.c:1996-2017` `keyboard_block`，每 0.73 ms），所以画得慢也不影响弹。
- LCD SPI 仍是 Felucca 的 12 MHz（`LCD_BAUD 4`）；但同一家族的 fm1-base-amb 用 `LCD_BAUD 0`（60 MHz，实测有效约 44 MHz），注释说 12 MHz 下一条 240×72 的条带要 23 ms，比面板 16.7 ms 刷新还慢，会撕裂（`repos/fm1-base-amb/device/lcd.c:11-15`）。
- 字体：2.6 起 Terminus Bold，1 位 / 像素的点阵（更小、更快）；调色板是一种颜色的 5 级 + 白色强调，2.7 可自定义主题（`gfx.c:33-80`），主题加载时校验"最亮一级和强调色必须和背景拉开"，防止把菜单藏起来（`gfx.c:113-135`）。
- UI 函数全标 `minsize`（`gfx.c:6-15`）。

### FN ACID 现状

画布 240×110。FULL 模式下每 33 ms 无条件画环（134×170）+ 示波器（106×170），每 100 ms 画顶栏（240×70）（`dinghy.c:3122-3132`）。按 12 MHz 算：环一次约 30 ms、示波器约 24 ms、顶栏约 22 ms 的 SPI 时间；画示波器前 `cv_begin` 要 `lcd_sync` 等环发完（`gfx.c:63`），这是忙等。结果是主循环每帧被卡 30 ms 以上，实际跑在 20–30 Hz；**按键、编码器、MIDI 输入输出、灯都在这个循环里**（`dinghy.c:3047-3120`）。（已有 SCREEN LITE / OFF 测试开关。）

### 差距与建议

1. **按键进 ISR（S–M，低）**：照 `keyboard_block`：在 `app_audio` 每块开头读 `fm1_in.notes` 的变化，ACID / KEYS 的现场音符直接在 ISR 里起；主循环只处理层、页面、提示。最低成本的折中：`lcd_sync` 等待时回调一个"快轮询"。
2. **LCD 提速（S，中）**：`LCD_BAUD` 先试 1（30 MHz）、再试 0（60 MHz），上机看有没有花屏；SPI 时间降到 1/2.5–1/3.7。
3. **只画变了的（M，低）**：停止且没有触碰时不重画环；示波器降到 15 fps 或缩小；顶栏用签名。
4. **双画布 / 分条（M，低）**：环分成两条，CPU 画第二条时第一条在 DMA；`.pool` 还有 17 KB。
5. UI 函数 `minsize`（S，低）：减镜像，可能也减轻 I-cache 被 UI 挤占；配合第 2 节的 FULL / OFF 对比验证。

---

## 8. 健壮性

### SLOOP / Felucca 怎么做

- 开机第一件事抬供电、武装看门狗（`main.c:365-376`）；**bootguard**：连续两次启动没活过 30 s 就进 USB 救援，救援本身也复位就进 ROM（`bootguard.h`，与我们的相同）。
- **崩溃屏**（`main.c:129-160`）：向量、PC、EMU、DBG、RETS、**SP**、**core 1 状态和迟到次数**，停 10 s 方便拍照（2.7.1 加了 SP 和 core 1 字段）。
- **`felucca_dbg` 放 `.noinit`**（`audio.c:12-22`，`main.c:210-222`）：半块数、最大耗时、迟到数、UI 帧数、**主循环走到哪一步的面包屑**（stage / page / home），开机把上一次的值存成 `prev_*`：看门狗复位后能知道死在哪。
- 开机验证 `.lowram`、给栈涂色；core 1 的状态码（0 没编进 / 1 没报到 / 2 答错或太慢 / 3 运行）。
- 读回设置、项目、预设时严格校验（CRC、范围）；主题校验；新版本备份恢复进旧固件之前就拒绝；会复位的操作前先落盘；除零陷阱关掉（编译器会把被保护的除法提前执行，Felucca 1.0.3.1，我们的 `fm1_irq.h:68-71` 已经同步）。
- 停着时 OCT− + OCT+ 按住 5 s 进 UBOOT，2 s 后有倒计时，松手取消；播放中不会误触（鼓轨上这两个键常被长按）。

### FN ACID 现状

bootguard、救援、看门狗、OTA、除零陷阱与 SLOOP 相同（`platform.c`）。崩溃屏只显示 vec / pc / rets，8 s（`platform.c:288-305`）。没有 noinit 诊断、没有迟到计数、更新 / UBOOT 前不落盘、存储迁移按尺寸猜（见第 4 节）。

### 差距与建议（都是 S，低风险）

1. 崩溃屏加 SP、EMU、DBG（以后加 core 1 状态），停 10 s。
2. 一个 `.noinit` 诊断结构：开机次数、最大 / 最近渲染耗时、迟到数、主循环面包屑（在画环、在存 flash、在处理 MIDI……），开机保存上一次的值，在 GLO 第 1 页或一个隐藏页显示。
3. `ota_session` / UBOOT 之前调 `app_flush()`。
4. 做双核时，第 1 节的 2.7.1 hold 写法和供电抬升是**必需**，不是可选。

---

## 附：建议先做的三次测量

1. 校准后的音频负载（扣 TIMER5、去 `fpu_sum`），SCREEN FULL vs OFF 各一次：差值大 = I-cache 被 UI 挤占，第 2 节的代码进 RAM 值得做；差值小 = 主要是算力，第 1、3 节（分核、按块渲染）更值。
2. 剖面四项（DRUM / CTL / KEYS / FX）在 909 套、808 套、歌曲 DEEP（pad）下各读一次：决定 core 1 先拿鼓还是 KEYS。
3. 页编程的最坏 `took_us`：决定第 4 节"播放中只写页"能否做。
