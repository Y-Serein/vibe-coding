## 2026-05-06 HID Output Report 接入 LCD UI

用户当前状态：
- 按键上行已经能到上位机。
- `key1` / IOA24 可能因无外部上拉暂时不用处理。
- `tools/Vibe_Bridge` 上位机能检测到按键数据。
- 上位机执行
  `cargo run -- test-screen --text "Hello Vibe Bridge!"` 后，屏幕无反应。

原因确认：
- `Vibe_Bridge` 的 `test-screen` 走 Vendor HID output report `0x20`。
- 板端旧 `aikb_hid_input` 只解析 `0x20` 并 ACK `0x21`，没有把 payload
  送给 `aikb_lcd_ui`。
- `aikb_lcd_ui` terminal 视图消费的是 `--input PATH` 的原始 VT100/UTF-8
  字节流。

本轮已完成：
- `middleware/v2/sample/aikb_hid_input/aikb_hid_input.c`
  - 新增 `--screen-out PATH`。
  - HID output report `0x20` 现在会翻译成 VT100 字节并写到 `PATH`：
    - `0x01` clear -> `ESC[2J ESC[H`
    - `0x02` write -> UTF-8 payload
    - `0x03` set cursor -> `ESC[row;colH`
    - `0x04` newline -> `CR LF`
    - `0x05` backspace -> `BS`
  - ACK `0x21` 的 status 现在反映 screen-out 写入结果：
    `0x00 OK`、`0x01 busy/no reader yet`、`0x02 error`。
- `middleware/v2/sample/aikb_lcd_ui/aikb_lcd_ui.c`
  - `--input` 如果是 FIFO，会用 `O_RDWR | O_NONBLOCK` 打开，避免无 writer
    时 FIFO EOF/POLLHUP 影响后续 HID 写入。
  - 只有字符设备才做 termios serial 配置，FIFO 不再打印 serial setup warning。
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/auto.sh`
  - 创建 `/tmp/aikb_lcd_ui.in` FIFO。
  - 启动 `aikb_hid_input --hid /dev/hidg0 --screen-out /tmp/aikb_lcd_ui.in`。
  - 启动 `aikb_lcd_ui --input /tmp/aikb_lcd_ui.in --view terminal`。
- README 已更新 HID 下行到 LCD UI 的路径说明。

已同步到生成目录：
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/bin/aikb_hid_input`
- `buildroot/output/target/mnt/system/usr/bin/aikb_hid_input`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/usr/bin/aikb_hid_input`
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/bin/aikb_lcd_ui`
- `buildroot/output/target/mnt/system/usr/bin/aikb_lcd_ui`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/usr/bin/aikb_lcd_ui`
- `buildroot/output/target/mnt/system/auto.sh`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/auto.sh`

当前二进制 SHA-256：
- `aikb_hid_input`: `000fc2459663c717376c7f4117d8938fc774682e8286505d329d5c4d5bd89581`
- `aikb_lcd_ui`: `b96aebb69cd93e94dd3e5ea995707536d0ba5e3a915244a4d43f98d5acac1a93`

轻量验证：
- `aikb_hid_input` RISC-V musl 交叉编译通过。
- `aikb_lcd_ui` RISC-V musl + FreeType 交叉编译通过。
- 两个二进制 `riscv64-unknown-linux-musl-strip --strip-all` 通过。
- overlay/output/install 三处二进制 SHA 一致。
- overlay/output/install 三处 `auto.sh` 的 `sh -n` 通过。
- `git diff --check` 通过。

下一次定向打包：
```
cd /home/rv_nano/AIKB/LicheeRV-Nano-Build
apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && pack_rootfs && pack_burn_image'
```

板端验证：
```
pidof aikb_hid_input
pidof aikb_lcd_ui
ls -l /tmp/aikb_lcd_ui.in
cat /tmp/aikb_hid_input.log
cat /tmp/aikb_lcd_ui.log
```

上位机验证：
```
cd /home/rv_nano/Sipeed/rv_nano/tools/Vibe_Bridge
cargo run -- test-screen --text "Hello Vibe Bridge!"
```

## 2026-05-06 AIKB 按键/旋钮和 Vendor HID 接入

用户要求：
- 三个按键：A15、A24、A23。
- 旋转编码器 A/B/E：A27、A25、A22。
- 引脚需要上拉。
- 做 HID 定义，参考
  `/home/rv_nano/Sipeed/rv_nano/docs/hid_vendor_terminal_spec.pdf`。
- 旋转编码器逻辑参考
  `https://blog.csdn.net/Ammon_Zhang/article/details/84585205`。

本轮已完成：
- 新增 `middleware/v2/sample/aikb_hid_input/`
  - `aikb_hid_input.c`：独立用户态守护进程，通过 `/dev/mem` 配置并轮询 GPIOA。
  - `Makefile`：支持 sample 级编译。
  - `README.md`：记录按键/编码器/HID report 映射和板端测试命令。
- GPIO/Pad 映射：
  - key0: A15 / `SPK_EN` / GPIOA15 / IOBLK `0x03001908`
  - key1: A24 / `SPINOR_CS_X` / GPIOA24 / IOBLK `0x03001928`
  - key2: A23 / `SPINOR_MISO` / GPIOA23 / IOBLK `0x03001924`
  - encoder A: A27 / `SPINOR_WP_X` / GPIOA27 / IOBLK `0x03001920`
  - encoder B: A25 / `SPINOR_MOSI` / GPIOA25 / IOBLK `0x0300191c`
  - encoder E: A22 / `SPINOR_SCK` / GPIOA22 / IOBLK `0x03001918`
- 每个输入启动时都会：
  - pinmux 低 3 位切到 XGPIOA function `0x3`。
  - IOBLK 设置 `PU bit2 = 1`，`PD bit3 = 0`。
  - GPIOA `DDR` 清位为输入，输入电平从 DW APB GPIO `EXT_PORTA`
    `0x03020050` 读取。
