# Helm One 工程文档

板卡包（构建树里是 `vendor/HelmOne/docs/`）的**工程文档源**：78 篇工程文档 + 5 篇目录说明
（本索引、`agent/` `debug/` `power/` `tools/` 各一份），外加地图流水线、字体子集、UI 图册三类素材。仓库根的 [`docs/`](../../docs/) 是**面向提交材料的精选副本**，
两者关系见文末。

> 维护口径：**根级文件名是"公共 API"**（源码注释、defconfig、仓库 README 直接引用
> `docs/sd_partition.md` 这类路径），所以根级 28 篇工程文档（不含本索引）保持扁平、不改名、不挪位；
> 主题子目录放模块内部设计、流水线与一次性记录。新增文档先问一句"会不会被代码引用"——
> 会 → 根级并登记到本表；不会 → 对应子目录 + 登记。

---

## 1. 五条阅读路线

| 你要干什么 | 按这个顺序读 |
| --- | --- |
| 第一次接触这个作品 | [product.md](product.md) → [system_states.md](system_states.md) → [map/README.md](map/README.md) → [pitch/README.md](pitch/README.md) |
| 编译、烧录、上板 | [build_guide.md](build_guide.md) → [board_guide.md](board_guide.md) → [sd_partition.md](sd_partition.md)（启动链看 [boot_2sfbl.md](boot_2sfbl.md)） |
| 现场排障（死机/卡顿/掉外设） | [tools/debug_workflow.md](tools/debug_workflow.md) → [diag.md](diag.md) → [diag_log.md](diag_log.md) → [gnss_ble_diag_log.md](gnss_ble_diag_log.md) |
| 做地图数据 / 改渲染 | [osm/README.md](osm/README.md) → [map/PRODUCTION.md](map/PRODUCTION.md) → [map/RUNTIME.md](map/RUNTIME.md) → [osm/CITIES_MAP.md](osm/CITIES_MAP.md) |
| 重新 `repo sync` 后救回改动 | [required_patches.md](required_patches.md) → [pitch/README.md](pitch/README.md) → [pitch/patches/README.md](pitch/patches/README.md) |

---

## 2. 产品与状态口径

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [product.md](product.md) | 作品卖点、受众、车上功能与工厂模式叙事（5 分钟演示脚本在这） | 现行 |
| [system_states.md](system_states.md) | 整机状态机、按键矩阵、上屏/存储/交互/电源阈值——**口径唯一权威** | 现行 |
| [开发计划.md](开发计划.md) | 开发板 + 面包板阶段的分阶段路线与采购（2026-05-30） | 历史·已走完 |
| [retire_eval.md](retire_eval.md) | 抽换驱动"能否回退上游"的判据与 `retire-eval` 分支存档 | 计划·2026-09-24 |

## 3. 接入与构建

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [board_guide.md](board_guide.md) | 把新板卡接进 OpenVela repo 的 manifest、改板与 menuconfig 要点 | 现行 |
| [build_guide.md](build_guide.md) | `vendor/HelmOne` 编译、烧录、清编与可复现性审计 | 现行 |
| [required_patches.md](required_patches.md) | 干净上游树必补的 zblue / LVGL 改动清单（`repo sync` 后照着做） | 现行 |
| [gpio_pinmux_guide.md](gpio_pinmux_guide.md) | 引脚交换矩阵、默认脚表、UART/I2C 驱动用法——**改脚查表权威** | 现行 |
| [ai_native_skill.md](ai_native_skill.md) | 把板级事实固化成 AI Skill 的结构与安装约定 | 现行 |

## 4. 启动链

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [boot_2sfbl.md](boot_2sfbl.md) | 二级 boot 启动链、msh 命令、KV 槽与烧录坑 | 现行 |
| [nuttx_boot_flow.md](nuttx_boot_flow.md) | 上电跳转顺序，CMake/ROMFS/builtin 的隐性资源注册 | 现行 |
| [test_app_guide.md](test_app_guide.md) | `test_app` 四种启动方式与默认方式（板级异步 spawn） | 现行 |

