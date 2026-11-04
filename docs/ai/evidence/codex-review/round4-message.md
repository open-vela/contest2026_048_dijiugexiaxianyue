C1 — PARTIAL — [helm_shell.c:6356](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6356) — 正常重建路径已消除旧内容先亮、随后消失的问题；但兜底条件写反，仍存在永久透明路径。

1. **不可见窗口与单轮入场**：pending 分支同步置 `opa=0 / translate_y=12`，不启动动画；重建清除 pending 后才播一次。设重建末尾调用时刻为 R，三组分别在 R+40、R+120、R+200 ms 开始，R+400 ms 全部结束。若 R≈220 ms，则约 260/340/420 ms 开始、620 ms 全显；实际还要加调度与重建耗时。正常路径符合“背景先出现、三组随后只播一轮”。

2. **定时器释放安全**：`helm_shell_enter_anim()` 删除定时器后置 NULL；超时回调只置 NULL，没有自行释放。仓库 [lv_timer.c:343](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/apps/graphics/lvgl/lvgl/src/misc/lv_timer.c:343) 在回调前递减次数、回调后自动删除，并通过删除标志避免再次访问已释放定时器；handler 也会重启链表遍历。当前同一 UI 线程内的回调链删除未发现双重释放。

3. **重复 will_appear**：pending 仍为真时再次调用，只会继续保持透明，复用尚未到期的定时器，不叠加动画；若定时器已经到期，则重建一个新定时器，但同样无法兜底。pending 已清除时，立即重复调用会停止旧动画并重起，不会并存两套动画。

4. **指定两处注释已修正**：`helm_shell.c:6246` 的 `HELM_SB_IN_DY(12)` 与实际参数一致；`helm_shell.h:20` 已写两处触发。

新发现：

- **[P2] 兜底判断反向** — [helm_shell.c:6283](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6283)：重建一直不来 ⇒ pending 一直为真 ⇒ 500 ms 回调直接 `break` ⇒ 指针已清空、定时器自动删除，三组仍为透明且下移 12px。实际行为与修复说明相反。
- **[P2] 定时器分配失败未恢复末态** — [helm_shell.c:6363](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6363)：控件已隐藏，`lv_timer_create()` 返回 NULL 后直接返回；若重建未发生，同样持续透明。
- **[P3] 总说明仍与实现冲突** — [helm_shell.c:6215](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6215)：仍写“有待重建时……只落末态”，实际是保持起始透明态。

HEAD 与四个哈希在审查前后均一致。仅做只读源码核查；上板目视及运行回归 NOT_RUN。

结论: REJECT