- HID 上报：
  - Vendor-defined Usage Page `0xFF60`，Usage `0x01`。
  - 输入 report ID `0x10`，固定写 64 字节：
    - byte0 = `0x10`
    - byte1 bit0..bit2 = key0..key2，bit7 = encoder switch
    - byte2 = signed encoder delta，left = -1，right = +1
    - byte3..63 = 0
  - 输出 report ID `0x20` 已能解析，并用输入 report ID `0x21` ACK
    `seq/status`；LCD UI 消费 HID 下行文本的路径先保留，后续可接。
  - feature report ID `0x30` 已放入 descriptor，板端守护进程暂不消费。
- USB gadget：
  - `buildroot/board/cvitek/SG200X/overlay/etc/init.d/S08usbdev`
    在 `/boot/usb.aikb_hid` 存在时创建 `functions/hid.aikb`。
  - HID `subclass=0`、`protocol=0`，避免被主机识别成 Keyboard HID。
  - `report_length=64`，descriptor 带固定 padding，匹配 `/dev/hidg0`
    64 字节读写。
- 开机启动：
  - `buildroot/board/cvitek/SG200X/overlay/mnt/system/auto.sh`
    现在先启动 `/mnt/system/usr/bin/aikb_hid_input --hid /dev/hidg0`，
    再启动 LCD UI。
  - `S09aikb stop` 会同时停 `aikb_hid_input` 和 `aikb_lcd_ui`。
- boot 分区标记：
  - `build/tools/common/sd_tools/sd_gen_burn_image_rootless.sh`
    会生成 `input/usb.aikb_hid`。
  - `build/tools/common/sd_tools/genimage_rootless.cfg` 和当前
    `install/soc_sg2002_licheervnano_sd/genimage.cfg` 已把
    `usb.aikb_hid` 放进 boot.vfat。

已同步到生成目录：
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/bin/aikb_hid_input`
- `buildroot/output/target/mnt/system/usr/bin/aikb_hid_input`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/usr/bin/aikb_hid_input`
- `buildroot/output/target/etc/init.d/S08usbdev`
- `buildroot/output/target/etc/init.d/S09aikb`
- `buildroot/output/target/mnt/system/auto.sh`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/auto.sh`
- `install/soc_sg2002_licheervnano_sd/input/usb.aikb_hid`

当前 `aikb_hid_input` RISC-V musl 二进制：
- SHA-256: `79bc2c80f2c47ee8129cae1d94142ef590cfdf0b7200a4d7dca63e5b7ea0611e`
- 大小约 10 KB，动态链接 `/lib/ld-musl-riscv64xthead.so.1`。

轻量验证：
- sqfs 容器内 sample 级 `make -B CC=...riscv64-unknown-linux-musl-gcc`
  通过。
- `riscv64-unknown-linux-musl-strip --strip-all` 通过。
- `S08usbdev` / `S09aikb` / `auto.sh` 的 `sh -n` 通过。
- 三处 rootfs/overlay 同步二进制 SHA 一致。
- 内核当前 `.config` 已有 `CONFIG_USB_CONFIGFS_F_HID=y`、
  `CONFIG_USB_F_HID=y`、`CONFIG_DEVMEM=y`；板端 gadget 侧不依赖
  `CONFIG_HIDRAW`。

下一次定向打包：
```
cd /home/rv_nano/AIKB/LicheeRV-Nano-Build
apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && pack_rootfs && pack_burn_image'
```

板端验证：
```
ls -l /dev/hidg0
pidof aikb_hid_input
cat /tmp/aikb_hid_input.log
cat /sys/kernel/config/usb_gadget/g0/functions/hid.aikb/report_length
```

如果旋钮方向和预期相反，手动验证：
```
killall aikb_hid_input
/mnt/system/usr/bin/aikb_hid_input --hid /dev/hidg0 --reverse --debug
```

## 2026-05-06 VT100 终端视图和 Powerline/字体适配

用户确认当前启动/UI 都正常，要求把终端显示体验改成 VT100 兼容控制标准，
参考 `Sipeed/rv_nano/tools/vt100-parser` 和
`Sipeed/rv_nano/tools/TFT_vt100-master`，并适配 Powerline 状态栏符号体系及
SorasaGothic / Sarasa Gothic 这类适合中英文混排的编程字体。

本轮已完成：
- `middleware/v2/sample/aikb_lcd_ui/aikb_lcd_ui.c`
  - 新增默认 `--view terminal` VT100/ANSI 终端视图。
  - 保留旧参数面板为 `--view dashboard`，后续 JSON 参数接入仍可用。
  - Type-C `--input /dev/ttyGS0` 在 terminal 视图下按原始 UTF-8 字节流处理；
    dashboard 视图下仍按原来的 newline JSON 处理。
  - VT100/ANSI 支持范围：
    - C0 控制：CR/LF/BS/TAB/ESC
    - 光标移动/定位：`CSI A/B/C/D/H/f`
    - 擦除：`CSI J/K`
    - 滚动区域/插入删除行/反向索引：`CSI r/L/M`、`ESC M`
    - 保存/恢复光标：`ESC 7/8`、`CSI s/u`
    - SGR：reset、bold、inverse、8/16 色、`38;2;r;g;b`/`48;2;r;g;b`
  - 内置绘制 Powerline PUA 符号 `U+E0B0`..`U+E0B3`，避免字体不带
    Powerline glyph 时状态栏断裂。
  - 新增 FreeType 渲染路径，字体回退优先级包含：
    `SarasaMonoSC`、`SarasaTermSC`、`SarasaGothicSC`、`SorasaGothic`、
    WQY ZenHei、DejaVuSansMono；也支持 `--font PATH` 指定。
  - 若 FreeType/字体不可用，回退到 U-Boot 内置 8x16 字模，保证程序仍能启动。
- `middleware/v2/sample/aikb_lcd_ui/Makefile`
  - 交叉编译时链接 buildroot sysroot 里的 FreeType。
  - 本机无 FreeType 时自动 `-DAIKB_USE_FREETYPE=0`，不阻塞源码级编译。
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/auto.sh`
  - 启动命令显式改为：
    `/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --input /dev/ttyGS0 --rotate auto --view terminal`