## 5. 存储与固件

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [sd_partition.md](sd_partition.md) | 卡上分区表**唯一源**、余量窗口、改表步骤（4 篇存储文档的枢纽） | 现行 |
| [sd_recovery.md](sd_recovery.md) | SDIO 控制器卡死用 RCC 模块复位救活，含判死闸门与取证字段 | 现行 |
| [factory_firmware.md](factory_firmware.md) | 独立工厂镜像做 USB/MTP 全盘拷文件（boot 里不做 MTP） | 现行 |
| [fs_firmware.md](fs_firmware.md) | 产品固件按版本存 KV 槽、优先于 SD main 加载 | 现行 |
| [psram_cache_wb_audit.md](psram_cache_wb_audit.md) | 开 `PSRAM_CACHE_WB` 前必须补的 invalidate 缺口与实测 | 现行·末节待办 |
| [debug/mklfs.md](debug/mklfs.md) | SD 上 LFS 挂载 8 s→0.47 s 的根因复盘 | 历史·2026-08-13 |
| [debug/mtp_upload.md](debug/mtp_upload.md) | MTP 上传慢 / gvfs 报错的分层排查 | 历史·2026-09-07 |

## 6. 运行时与诊断

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [diag.md](diag.md) | 独立 diag 线程轮询外设、按注册表 restart 的机制 | 现行 |
| [diag_log.md](diag_log.md) | 运行时日志只落异常级并定期写盘的机制与限制 | 现行 |
| [dvfs.md](dvfs.md) | HCPU 调频：静止 72M、交互 boost、240M floor | 现行 |
| [watchdog_irq_priority.md](watchdog_irq_priority.md) | 高优先级中断写坏定时器链表导致 clip 的修复与验证 | 现行·已上板 8.9 h |
| [tools/debug_workflow.md](tools/debug_workflow.md) | 日常调试口径：编译/烧录/看日志不抢 tty | 现行 |
| [board_anomaly_scan_2026-10-14.md](board_anomaly_scan_2026-10-14.md) | 5 项已修代码异常 + 1 项设计风险的扫描清单 | 历史·2026-10-14 |

## 7. 蓝牙

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [bluetooth_guide.md](bluetooth_guide.md) | 使能架构、defconfig 关键项、HCI FAQ（**总入口**） | 现行 |
| [ble/README.md](ble/README.md) | BLE 文档索引、代码位置与阅读顺序 | 现行 |
| [ble/companion_impl_plan.md](ble/companion_impl_plan.md) | Companion 决策记录 + 协议 v0.3（权威版本） | 现行 |
| [ble/ble_sensor.md](ble/ble_sensor.md) | 传感器 Central / 双角色与带宽硬约束 | 现行 |
| [ble/pairing_and_radio.md](ble/pairing_and_radio.md) | 配对准入、广播分档、退避与并发纪律 | 现行 |
| [ble/directed_reconnect.md](ble/directed_reconnect.md) | 已绑定手机的 RPA/RAL 定向回连 | 现行 |
| [ble/companion_recording.md](ble/companion_recording.md) | GPX 记录与展示分离（Phase 1） | 计划 |
| [ble/companion_bringup_notes.md](ble/companion_bringup_notes.md) | 早期 bring-up 手记与踩坑方法论 | 历史·2026-08-14 |
| [ble/companion_design.md](ble/companion_design.md) | BLE 总体设计 v0.1 | 历史·2026-06-15 |
| [ble/companion_scheme.md](ble/companion_scheme.md) | 最初方案初稿（已被 impl_plan 取代） | 历史·已 supersede |

## 8. GNSS

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [gnss_ble_diag_log.md](gnss_ble_diag_log.md) | GNSS / BLE 日志字段含义与取日志方法速查（最长一篇） | 现行·§2.2 结论已自证伪 |
| [gnss_eph_inject.md](gnss_eph_inject.md) | RINEX→UBX-MGA 星历打包字段表与四条验证判据 | 现行·文末有未结项 |
| [gnss_speed_filter.md](gnss_speed_filter.md) | 速度选源与静止门：步骤 1/2 已落地，解析与 3/4 待做 | 计划·2026-09-17 |

