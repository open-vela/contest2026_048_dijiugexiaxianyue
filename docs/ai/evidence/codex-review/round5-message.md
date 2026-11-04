C1 — FIXED — lvgl_page/helm_shell.c:6288 — 上轮三项均已修复：

- **500 ms 内重建到来**：`7937` 清 pending，重建末尾调用入场，`6387–6389` 删除兜底 timer；动画完成后为 `opa=COVER、translate_y=0`。
- **重建永不来**：pending 保持 true，兜底在 `6292–6294` 停动画并将三组落到上述末态。
- **timer 创建失败**：`6377–6379` 覆盖同一数组中的全部三组。settle 仅赋固定末值，重复调用不会启动动画或累积位移。
- **注释**：`6215–6217` 已准确描述“保持起始透明态，超时落末态”。

新发现：[P2] **兜底落末态后，后续重建仍会造成“出现→消失→再出现”** — [lvgl_page/helm_shell.c:6377](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6377)。

反例：pending=true 时返回主界面，timer 创建失败，三组立即 settle；页面随后按 220 ms fade 显示这些控件。转场结束后，`7937–7938` 执行重建，`7865` 再调用入场，`6311` 又将新控件置为透明后淡入。超时兜底后重建才到来也存在同样缺口。代码没有记录“本次入场已通过兜底显示”，因此 `6213` 的“视觉上只播一轮”仍不成立。需要保留重建，同时抑制该次重建后的再次入场。

本轮核查路径未发现永久透明残留。HEAD、四份哈希在审查前后均吻合；仅静态审查，运行及故障注入 NOT_RUN。

结论: REJECT