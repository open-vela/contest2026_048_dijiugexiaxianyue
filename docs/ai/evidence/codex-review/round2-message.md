审查前后 HEAD 均为 `2c4fedad`，四个源码哈希均与本轮提供值一致。

- **C1 — PARTIAL — [helm_shell.c:7789](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:7789) —** 存在第二轮可见动画：菜单内换主题后返回，`map_will_appear():5784` 先启动入场；三组实际时间为 **40–240 / 120–320 / 200–400 ms**。页面淡入约 220 ms 完成后，nav busy 清零，待办重建通过门控，删除旧控件，再调用入场，将新控件重新置为 `opa=0、translate_y=12`。已出现的内容因此再次消失并入场。“删除旧动画”只能避免节点并存，不能保证视觉上只播一轮。
  
  `helm_apply_view():3913` 仅在 `HELM_PAGE_STANDBY` 显示 `sb_card`，其他页走 settle；`helm_obj_shown():502` 只检查对象自身 HIDDEN。正常返回路径中，页面完成回调先恢复 shell，再清 busy；未发现可被主题轮询利用的“页面仍在淡入但 busy 已假”窗口。**菜单开着时，`:7856` 的当前页门控仍能挡住重建。**

- **C4 — FIXED — [helm_shell.c:6283](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6283) —** start 返回 NULL 后立即 settle，`:6249` 同时写回 `opa=COVER、translate_y=0`，覆盖同步起始态的两个属性。隐藏卡片或挂起入口也会 stop＋settle；动画中途挂起只跳过 exec，LVGL 仍推进时间并执行 completed，再 settle。重建删除的是控件本体及其动画。本轮所查路径未发现其他永久透明分支。

- **C3 — FIXED — [startup_page.c:777](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/startup/startup_page.c:777) —** finish 仅执行 pop。`lv_pm_core.c:1261` 在返回前同步调用前页 `will_appear`；Startup 的 no_anima 退出同步完成隐藏和删除。开机资源创建已在 `map_page.c:2771` attach shell。`:5766` 的 `s_boot_deferred` 条件只包围页面动画配置，`:5784` 的入场调用始终执行，因此删除补调不会丢失开机入场。

本轮新发现：**P3，注释过期**——`helm_shell.c:6205、6293` 仍声称主题重建不重播、触发点只有一处，与新增调用矛盾；说明中的 170/45/10 和 260 ms 也与实际参数不符。

运行及上板观感验证：NOT_RUN，本轮仅做只读源码审查。

结论: REJECT