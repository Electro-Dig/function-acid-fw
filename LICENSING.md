# Licensing

Dinghy is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The app `firmware/src/dinghy.c`, the wiring `platform.c`, the host mode `tests/`, the docs | zvenson |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

`assets/logo.svg` / `logo.png` carry the designburg signet: Copyright (C) designburg, all rights reserved, not under
the GPL. It marks Dinghy itself; a fork uses its own logo.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. Dinghy is not made or endorsed by them.

## FN ACID additions

| What | Origin | Licence |
| --- | --- | --- |
| 909 drum engine (`firmware/src/dsp/drum909*`, `fastmath.h`, `x0x_param.h`), `tools/gen_drum_samples.py` | [X0X](https://github.com/charlesvestal/fm1-x0x) (Charles Vestal), ported from [9W9](https://github.com/athousanddetails/schwung-9W9) (athousanddetails), itself after [ER-99](https://github.com/matthewcieplak/er-99) (Matthew Cieplak); unchanged | GPL-3.0 |
| 909 hi-hat, ride and crash samples (`assets/909/`) | ER-99 via 9W9 via X0X | GPL-3.0 |
| 808 drum engine (`firmware/src/dsp/drum808*`) | [X0X](https://github.com/charlesvestal/fm1-x0x) (Charles Vestal), ported from 8W8 (Charles Vestal and contributors); the rim shot from Yoshinosuke Horiuchi's sc808; unchanged | GPL-3.0 (sc808: MIT) |
| The animal calls on ACID / T2 ZOO (`CALLS` in `dinghy.c`) | the acid-cat test (this project), after the video "如何用 Acid 学猫叫？" | GPL-3.0 |
| Web installer page logic (`web/installer/sloop-installer-2.7.1.orig.html`) | [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) | GPL-3.0 |