## 9. 显示与 UI

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [ui/README.md](ui/README.md) | 板端 UI 图册索引（P/S/N/O 系列）与命名规则 | 现行 |
| [ui/MAIN_DASHBOARD_LAYOUT.md](ui/MAIN_DASHBOARD_LAYOUT.md) | App 自定义 8 格主界面布局设计稿 | 计划·2026-11-02 |
| [ui/DASHBOARD_WIDGETS_AND_MIGRATION.md](ui/DASHBOARD_WIDGETS_AND_MIGRATION.md) | 格子控件目录与现有页面迁移映射 | 计划·2026-11-02 |
| [ui_epic_performance_audit_2026-10-15.md](ui_epic_performance_audit_2026-10-15.md) | EPIC 没生效的真因、优化排序与 O1/O2 实测 | 历史·2026-10-15 |
| [st7789_spi_fb_dma_guide.md](st7789_spi_fb_dma_guide.md) | ST7789S 经 SPI2/DMA 刷 fb 的架构与调试坑（**已改 NV3031A，待删**） | 历史·2026-06 |

## 10. 地图与 OSM 流水线

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [map/README.md](map/README.md) | vmap 文档索引、两种打包模式与阅读顺序 | 现行 |
| [map/LIVEMAP.md](map/LIVEMAP.md) | 设备端 LiveMap 实现约定与硬约束 | 现行 |
| [map/RUNTIME.md](map/RUNTIME.md) | 加载与增量渲染原理 | 现行 |
| [map/PRODUCTION.md](map/PRODUCTION.md) | OSM → `.vt` → 打包的 PC 制作链路 | 现行 |
| [map/STYLE.md](map/STYLE.md) | 地图配色/线宽/标签的**唯一口径** | 现行·2026-11-01 |
| [map/GPU_ACCEL.md](map/GPU_ACCEL.md) | EPIC 搬运分层、旋转代价与移植 API | 部分历史·2026-09-25 |
| [map/perf/README.md](map/perf/README.md) | 光栅化优化记录与验证方法（专题入口） | 现行索引 |
| [map/perf/MEASUREMENTS.md](map/perf/MEASUREMENTS.md) | 埋点、时钟标定与改前改后原始数据 | 数据 |
| [map/perf/STATUS.md](map/perf/STATUS.md) | 渲染性能收口结论与已否方向 | 现行·2026-10-28 |
| [map/perf/TESTS.md](map/perf/TESTS.md) | 三层验证索引、套件清单与缺口 | 现行 |
| [map/perf/POLYGON_DDA.md](map/perf/POLYGON_DDA.md) | 多边形填充去逐行软除（已落地 −7.6%） | 历史·2026-09-25 |
| [map/perf/PUMP_PERIOD.md](map/perf/PUMP_PERIOD.md) | 渲染泵 20→10 ms 实测否掉 | 历史·2026-09-25 |
| [map/perf/ROT_AREA.md](map/perf/ROT_AREA.md) | 航向朝上子矩形旋转：已上板、已回退 | 历史·2026-10-28 |
| [osm/README.md](osm/README.md) | OSM 工具链操作指南（目录入口） | 现行 |
| [osm/DESIGN.md](osm/DESIGN.md) | 早期系统架构与二进制格式设计 | 历史设计 |
| [osm/ROUTING.md](osm/ROUTING.md) | VGRF 路网、Dijkstra 与偏航重规划 | 现行 |
| [osm/CITIES_MAP.md](osm/CITIES_MAP.md) | 滁州/南京/广州 3 km 网格合包 | 现行·2026-09-06 |
| [osm/CHUZHOU_MAP.md](osm/CHUZHOU_MAP.md) | 旧 5 km `dNNN/rNNN` 滁州流程 | 历史 |
| [osm/MAP_RECORD.md](osm/MAP_RECORD.md) | 滁州 demo 数据集参数与统计 | 历史·2026-06-19 |
| [osm/web/README.md](osm/web/README.md) | 浏览器框选生成器（Flask + Leaflet） | 工具 |

## 11. 上游改动（`repo sync` 会冲掉的部分）

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [pitch/README.md](pitch/README.md) | 上游必改件总览、打法、补丁清单与 `--check` 命令 | 现行·2026-11-04 复核 |
| [pitch/ble.md](pitch/ble.md) | zblue `id.c` 扫描 `-EACCES` 回退 | 现行 |
| [pitch/lvgl.md](pitch/lvgl.md) | LVGL 官方构建脚本避让 + EPIC 头 | 现行 |
| [pitch/ble-hci-h4-rx-reopen.md](pitch/ble-hci-h4-rx-reopen.md) | HCI H4 读错误的有界重开 | 现行 |
| [pitch/zblue-scan-stop-null-ctx.md](pitch/zblue-scan-stop-null-ctx.md) | scan-stop 拆除窗口空指针（panic 根因） | 现行 |
| [pitch/nuttx-kconfig-quote-blank.md](pitch/nuttx-kconfig-quote-blank.md) | `.config` 空行触发 CMake ≥3.23 报错 | 现行 |
| [pitch/overrides.md](pitch/overrides.md) | `vela_override` 抽换层 19 条索引快照 | 快照·2026-09-19 |
| [pitch/patches/README.md](pitch/patches/README.md) | 补丁 ↔ 仓库 ↔ 说明文档映射与自查 | 现行 |
| [pitch/replace/README.md](pitch/replace/README.md) | 可整文件覆盖的上游文件与 `cp` 命令 | 现行 |

