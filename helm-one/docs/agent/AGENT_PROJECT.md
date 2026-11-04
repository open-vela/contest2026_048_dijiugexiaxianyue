# AGENT_PROJECT — 当前任务：UI 在 lvgl_sim 上完整启动（2026-10-29 起）

## 目录约定

- 项目根按**固件仓库**算：`vendor/HelmOne/`（工作区根 `openvela/` 不是 git 仓库，
  其 `docs/` 是上游 openvela-docs，不往里写）。
- 阶段记忆：`docs/agent/AGENT_COORDINATION.md`
- 证据：`docs/agent/evidence/WP-UI<n>/R<k>/`
- 模拟器：`/home/jinsc/SDK/vela/lvgl_sim`（LVGL 直接取本仓库 `apps/graphics/lvgl/lvgl`）

## 目标

把 `boards/sf32lb52/helmone/ui/bicycle/src` 这套**真实 UI** 在 PC 模拟器上
以 **240×320 竖屏**（与板端一致）完整跑起来（能进主界面、能翻页、能截全页图），
然后**用模拟器继续做 UI 开发**。

背景：先做的 HTML 地图样式（`docs/osm/web` + `docs/map/STYLE.md`）是这一步的前奏 ——
目标观感已经定下来了，现在要让它出现在真正的 LVGL 渲染里，而不是只在浏览器里好看。

## 基线

- 固件仓库 `vendor/HelmOne` @ `752c6c0f`（分支 `master`），
  工作树有未提交改动：`docs/osm/web/*`（地图样式）、`docs/agent/*`（本文件与协调记录）。
- 模拟器 `lvgl_sim` 已可用：`./build.sh` + `--scene demo|vmap` + `--headless --shot`。
- 板子/串口在用户手上（`monitor-ctl`）；本任务**不接管串口、不烧录**。

## 关键约束（决定方案走向）

1. **屏幕 240×320 竖屏**（`HELM_STATUS_H 24` + `HELM_PAGE_H 296` = 320），
   与板端完全一致 ⇒ 启动阶段**不需要任何布局改动**。
   （2026-10-29 用户纠正：此前按"320×240 横屏"做的判断作废。）
2. **PC 上没有 EPIC**：板端 `src/draw/sifli/**` 编不了，模拟器走 LVGL 软件渲染。
   所以**性能数字不作数**，只看画面/交互/布局。
3. **数据源要打桩**：GNSS/传感器/BLE/KV/文件系统在 PC 上没有，先给"有数据但不真实"
   的假源（速度/里程/心率/轨迹），保证页面不是空的。
4. **字体差异**：板端是定制点阵字库，PC 侧用 FreeType 挂系统 TTF（`sim_font_cjk`）。
5. **板级 MMIO 会崩**：`vmap_view.c:69 VMAP_PERF_PROBE 1` 读写 DWT 寄存器，
   x86 必 SIGSEGV ⇒ 用构建期副本关掉，不改 vendor 源码。

## 验收

- 模拟器里 240×320 能进主界面，页面切换、按键/触摸交互可用（截图留证）。
- 每个页面有截图对照（`docs/agent/evidence/WP-UI*/R*/shots/`）。
- 不改板端行为：`vendor/HelmOne` 里对 `ui/bicycle` 的改动必须能回退，
  且板端仍可编译（`cmake_out/helmone_nsh`，由后续阶段验证）。
- 验证口径：**快速冒烟（本轮）** = 模拟器起得来 + 截图非空 + 无断言；
  **定向回归（收尾）** = 模拟器全页走一遍 + 用户上板验证（我只给步骤和预期）。

## 工作包

| ID | 目标 | 状态 |
|---|---|---|
| WP-UI1 | 依赖摸底：启动链路、外部依赖三档分类、分辨率假设、最小可编译子集 | 完成（见证据） |
| WP-UI2 | 在 lvgl_sim 里编出 UI（compat 层 + 服务桩），240×320 出主界面 | 进行中 |
| WP-UI3 | 全页截图对照 + 观感问题清单（含新地图样式落地位置） | 待定 |
| WP-UI4 | 按清单做 UI 开发（样式落地 + 修问题） | 待定 |

## 禁止

- 不提交、不推送；不烧录；不碰用户手上的串口/monitor hub。
- 不把板端代码改成"只能在 PC 上跑"——compat 层必须隔离在 `lvgl_sim` 或独立目录，
  `ui/bicycle` 的改动只允许是"两边都能编"的。
- 不在模拟器上得出性能结论（没有 EPIC）。

---

## 历史任务（已收口）：低功耗优化（2026-10-29）

用户判断：运行时软件侧已接近地板，不立项 CPU 微优化；剩余只有产品/硬件口径
（背光、GNSS 星座/PSM、BLE 间隔、面板）与高风险项（72 MHz 长驻留档）。
方案与证据见 `docs/power/PLAN.md`、`docs/agent/evidence/WP-LP*/`。
硬约束：代码在 PSRAM ⇒ 无 deep sleep；DVFS 仅 144/240；板上无电流检测。

## 历史任务（已收口）：vmap 瓦片地图效率

`tile rect = 视口 + 至多一格` 的选片算法已落地；道路图层分桶、多边形 DDA 等
见 `docs/map/perf/STATUS.md` 与 `AGENT_COORDINATION.md` 第三轮。