- `middleware/v2/sample/aikb_lcd_ui/README.md`
  - 记录 terminal/dashboard 两种输入模式、VT100 支持范围、Powerline 和字体回退路径。

已同步到生成目录：
- `buildroot/output/target/mnt/system/auto.sh`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/auto.sh`
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/bin/aikb_lcd_ui`
- `buildroot/output/target/mnt/system/usr/bin/aikb_lcd_ui`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/usr/bin/aikb_lcd_ui`
- `install/soc_sg2002_licheervnano_sd/rootfs/usr/lib/` 下已同步
  `libfreetype.so.6` 及其依赖 `libbz2/libpng16/libz/libbrotli*`
- `install/soc_sg2002_licheervnano_sd/rootfs/usr/share/fonts/wqy-zenhei/`
  已同步 WQY ZenHei 回退字体

当前 `aikb_lcd_ui` RISC-V musl 二进制：
- SHA-256: `118a20afe28918e3e790e133a06350a241d21a0af1a596acf9c9a3724076b6e8`
- 大小约 55 KB，动态链接 `/lib/ld-musl-riscv64xthead.so.1` 和 rootfs FreeType。

轻量验证：
- `riscv64-unknown-linux-musl-gcc` 单文件交叉编译并链接 FreeType 通过。
- `riscv64-unknown-linux-musl-strip` 通过。
- sqfs 容器内 sample 级 `make -B CC=...riscv64-unknown-linux-musl-gcc` 通过，
  未运行 `build_middleware` / `build_all`。
- 已确认三处同步二进制 SHA 一致。
- `readelf -d /tmp/aikb_lcd_ui.musl.riscv` 显示运行时依赖为
  `libfreetype.so.6` 和 `libc.so`；当前 install rootfs 已补齐 FreeType 依赖链。

注意：
- 当前镜像内没有找到 Sarasa/Sorasa 字体文件；程序会先尝试这些路径，实际会回退到
  `/usr/share/fonts/wqy-zenhei/wqy-zenhei.ttc` 或 DejaVu。后续如果要真正使用
  Sarasa，把字体放到 `/mnt/system/fonts/SarasaMonoSC-Regular.ttf` 等 README
  记录路径即可，无需改代码。
- 本轮未运行 `build_all`，遵守用户要求：长耗时完整构建由用户操作。

下一次定向打包：
```
cd /home/rv_nano/AIKB/LicheeRV-Nano-Build
apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && pack_rootfs && pack_burn_image'
```

## 2026-05-06 新镜像进 UI 后绿屏空窗和颜色偏蓝

用户反馈：执行 `pack_rootfs && pack_burn_image` 后烧录，上电约 4s 显示横屏
`starting`，约 7s 屏幕变绿，约 17s 进入正式 UI，但 UI 整体偏蓝。

判断：
- `starting -> UI` 说明 rootfs 自启动链现在已经跑通。
- 7s 绿屏发生在 Linux 加载/启用 `soph_fb.ko` 后、`aikb_lcd_ui` 画第一帧前。
  之前把 `soph_fb.ko` 放到 `S00kmod` 很早加载，而 UI 在 `S99aikb` 才启动，
  中间空窗太长。
- UI 偏蓝是 32bpp 字节序问题。`osdrv/interdrv/v2/fb/fb.c` 的 fb bitfield
  看起来像 ABGR，但底层 GOP mode 是 `SCL_GOP_FMT_ARGB8888`。用户态按 fb
  bitfield 写会把红蓝通道反掉，琥珀色会变成蓝色。
- 之前 `build_all` 没达到效果，主要是当时改动还在 U-Boot/ramdisk 或 overlay
  源码路径，实际烧录使用的 buildroot rootfs 生成目录没有拿到正确 init 脚本。
  这次显式 `pack_rootfs && pack_burn_image` 在同步了 `buildroot/output/target`
  和 `install/.../rootfs` 后重打 rootfs，所以有效。当前源码修正后，干净状态下
  `build_all` 也应包含这些改动；继续调屏时仍建议用定向 `pack_rootfs &&
  pack_burn_image`，避免长时间全量构建。

本轮准备改动：
- `middleware/v2/sample/aikb_lcd_ui/aikb_lcd_ui.c`
  - 新增 `--pixel-format auto|fb|argb8888|abgr8888`。
  - 默认 `auto` 下，如果 framebuffer id 是 `cvifb` 且 32bpp，则按 GOP
    `ARGB8888` 写入，修正红蓝通道交换。
  - `/dev/ttyGS0` 暂时打不开时，每 5 秒重试，允许 UI 提前启动后等待
    Type-C gadget 晚一点出现。
- `buildroot/board/cvitek/SG200X/overlay/etc/init.d/S00kmod`
  - 移除早期加载 `soph_fb.ko`，避免 fb 图层过早启用导致绿色空窗。
- 新增 `buildroot/board/cvitek/SG200X/overlay/etc/init.d/S09aikb`
  - 在 `S08usbdev` 后、`S10udev` 前加载 `soph_fb.ko option=1 opt_bpp=32`，
    然后立即启动 `/mnt/system/auto.sh`，尽量缩短绿屏到第一帧 UI 的时间。
- 保留 `S99aikb` 作为兜底；`auto.sh` 现在会先检查 `pidof aikb_lcd_ui`，
  已运行时不清空日志、不重启。
- `middleware/v2/sample/aikb_lcd_ui/README.md`
  - 记录 `--pixel-format` 调色诊断命令。

已同步到生成目录：
- `buildroot/output/target/etc/init.d/S00kmod`
- `buildroot/output/target/etc/init.d/S09aikb`
- `buildroot/output/target/etc/init.d/S99aikb`
- `buildroot/output/target/mnt/system/auto.sh`
- `buildroot/output/target/mnt/system/usr/bin/aikb_lcd_ui`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/auto.sh`
- `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/usr/bin/aikb_lcd_ui`

