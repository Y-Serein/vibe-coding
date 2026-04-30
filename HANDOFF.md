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
