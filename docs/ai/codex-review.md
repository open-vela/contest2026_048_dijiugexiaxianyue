# codex-review：把「第二双眼睛」做成 Skill

本作品的**第三个自定义 Skill**。前两个管知识与方法，这个管**验收**：让另一个模型（Codex）
在**只读**前提下独立审代码，输出逐项缺陷与最终 `ACCEPT / REJECT`。

| 项 | 值 |
| --- | --- |
| 版本 | **1.2.0**（首版 2026-10-24，2026-10-28 更新） |
| 审查模型 | 固定 `gpt-6.1-sol`（每次显式传 `-m`，不依赖本机默认配置） |
| 运行形态 | Skill：`SKILL.md` + `references/brief-template.md`；`install.sh` 一键装 |
| 沙箱 | `codex exec -s read-only`：不改代码、不构建、不烧录、不碰串口、不联网、不委派 |

## 它解决什么问题

AI 结对写 25 万行，最大的风险不是"写错"，而是**"写错了没人看见"**：

- 实现方自测（smoke、宿主回归）只能证明"我以为对的部分对"；
- 同一个模型回头检查自己的产出，倾向于复述自己的假设；
- 上下文越长，越容易把"看起来合理"当成"验证过"。

所以要一个**不同模型 + 只读工具 + 有界范围**的审查者，并且它的结论必须可复核、可追溯。

## 流程（五步）

| 步 | 做什么 | 防的是什么 |
| --- | --- | --- |
| 1. 定对象 | 未提交改动 / 某个 commit / 相对分支 / 某个工作包 | 范围里混进无关改动被判 |
| 2. 冻结基线 | 记录 `sha256sum`，发送前再核对一次；跑完再核对一次 | 审查期间工作树漂移 ⇒ 结论 **STALE** |
| 3. 有界 brief | 只列需要读的文件与接口，控制 Codex 配额 | 一次喂整棵树 ⇒ 配额耗尽 + 泛泛而谈 |
| 4. 只读执行 | `codex exec -m gpt-6.1-sol -s read-only` | 审查者顺手改代码 / 构建污染工作树 |
| 5. 提取结论 | 逐项 `FIXED / PARTIAL / NOT FIXED` + 最终 `ACCEPT / REJECT`；无结论标 **INCOMPLETE** | 把"没审完"当成 PASS |

**硬规则**：主对话不得替 Codex 下结论，也不得把实现方自测写成 Codex 的 PASS；审查期间主对话
只做范围、证据、启动、校验与转述。

## 本作品里的真实记录

不是"装了没用"。开发期围绕地图与 UI 做过成轮次的独立审查，记录在
[`helm-one/docs/agent/AGENT_COORDINATION.md`](../../helm-one/docs/agent/AGENT_COORDINATION.md)：

| 轮次 | 对象 | 结论 | 抓到什么 |
| --- | --- | --- | --- |
| 1 | 提交 `e66eb6ff` | **REJECT** | P1 航向朝上帧外增量不进脏区；P2 开帧顺序、标签计数/泄漏、cos 精度 |
| 2 | 工作树（WP-12/13） | **REJECT** | 被限频跳过的增量无重试；nav 盖章只作废隐藏 C；`[mapfps]` 丢样本 |
| 3 | 工作树（+pending） | **REJECT** | 航向饥饿回归（service 用旧角刷新窗口）；track 全量重绘绕过记账 |
| 4 | 工作树（want 合并 + track 统一入口） | **ACCEPT** | 仅剩 P2：旋转失败会丢角请求 |
| 5 | 单点（成功后提交 `rot10`） | **ACCEPT** | 失败保留 want、下一拍重试；其余调用点副作用不变 |

另有 `WP-10`（`e66eb6ff`）、`WP-13` 第二轮、`WP-UI4`（REJECT → 三条已修）与
`WP-VISUAL` 的**6 轮**审查。`WP-VISUAL` 那 6 轮的原始记录**已随仓**：
[`evidence/codex-review/`](evidence/codex-review/)（`round1~6` 各一份 brief + 一份 log，
共 100 KB）。第 3 轮抓到的"航向饥饿回归"是**主对话自己没发现**的问题 —— 这正是引入
第二双眼睛的价值。

## 和另外两个 Skill 的关系

```text
single-conversation-subagent（方法）→ 定义三种角色：协调者 / 执行子 Agent / 外部只读审查者
codex-review（验收）              → 把第三种角色做成可执行流程
vela-helm-one（知识）             → 审查结论沉淀成 troubleshooting.md 的条目
```

顺带一个对大赛有利的事实：Codex 是官方《AI Coding 日志归集手册》里**可自动采集的四种工具之一**，
用它做审查，相关会话可被 `contest-snapshot --backfill` 直接补进 `logs/`。

## 怎么复核

```bash
# 任意一次改动，让 Codex 只读审一遍（不会改你的工作树）
codex exec -m gpt-6.1-sol -s read-only "审查 git diff HEAD 的改动，逐项 FIXED/PARTIAL/NOT FIXED，最后给 ACCEPT/REJECT"
```

审查记录与哈希留档格式见 `references/brief-template.md`。