当前 `aikb_lcd_ui` musl 二进制 SHA-256：
`4a061f2b93db9f4a3702301391f58613cd91263375bbefbf75cbf648c8baf235`

轻量验证：
- `riscv64-unknown-linux-musl-gcc` 单文件编译通过。
- `S00kmod` / `S09aikb` / `S99aikb` / `auto.sh` 相关脚本 `sh -n` 通过。
- `git diff --check` 通过。

下一次定向打包命令：
```
cd /home/rv_nano/AIKB/LicheeRV-Nano-Build
apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && pack_rootfs && pack_burn_image'
```

若新镜像仍偏蓝，板端对比：
```
killall aikb_lcd_ui
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto --pixel-format argb8888
killall aikb_lcd_ui
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto --pixel-format abgr8888
```

## 2026-05-06 fb0 手动加载后的 UI 刷新问题

用户在板端手动验证：
```
insmod /mnt/system/ko/soph_fb.ko option=1 opt_bpp=32
ls -l /dev/fb0
cat /proc/fb
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto --once
```

结果：
- `/dev/fb0` 能出现，`/proc/fb` 为 `0 cvifb`。
- UI 程序识别到 `412x960 32bpp line=1648 rotate=cw`。
- 但画面“奇奇怪怪的，没什么东西”。
- 系统还有 `INIT: Id "acm" respawning too fast: disabled for 5 minutes`，这是 USB gadget/串口 getty 相关问题，和 framebuffer 绘制问题分开处理。

原因判断：
- `osdrv/interdrv/v2/fb/fb.c` 的 `cvifb_release()` 在最后一个 `/dev/fb0` fd 关闭时会 `_fb_enable(false)`。所以用 `--once` 测试时，程序画完立刻退出，驱动随后关闭 GOP framebuffer 图层，肉眼可能只看到闪一下或残留怪画面。
- cvifb 驱动在 `cvifb_pan_display()` / `cvifb_set_par()` 路径里做 DMA sync；用户态 mmap 写 framebuffer 后如果不触发 pan，RISC-V 上可能出现显示端读到旧数据/缓存未同步的花屏。

本轮准备改动：
- `middleware/v2/sample/aikb_lcd_ui/aikb_lcd_ui.c`
  - `fb_blit()` 写完 framebuffer 后增加 `msync()` 和 `FBIOPAN_DISPLAY`。
  - `--once` 默认画完保留 3000ms 再退出；新增 `--hold MS` 可调。
  - 新增 `--alpha 0..255`，默认 255；如果后续确认 GOP alpha 语义相反，可板端直接试 `--alpha 0`。
- `middleware/v2/sample/aikb_lcd_ui/README.md`
  - 记录 `--once --hold 5000` 的板端调试方法。
  - 说明 cvifb close 会关闭图层，以及 `--alpha 0` 的调试用途。
- `buildroot/board/cvitek/SG200X/overlay/etc/inittab`
  - 注释掉 `acm::respawn:/sbin/getty -L ttyGS0 ...`。
  - 原因：`ttyGS0` 后续要作为 Type-C JSON 数据通道给 `aikb_lcd_ui --input /dev/ttyGS0` 使用，不能再被 login/getty 占用；这也解释了当前旧镜像上的 `INIT: Id "acm" respawning too fast`。

当前旧镜像上无需重烧的即时验证建议：
```
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto
```
不要带 `--once`，保持进程运行；如方向反了，停止后试：
```
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate ccw
```

## 2026-05-06 新镜像只停在 `starting` 的自启动修正

用户反馈：烧录新镜像后启动页已横屏，但一直只显示 U-Boot splash 的
`starting`，没有进入 Linux 阶段的 `aikb_lcd_ui` dashboard。

判断：
- 横屏说明 U-Boot splash 改动已经生效。
- 一直显示 `starting` 说明 Linux 用户态 UI 没有成功接管 framebuffer；
  常见原因是 `/dev/fb0` 没起来、`/mnt/system/auto.sh` 没跑、或者
  `aikb_lcd_ui` 启动后马上退出。
- 查到两个脚本风险点：
  - `auto.sh` 看到 `/tmp/evb_init` 已存在会直接 `exit 1`，别的应用/脚本
    先创建这个文件时会跳过 UI。
  - `ramdisk/rootfs/overlay/*/etc/init.d/S99user` 如果存在
    `/mnt/data/auto.sh` 会启动它然后 `exit 1`，导致 `/mnt/system/auto.sh`
    不执行。

