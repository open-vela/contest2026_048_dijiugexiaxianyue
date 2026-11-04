C1 — FIXED — [helm_shell.c:7874](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:7874) — 重建保留，仅在 `s_sb_in_visible == false` 时补播。三种时序核对如下：

- **a）500 ms 内重建：播一次。** pending 分支置 `false` 并保持透明（6369–6373）；主题应用先清 pending（7948），重建末尾补播，起播前置 `true`（6407）。
- **b）兜底后重建：不补播。** 兜底落末态并置 `true`（6296–6300），随后重建跳过入场，新控件直接可见。定时器创建失败同样处理（6383–6386）。
- **c）无 pending：页面出现播一次，后续重建不补播。** 正常入口置 `true` 后启动三组动画（6407–6409），重建不会再启动。

**菜单返回应播，现在也会播。** [map_page.c:5784](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/live_map/map_page.c:5784) 每次 `map_will_appear()` 都直接调用入场入口；入口没有用旧的 `true` 拦截页面出现。未换主题时直接重播；菜单内换主题后，pending 在菜单期间保留，返回时入口将 visible 重置为 `false`，待转场结束重建后播一次。因此不需要在离开页面时额外复位。

新发现：无。本轮范围内未发现新的永久透明或重复播放路径；动画分配失败、定时器分配失败均有落末态处理。

HEAD、四个哈希在审查前后均一致。仅静态只读审查；构建、运行及上板目视：NOT_RUN。

结论: ACCEPT