## 12. 工具与字体

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md) | 板级工具**全量命令说明**（build/flash/monitor/…） | 现行 |
| [tools/debug_workflow.md](tools/debug_workflow.md) | 调试现场的操作口径（上表已列，入口级） | 现行 |
| [font/README.md](font/README.md) | 字库子集/嵌入工具链怎么用 | 现行 |
| [font/FONT_CHARSET.md](font/FONT_CHARSET.md) | 字表口径与覆盖范围 | 现行 |

## 13. 过程记录与协作记忆

| 文档 | 讲什么 | 状态 |
| --- | --- | --- |
| [agent/AGENT_PROJECT.md](agent/AGENT_PROJECT.md) | 当前工作包任务书 | 现行·2026-10-29 起 |
| [agent/AGENT_COORDINATION.md](agent/AGENT_COORDINATION.md) | 分阶段协作流水账（vmap / 低功耗 / UI 模拟器） | 历史·2026-10-27~29 |
| [power/PLAN.md](power/PLAN.md) | 低功耗 Phase 1 只读调查与优化清单（无代码） | 历史计划稿·2026-10-29 |

---

## 14. 素材与脚本（非 Markdown）

| 路径 | 是什么 | 备注 |
| --- | --- | --- |
| [osm/osm/chuzhou.osm](osm/osm/chuzhou.osm) | 滁州 OSM 原始数据 | 6.4 MB，流水线输入 |
| [osm/vgraph.bin](osm/vgraph.bin) / [osm/graph.vgrf](osm/graph.vgrf) | 编好的路网图（二进制 / 文本） | 可复现，勿手改 |
| [osm/data/](osm/data/) | 汉字覆盖表（`TSCharacters`、GB2312 子集等） | 字库子集输入 |
| [osm/web/static/](osm/web/static/) | Leaflet 及其本地副本 + 可视化前端 | 第三方，勿改 |
| [map/perf/verify/](map/perf/verify/) | 宿主侧差分测试桩（`harness.c`、`nav_window_replay.c`、假头、`navAhead/`） | 由 [map/perf/TESTS.md](map/perf/TESTS.md) 索引 |
| [ui/](ui/) | 板端 UI 图册：`P*`/`S*` 现状、`legacy/O*` 回退前、`night/N*` 夜间、`statusbar/` 5× 放大 | 索引见 [ui/README.md](ui/README.md) |
| [ui/tools/panel_wash.py](ui/tools/panel_wash.py) | 半透半反面板"洗白"预览 | — |
| [pitch/patches/](pitch/patches/) | 8 个 `.patch` | 说明见 [pitch/patches/README.md](pitch/patches/README.md) |
| [pitch/replace/](pitch/replace/) | 4 个可整文件覆盖的上游文件 | 说明见 [pitch/replace/README.md](pitch/replace/README.md) |
| [tools/](tools/) | 构建/烧录/串口脚本（`vela_my_vendor_tools.py`、`serial_hub.py`、`make_sd_*.sh` 等） | 说明见 [tools/README.md](tools/README.md) |
| [font/](font/) | 字体子集与嵌入脚本（`subset_font.py`、`embed_ttf.py`） | 说明见 [font/README.md](font/README.md) |

---

## 15. 本轮修掉的 5 处不一致

1. ~~[pitch/README.md](pitch/README.md) 写"六份补丁 = 上游的全部改动"~~ —— **已改为八份**，
   一键 `apply()` 与文末反向 dry-run 都补上了 `nuttx-psram-cache-wb.patch` 与
   `nuttx-lfs-prog-error.patch`，自查判据同步改成"8 份补丁覆盖的文件"。
2. ~~[pitch/patches/README.md](pitch/patches/README.md) 表格漏 `nuttx-lfs-prog-error.patch`~~ ——
   **已补行**（含它改的两个文件与症状链）。
