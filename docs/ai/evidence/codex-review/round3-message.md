C1 — PARTIAL — [helm_shell.c:6328](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6328) — 阻止了第一轮子控件动画，但没有消除“先出现、再消失、再出现”的视觉路径：

- pending 分支把旧控件设为 `opa=COVER`。随后页面根执行 **220 ms 淡入**，旧内容会随之出现。
- [helm_shell.c:7876](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:7876) 要等转场结束才允许重建；重建末尾再次调用入场函数，新控件在 [helm_shell.c:6280](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6280) 同步设为 `opa=0`，再错峰淡入。因此旧内容已经显示后，三个区域仍会突然消失再出现。

其余核对：

1. **标志时序正确。** 有效 KV 加载触发通知，帧后 `helm_theme_poll_apply()` 登记 pending；Startup 或菜单在栈顶时，apply 的门保留该标志。正常返回主界面时为真。初次 push 空壳时不保证为真，但此时 `attached=false`，直接返回。重建入口在 **7881 行先清 false**，末尾动画不会被自身挡住。
2. **没有新增永久透明路径。** pending 分支停止动画并显式设置 `COVER/0`，不依赖重建最终发生；即使始终不返回主界面，也不会因此停在 `opa=0`。
3. **声明移动无语义变化。** 全文件只有 **6203 行**一处定义，仍是静态存储期、隐式初始化为 false。
4. **P3 — PARTIAL。** 200/80/40 ms、三组结束时间及 ≤400 ms 与参数一致；但 [helm_shell.c:6244](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.c:6244) 仍写“下移 10px”，实际为 12px；[helm_shell.h:20](/home/jinsc/SDK/vela/openvela/vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/src/lvgl_page/helm_shell.h:20) 仍写“只在页面出现时调一次”，遗漏重建末尾入口。

新发现：无。

HEAD 与四个源码哈希均吻合，审查结束时未变化。运行验证：NOT_RUN（只读静态审查）。

结论: REJECT