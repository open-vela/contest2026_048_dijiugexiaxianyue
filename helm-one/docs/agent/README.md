# agent/ — 协作记忆（不是产品文档）

本目录按协作协议保存**开发过程的阶段记忆**，给下一次会话恢复上下文用。
读作品本身请回 [../README.md](../README.md)；这里可以整目录跳过。

| 文件 | 是什么 | 状态 |
| --- | --- | --- |
| [AGENT_PROJECT.md](AGENT_PROJECT.md) | 当前工作包任务书：目标、目录约定、验收口径 | 现行·2026-10-29 起（UI 在 lvgl_sim 上跑起来） |
| [AGENT_COORDINATION.md](AGENT_COORDINATION.md) | 分阶段流水账：vmap 效率 → 航向朝上旋转 → UI 模拟器，含子任务、证据索引、异常 | 历史·2026-10-27~29 |

## 约定

- 事实源就是这两个文件；`evidence/` 按协议在 `docs-root/agent/evidence/WP-<id>/R<rev>/` 落盘，
  **本提交仓的精选副本没有收录证据目录**（截图/日志留在主开发树）。
- 只有主对话（协调层）写这两个文件；执行子 Agent 只读。
- 与产品文档冲突时以产品文档为准（尤其 [../system_states.md](../system_states.md) 的口径），
  协作记录只描述"当时怎么做的"。