本轮准备改动：
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/auto.sh`
- `ramdisk/rootfs/overlay/{musl_riscv64,glibc_riscv64,32bit}/system/auto.sh`
  - 去掉 `/tmp/evb_init` 已存在时退出的行为，改成记录日志后继续。
  - 如果 `/dev/fb0` 不存在，主动重跑一次 `/mnt/system/ko/loadsystemko.sh`。
  - 等待 `/dev/fb0` 的时间从 8 秒增加到 30 秒。
  - 所有关键状态写入 `/tmp/aikb_lcd_ui.log`。
- `ramdisk/rootfs/overlay/{musl_riscv64,glibc_riscv64,32bit}/etc/init.d/S99user`
  - 删除 `/mnt/data/auto.sh` 存在时的提前 `exit 1`，允许 `/mnt/system/auto.sh`
    总是继续运行。
- 已把当前 musl 版 UI 程序同步到：
  `buildroot/board/cvitek/SG200X/overlay/mnt/system/usr/bin/aikb_lcd_ui`
  - SHA-256: `2111cff9e0cb06a07493d25a07b0e82e1bac75215d6223f089fc6a54b837863d`

本轮轻量验证：
- 4 个 `auto.sh` 均 `sh -n` 通过。
- 3 个 `S99user` 均 `sh -n` 通过。
- `git diff --check` 通过。
- `aikb_lcd_ui` 用 RISC-V musl gcc 单文件编译通过。

当前板端无需重烧的诊断命令：
```
ls -l /dev/fb0
cat /proc/fb
pidof aikb_lcd_ui
cat /tmp/aikb_lcd_ui.log
ls -l /mnt/system/usr/bin/aikb_lcd_ui
sh /mnt/system/auto.sh
cat /tmp/aikb_lcd_ui.log
```

如果 `sh /mnt/system/auto.sh` 后 dashboard 出现，说明就是自启动脚本路径/时序
问题；使用本轮脚本改动重新打包 rootfs 后应可上电自动进入 UI。

## 2026-05-06 确认 rootfs 使用 buildroot init.d，而不是 ramdisk S99user

用户板端继续确认：
```
ls -l /dev/fb0
cat /proc/fb
pidof aikb_lcd_ui
cat /tmp/aikb_lcd_ui.log
rm -f /tmp/evb_init
sh /mnt/system/auto.sh
cat /tmp/aikb_lcd_ui.log
```

结果：
- `/dev/fb0` 不存在。
- `/proc/fb` 为空。
- `aikb_lcd_ui` 没有运行。
- `/tmp/aikb_lcd_ui.log` 原本不存在，手动跑旧版 `/mnt/system/auto.sh` 后只写
  `/dev/fb0 not ready`。

本地确认：
- 实际生成的 `buildroot/output/target` 里没有 `ramdisk/.../etc/init.d/S99user`。
- 因此之前依赖 `S99user -> /mnt/system/ko/loadsystemko.sh -> /mnt/system/auto.sh`
  的启动路径在 buildroot rootfs 里根本不会执行。
- 之前烧录的 rootfs 中 `/mnt/system/auto.sh` 也是旧副本，说明 overlay 源码改动
  没有同步到已生成 rootfs。

本轮准备改动：
- `buildroot/board/cvitek/SG200X/overlay/etc/init.d/S00kmod`
  - 在 `soph_rgn.ko` 后直接加载：
    `insmod soph_fb.ko option=1 opt_bpp=32`
  - 这样 buildroot init 流程会注册 `/dev/fb0`。
- 新增 `buildroot/board/cvitek/SG200X/overlay/etc/init.d/S99aikb`
  - `start` 时后台执行 `/mnt/system/auto.sh`。
  - `stop` 时停止 `aikb_lcd_ui`。
- 已同步生成目录，避免只重新打包时拿到旧文件：
  - `buildroot/output/target/etc/init.d/S00kmod`
  - `buildroot/output/target/etc/init.d/S99aikb`
  - `buildroot/output/target/mnt/system/auto.sh`
  - `buildroot/output/target/mnt/system/ko/loadsystemko.sh`
  - `buildroot/output/target/mnt/system/usr/bin/aikb_lcd_ui`
  - `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/auto.sh`
  - `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/ko/loadsystemko.sh`
  - `install/soc_sg2002_licheervnano_sd/rootfs/mnt/system/usr/bin/aikb_lcd_ui`
- `aikb_lcd_ui` 三处 SHA-256 均为：
  `2111cff9e0cb06a07493d25a07b0e82e1bac75215d6223f089fc6a54b837863d`

本轮轻量验证：
- `S00kmod` / `S99aikb` 源码和生成目录副本均 `sh -n` 通过。
- `rg` 确认 `buildroot/output/target/etc/init.d/S00kmod` 已包含
  `soph_fb.ko option=1 opt_bpp=32`。
- `rg` 确认 `buildroot/output/target/etc/init.d/S99aikb` 会执行
  `/mnt/system/auto.sh`。

下一次出镜像必须重建 rootfs 后再打包，不能只 `build_uboot && pack_burn_image`：
```
cd /home/rv_nano/AIKB/LicheeRV-Nano-Build
apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && pack_rootfs && pack_burn_image'
```

当前板端无需重烧的定位命令：
```
grep soph_fb /mnt/system/ko/loadsystemko.sh
sh /mnt/system/ko/loadsystemko.sh >/tmp/loadko.log 2>&1
tail -120 /tmp/loadko.log
ls -l /dev/fb0
cat /proc/fb
```
如果仍没有 `/dev/fb0`：
```
insmod /mnt/system/ko/soph_fb.ko option=1 opt_bpp=32
echo $?
dmesg | tail -120
```
如果 `/dev/fb0` 出现：
```
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto
```

## 2026-05-06 UI 接入进展

本轮没有继续改 U-Boot 显示驱动，新增了一个 Linux 用户态 LCD UI sample：
`middleware/v2/sample/aikb_lcd_ui/`

目标是先把 LCD 内容做成 `Sipeed/rv_nano/tools/vibe-keyboard/images/LCD1.png`
那种黑底琥珀色 overview 仪表盘，同时把数据层做成后续 Type-C 接入友好的结构。程序内部固定以 `960x412` 横屏 canvas 渲染；写入 framebuffer 时支持 `--rotate auto|none|cw|ccw`，其中 `auto` 会在 framebuffer 是 `412x960` 时自动顺时针旋转，适配 412W x 960H 面板横放。

新增文件：
- `middleware/v2/sample/aikb_lcd_ui/aikb_lcd_ui.c`
- `middleware/v2/sample/aikb_lcd_ui/Makefile`
- `middleware/v2/sample/aikb_lcd_ui/README.md`

当前 session 状态和协作约定：
- 用户明确要求：以后 `build_all` 这类耗时完整构建由用户自己操作；助手只负责做好准备工作，包括代码改动、容器命令、预期产物路径、检查点和后续验证步骤。
- 助手可以继续做短耗时或定向检查，例如源码检查、单文件/单 sample 编译、`build_uboot` 前准备核对、以及用户跑完构建后的产物验证。
- 本轮为把 `aikb_lcd_ui` 打进镜像，已在 `middleware/v2/Makefile` 的 `install` 流程增加拷贝：`sample/aikb_lcd_ui/aikb_lcd_ui -> $(DESTDIR)/usr/bin`。
- 当前工作树不干净：除 `HANDOFF.md`、`middleware/v2/Makefile`、`middleware/v2/sample/aikb_lcd_ui/` 这些本轮源码/交接改动外，完整 `build_all` 还改写了多处 SDK 生成物、ko、sample binary、middleware lib 和 overlay 文件；不要在未确认前批量 revert。
- 后续如果只提交 UI 源码改动，需要把构建产物和 overlay 生成文件单独处理；如果继续出测试镜像，则让用户手动运行完整构建，助手再核对产物。

## 2026-05-06 启动页和自启动准备

用户反馈：当前烧录后仍然是“中间竖条颜色一直变换”，没有看到目标 UI。这个现象仍按 GC9503 panel BIST/测试图案路径处理。

本轮已做准备改动，但未运行完整 `build_all`：
- `u-boot-2021.10/cmd/cvi_vo.c`
  - GC9503 初始化后不再启用 DISP color-bar，改为 `SCL_PAT_TYPE_OFF`。
  - 在 `GC9503_ENABLE_PANEL_BIST == 0` 路径下主动发送 `F0 55 AA 52 08 00` 和 `EE 00 00 00 00`，尝试关闭 panel 内置 BIST。
  - 新增 U-Boot 自绘启动页：黑底、琥珀色代码图标、`Vibe Coding` 文案。画面直接写入 `CVIMMAP_BOOTLOGO_ADDR + 0x100000` 的 YUV420 buffer，再配置 DISP 从该 buffer 输出。
  - GC9503 的 `startvl` 会跳过 JPEG logo video-layer 覆盖，保持自绘 `Vibe Coding` 启动页。
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/auto.sh`
  - 上电进入 Linux 后等待 `/dev/fb0` 最多 8 秒。
  - 后台启动 `/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --input /dev/ttyGS0 --rotate auto`。
  - 日志写到 `/tmp/aikb_lcd_ui.log`；若 `/dev/ttyGS0` 未就绪，程序会先用 mock 页面显示。