3. ~~[ui/README.md](ui/README.md) 引用本仓没有的截图~~ —— **已改写**成"截图在主开发树
   `docs/agent/evidence/`，本提交仓只带结论"，并列出子路径表。
4. ~~[osm/web/README.md](osm/web/README.md) 的包格式段过期~~ —— **已改为现行**
   `lon*/lat*/x*_y*.vpk` 3 km 网格，旧 `map.idx + rNNN.vpk` 降为"仍能读"的兼容说明。
5. ~~重复素材~~ —— `ui/statusbar/电池_V2_5x.png` 原来与 S01 整图字节相同，**已按像素实测裁出**
   电量图标（15×9 电池 + 2×4 极，5×，100×60）；`osm/web/out/` 不是残留，是工具的输出根
   （`app.py` 的 `OUT_ROOT`，已在 `osm/web/.gitignore` 里）。

> 另外一处不是错误、但容易误读：[gnss_ble_diag_log.md](gnss_ble_diag_log.md) §2.2 记录了
> "`poll` 超时钳位"这条推断**被自己证伪**的过程（钳位没解决问题，真因是 tick 基准被打断）。
> 引用该节时按结论读，别把钳位当修复。

---

## 16. 与仓库根 `docs/` 的关系

仓库根 `docs/`（提交材料用的精选副本）里有 5 篇本目录文档的副本，**链接已改写成
`../../helm-one/docs/...` 前缀**：

| 精选副本 | 与本目录的差异（已把 `../../helm-one/docs/` 前缀归一化后比对） |
| --- | --- |
| `docs/build/board_guide.md` | **内容一致**（2026-11-04 已把缺的 §7.3.1 同步过去） |
| `docs/build/sd_partition.md` | **内容一致**，只有链接前缀不同 |
| `docs/build/system_states.md` | **内容一致**，只有链接前缀不同 |
| `docs/build/vela_my_vendor_tools.md` | **内容一致**（2026-11-04 已同步 §8.1/§8.2/§8.5 串口租约与 2026-09-21 `monitor-ctl` 一节） |
| `docs/ai/ai_native_skill.md` | 与本目录一致 |

> 改文档时以**本目录为准**，需要同步提交材料时再把内容与链接一起搬过去；
> 反向从 `docs/build/` 往这里抄会丢更新。

---

## 17. 本次整理记录（2026-11-04）

**结构**

- 新增本索引 + [agent/README.md](agent/README.md)、[debug/README.md](debug/README.md)、
  [power/README.md](power/README.md)、[tools/README.md](tools/README.md)。
- 未移动、未改名、未删除任何文档：根级文件名被源码与仓库 README 直接引用，挪位会连带
  `boards/`、`chips/`、`boot_loader/`、`configs/` 里的注释与 AI Skill 一起改，收益不抵风险。
- 整理前基线：491 条相对链接、0 断链、21 篇文档没有任何入链；整理后：**全部 Markdown 可从本索引
  直达、0 断链**（数字见提交时的核验输出）。
- 原本没有任何入链的文档（如 [st7789_spi_fb_dma_guide.md](st7789_spi_fb_dma_guide.md)、
  [gnss_eph_inject.md](gnss_eph_inject.md)、[tools/debug_workflow.md](tools/debug_workflow.md)、
  [map/LIVEMAP.md](map/LIVEMAP.md)、[osm/MAP_RECORD.md](osm/MAP_RECORD.md)）现在都能从本索引到达。

**内容修正**（逐条见 §15）

- `pitch/README.md`：六份→八份补丁，一键脚本补上 `nuttx-psram-cache-wb` 与 `nuttx-lfs-prog-error`，
  反向 dry-run 与"全树文件数"自查判据同步修正。
- `pitch/patches/README.md`：补 `nuttx-lfs-prog-error.patch` 行（改的两个文件 + 症状链）。
- `ui/README.md`：证据路径改成"主开发树 `docs/agent/evidence/`，本仓未收录"；电池图重新裁切。
- `osm/web/README.md`：包格式改为现行 3 km 网格（旧格式降为兼容）。
- 仓库根 `docs/build/board_guide.md`、`vela_my_vendor_tools.md`：同步为最新内容（链接前缀保持
  `../../helm-one/docs/...`，从 `docs/build/` 起逐条校验 0 断链）。