- 同步更新了 `ramdisk/rootfs/overlay/{musl_riscv64,glibc_riscv64,32bit}/system/auto.sh`，避免后续切 rootfs overlay 时丢自启动逻辑。

已完成轻量验证：
- `git diff --check` 通过。
- 4 个 `auto.sh` 均通过 `sh -n`。
- 容器内定向编译 `cmd/cvi_vo.o` 通过：
  `apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh >/dev/null && defconfig sg2002_licheervnano_sd >/dev/null && export KBUILD_OUTPUT=/home/rv_nano/AIKB/LicheeRV-Nano-Build/u-boot-2021.10/build/sg2002_licheervnano_sd && make -C u-boot-2021.10 cmd/cvi_vo.o -j1'`

用户下一步手动完整构建命令：
`cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && build_all'`

构建后助手需要核对：
- 新镜像路径：`install/soc_sg2002_licheervnano_sd/images/*.img`
- U-Boot 日志应出现：
  - `GC9503 panel BIST: disabled for Vibe Coding splash`
  - `GC9503 panel BIST: force disable internal test pattern`
  - `GC9503 finish: DISP pattern disabled`
  - `GC9503 splash: show landscape Vibe Coding boot page`
  - `GC9503 startvl: keep Vibe Coding splash`
- Linux 后检查：`pidof aikb_lcd_ui` 和 `/tmp/aikb_lcd_ui.log`。

## 2026-05-06 fb0 和横屏修正

用户在板端确认：
- 启动页可见，但仍是竖屏方向。
- Linux 很久没有进入 `aikb_lcd_ui`。
- `/dev/fb0` 不存在。
- `/mnt/system/usr/bin/aikb_lcd_ui` 存在，大小约 26640 bytes。
- `pidof aikb_lcd_ui` 无输出。

原因判断：
- `loadsystemko.sh` 里没有加载 `soph_fb.ko`，所以 Linux 不会注册 `/dev/fb0`，`auto.sh` 会一直等不到 framebuffer。
- U-Boot 启动页之前按 412x960 面板原生坐标绘制，所以看起来是竖屏。

本轮已做准备改动，但未运行完整 `build_all`：
- `u-boot-2021.10/cmd/cvi_vo.c`
  - 启动页逻辑分辨率改为 `960x412` 横屏。
  - 写入面板 buffer 时按 `px = 411 - y, py = x` 旋转到 412x960 面板，目标是物理横放时显示横屏 `Vibe Coding starting`。
- `buildroot/board/cvitek/SG200X/overlay/mnt/system/ko/loadsystemko.sh`
  - 在 `soph_rgn.ko` 后增加：`insmod /mnt/system/ko/soph_fb.ko option=1 opt_bpp=32`
- 同步更新了 `ramdisk/rootfs/overlay/{musl_riscv64,glibc_riscv64,32bit}/system/ko/loadsystemko.sh`。

已完成轻量验证：
- `git diff --check` 通过。
- 4 个 `loadsystemko.sh` 均通过 `sh -n`。
- 容器内定向编译 `cmd/cvi_vo.o` 通过。

当前已烧录旧镜像可手动验证 `/dev/fb0` 和 UI：
```
insmod /mnt/system/ko/soph_fb.ko option=1 opt_bpp=32
ls -l /dev/fb0
/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto --once
```
如果 `insmod soph_fb.ko` 报依赖问题，先跑：
```
sh /mnt/system/ko/loadsystemko.sh
insmod /mnt/system/ko/soph_fb.ko option=1 opt_bpp=32
```

当前 UI 数据模型字段稳定为：
- 顶栏：`active/window/avg_usage/total_spend/focus`
- session 行：`session/tool/mode/model/cost/usage/reset/state`
- 底栏：`task/now/cost`

Type-C 后续接入建议：电脑侧把 Claude Code/Codex 运行状态转成 newline-delimited flat JSON，经 USB serial 发给板端；板端程序用 `--input /dev/ttyGS0` 读取。当前 parser 已支持：
- 自定义 summary/session 更新行，例如 `{"type":"session","id":"WEBAPI",...}`
- Claude Code hook 风格事件：`SessionStart/PreToolUse/PostToolUse/Stop/session_start/permission_request/tool_use`

验证状态：
- 当前环境没有 `make/gcc` 本机工具，无法运行 x86 版生成 PPM 截图。
- 已用 SDK 自带 RISC-V glibc 工具链编译通过：
  `/home/rv_nano/AIKB/LicheeRV-Nano-Build/host-tools/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-gcc -std=c99 -Wall -Wextra -Wno-unused-parameter -o /tmp/aikb_lcd_ui.riscv aikb_lcd_ui.c`
- 输出为 RISC-V Linux ELF，大小约 43K。
- 2026-05-06 已在 sqfs 容器里执行完整 `build_all`：
  `apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && build_all'`
- 容器构建中 `middleware/v2/sample/aikb_lcd_ui` 已由 `riscv64-unknown-linux-musl-gcc` 编译，并安装到 rootfs：`/mnt/system/usr/bin/aikb_lcd_ui`。
- 输出 SD 镜像：
  `install/soc_sg2002_licheervnano_sd/images/2026-05-06-10-01-40b994.img`
- 已复制一份到：
  `/home/rv_nano/Sipeed/rv_nano/docs/img/2026-05-06-10-01-40b994.img`
- SHA-256：
  `86a5306252fb36eb285dc8462816ca8c1b624c93d7fb645822112aa9f5e11d5d`
- 构建日志里 `pack_system_sd()` 的 `upgrade.zip` 打包阶段出现过一次 SDK 尺寸检查报错，但 `build_all` 进程最终返回 0，SD `hdimage` 正常生成；核对 `rawimages/rootfs.sd` 和最终镜像 rootfs 分区均为 `1677721600` 字节。

下一步如果继续：
- 在板端跑 `/mnt/system/usr/bin/aikb_lcd_ui --fb /dev/fb0 --rotate auto --once`，确认画面方向；方向反了就改 `--rotate ccw`。
- 用电脑侧串口发 README 里的 summary/session JSON，确认 Type-C 更新链路。
- 如果实际 Linux 没有 `/dev/fb0`，需要把 `fb_blit()` 后端换成现有显示驱动暴露的 buffer/VO 接口，UI canvas 和输入协议可以保留。

## 当前在做什么

正在给 LicheeRV Nano / SG2002 的 U-Boot 阶段适配一块 `GC9503-CXW034-412x960` MIPI DSI 屏。当前已经从只有背光推进到 panel 内置 BIST 有动态颜色输出。

2026-04-30 纠正：已从 `docs/TFT/CXW034BS001A.pdf` 图纸确认该屏是 3.4 inch、`412RGB(W) x 960(H)` 点阵。此前代码和交接中按 `480x800` 处理是错误的。U-Boot 和 middleware 的 GC9503 active 分辨率已改为 HACT=412、VACT=960，panel name 和符号名同步改为 `412x960`。当前 porch 仍沿用 bring-up 基线 HSA/HBP/HFP=10/50/50、VSA/VBP/VFP=10/20/20，计算 pixel clock 约 31.633 MHz，后续仍需从屏厂资料确认最终 DSI video timing。

目标是在 U-Boot 阶段先点亮屏幕。现在已经绕过 bootlogo 解码路径，改为强制输出 DISP color-bar 来判断 DSI video/显示链路是否通。

最新待测版本是 2026-04-30 11:52 构建输出的镜像，关闭 GC9503 内置 BIST，只测试 DISP color-bar：
`install/soc_sg2002_licheervnano_sd/images/2026-04-30-11-52-a17040.img`

注意：该镜像是 `build_uboot && pack_burn_image` 输出过的路径。若 `install/.../images` 目录后续被清理或只剩 `boot.vfat`，直接按下面命令重建即可：
`apptainer exec --cleanenv host/ubuntu/licheervnano-build-ubuntu.sqfs bash -lc 'cd /home/rv_nano/AIKB/LicheeRV-Nano-Build && source build/cvisetup.sh && defconfig sg2002_licheervnano_sd && build_uboot && pack_burn_image'`

## 已经试过的方案和结果（含失败的）

- 初始现象：U-Boot 有 `Init panel GC9503-CXW034-480x800`，屏只有背光。DCS/BTA 读 ID 报 `BTA error`，没有图像。后续已确认 480x800 是错误分辨率，应按 `GC9503-CXW034-412x960` 继续验证。
- 修正 GPIO 逻辑和复位顺序：确认 reset 是 `GPIOE0 active-low`，PWM 是 `GPIOE2 active-high`，power-ct 是 `GPIOE1 active-high`。日志显示 GPIO 都能按 active/inactive 切换，但屏仍只有背光。
- 尝试交换 GPIO 诊断：曾出现 pwm offset 变成 0、reset offset 变成 2，确认这是错误映射，后续已恢复为 reset `E0`、pwm `E2`、power `E1`。
- 跳过 BTA panel reads：去掉读 `0x04/0xDA/0xDB/0xDC/0x0A/0x0C`，不再触发 BTA error，但屏仍只有背光。结论：BTA 读失败不是主因。
- 改 GC9503 私有初始化长包类型：把 U-Boot 和 middleware 的 GC9503 长包从 `0x39` DCS long 改为 `0x29` Generic long。依据是本 SDK 其他 vendor panel 的私有寄存器表也多用 generic long。
- 加 U-Boot color-bar：在 `gc9503_finish_panel()` 切 HS video 后调用 `sclr_disp_set_pattern(SCL_PAT_TYPE_AUTO, SCL_PAT_COLOR_BAR, NULL)`。用户测试 10:01 镜像，日志出现 `GC9503 finish: enable DISP color-bar pattern`，但仍只有背光。
- 最新额外验证版：发现 `sclr_ctrl_init()` 初始化 `g_top_cfg.disp_enable=false`，且代码里没有再打开。已在 `mipi_tx_set_combo_dev_cfg()` 显式设置 `top_cfg->disp_enable = true` 并 `sclr_top_set_cfg(top_cfg)`，同时 color-bar 后调用 `sclr_disp_reg_force_up()`。此版 10:19 镜像已构建，尚未收到用户测试结果。
- 2026-04-27 14:38 验证版：发现 GC9503 流程先 `mipi_tx_set_combo_dev_cfg()` 配好 DSI/DISP，随后 `gc9503_prepare_panel()` 又 `vip_toggle_reset()` 了 `disp/bt/dsi_mac`，这会清掉刚写入的 MIPI/显示寄存器。已把 `disp/bt/dsi_mac` 复位移动到 MIPI 配置之前，避免 color-bar/HS video 前的配置被复位擦掉。此版已构建，尚未收到用户测试结果。
- 用户回传 14:38 日志，能看到 `GC9503 finish: enable DISP color-bar pattern`，但屏幕是否出图仍未确认；用户建议试屏幕 IC 内置测试指令。
- 2026-04-27 14:51 验证版：在正常 GC9503 init 之后、切 HS video 之前追加 panel 内置 BIST 测试序列，使用与 GC9503/NT 系列相同的解锁页 `F0 55 AA 52 08 00`，再发送本 SDK 其他 BIST 分支里用到的 `EE 87 78 FF FF`。日志会出现 `GC9503 panel BIST: enable internal test pattern` 和两条 `send dtype=0x29 ... ret=...`。此版已构建，尚未收到用户测试结果。
- 2026-04-30 10:52 修正版：确认 `CXW034BS001A.pdf` 实际为 3.4 inch、`412RGB(W) x 960(H)`，已把 U-Boot 和 middleware 的 GC9503 active 分辨率从 480x800 改为 412x960，重新 `build_uboot && pack_burn_image` 通过。
- 用户回传 10:52 日志：`Init panel GC9503-CXW034-412x960`，BIST 两条命令返回 0；屏幕白底，中间有一条竖线并不断切换颜色。结论：电源/reset/LP 命令通路和 panel 内部显示路径已经工作，但这仍不是 DISP 输出的外部 video color-bar。
- 2026-04-30 11:52 验证版：新增 `GC9503_ENABLE_PANEL_BIST 0`，默认关闭内置 BIST，启动时应打印 `GC9503 panel BIST: disabled for DISP color-bar test`，只保留 HS video 后的 DISP color-bar。

## 下一步计划（3-5条actionable)

- 让用户烧录 2026-04-30 11:52 镜像并回传完整 U-Boot 日志，重点确认 `GC9503 panel BIST: disabled for DISP color-bar test` 和屏幕是否出现 DISP color-bar。
- 如果 11:52 仍只有白屏/单线/无外部 color-bar，下一步优先查 HS video 链路：lane map/PN swap、video timing、DSI HS clock，以及是否需要给 GC9503 发送退出 BIST/normal display 的寄存器。
- 同时需要用万用表确认屏座/转接板供电：LCD 逻辑电源、VCI/AVDD/AVEE 是否真实存在，尤其确认 `GPIOE1` 是否真的是屏电源而不是 camera reset。
- 如果供电正常，用示波器看 DSI CLK lane 是否有 HS 时钟输出；有时钟但无画面时再试 lane map/PN swap 组合。
- 若需要继续软件尝试，优先做一个 lane/PN 专用验证版，不再改 GPIO：当前配置是 `lane_id={MIPI_TX_LANE_1, MIPI_TX_LANE_0, MIPI_TX_LANE_CLK}`、`pn_swap={true,true,true}`，与 `docs/TFT/README.md` 的连线表表面一致，但仍需实测验证。
- 如果确认 FPC/转接板版本不同，或现有 `GC9503SSD 1.c` 并不对应 `CXW034BS001A 412x960`，需要重新拿对应屏厂初始化表，不要继续在现有 init table 上盲试。

## 关键文件路径（相对路径，一行一个）

u-boot-2021.10/cmd/cvi_vo.c
u-boot-2021.10/drivers/video/cvitek/cvi_mipi.c
u-boot-2021.10/drivers/video/cvitek/cvi_disp.c
u-boot-2021.10/include/cvitek/cvi_panels/dsi_gc9503_cxw034.h
middleware/v2/component/panel/sg200x/dsi_gc9503_cxw034.h
build/boards/sg200x/sg2002_licheervnano_sd/dts_riscv/sg2002_licheervnano_sd.dts
build/boards/sg200x/sg2002_licheervnano_sd/u-boot/cvi_board_init.c
u-boot-2021.10/drivers/video/cvitek/scaler.c
u-boot-2021.10/drivers/video/cvitek/scaler.h
u-boot-2021.10/drivers/video/cvitek/dsi_phy.c

## 还没搞清楚的问题

- 10:52 这版追加 `F0/EE` 内置 BIST 指令后确认能出动态单线/颜色变化，但不是外部 video color-bar。
- `GPIOE1 power-ct-gpio` 是否真接屏电源不确定；文档里 `GPIOE1` 也对应 Camera Reset，有硬件冲突嫌疑。
- 屏座上屏端逻辑电源、VCI、AVDD/AVEE 是否真实上电还没测。
- 当前 lane map/PN swap 与 `docs/TFT/README.md` 表面匹配，但没有用示波器确认 DSI CLK/DATA 实际波形。
- `GC9503SSD 1.c` 里的 `SSD_SEND()` 宏定义没有拿到，当前把长包按 Generic long 是基于 SDK 里其他 panel 的经验判断，不是厂商宏的直接确认。
