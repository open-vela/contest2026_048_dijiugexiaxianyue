# logs/ — AI Coding 日志

本目录用于存放开发过程中与 AI 工具的对话日志。

## 当前状态

✅ **已按下方第 3 条路径整理入库**（本队主力工具不在官方采集列表内，风险提示见条目 3 与文末）。
2026-11-04 对全机 AI 会话做了一次完整搜寻，把 09-20 之后缺失的会话按同一格式补齐：

| 项 | 值 |
| --- | --- |
| 目录 | `logs/jinsc123654/`（248 个 `.jsonl` + `manifest.json`，20 个日期目录） |
| 会话数 | **248** = ZCode **181**（34 主会话 + 147 子代理会话）+ Codex **67** |
| 事件行数 | **75 670**（ZCode 74 601 + Codex 1 069） |
| 体积 / 时间范围 | 约 **195 MB**；2026-09-16 ~ 2026-11-04 |
| 生成方式 | `zcode-export` / `codex-export`：从本机会话库导出，只做字段映射，除下方脱敏外不改写内容 |
| 工具 / 模型 | ZCode（`DeepSeek-V4-Flash` 31 399 条、`deepseek-v4.1-flash` 37 133 条、`GLM-5.3-Flash` 1 217 条记录）；Codex（`gpt-6.1-sol` 1 029 条、`codex-auto-review` 40 条记录） |
| 工具调用统计 | Bash 24 541 / Edit 5 736 / Read 5 607 / Write 654 / exec 652 / TodoWrite 340 / Agent 144 |

**脱敏声明**：首批 34 个文件入库前对 7 处做了掩码（App 的 OTA 访问 token、一处飞书邀请链接 token），统一替换为
`***REDACTED***`；2026-11-04 新增的 214 个文件是原样导出，**未做任何掩码**——入库前对全部新增内容做了
私钥 / Bearer token / access·refresh token / api·secret key / password / 飞书邀请链接 / 云厂商 AK / GitHub PAT /
OpenAI key 十类模式扫描，**未命中任何一项**；仅命中 `127.0.0.1`（本机回环地址）与几个会话正文里的邮箱地址，
按首批口径不视为需处理项。全部 248 个文件 `seq` 无断档，`manifest.json` 的 `event_count` 与文件行数逐一对齐。

**导出范围**：会话的 `cwd` 落在以下三处之一才纳入（含其子代理会话）——
固件树 `/home/jinsc/SDK/vela/`、参赛仓 `/home/jinsc/esp/Desktop/vela/`、
配套 App 与硬件 `.../my_work/sifli/`。与作品无关的会话（ESP32 EPD、snf 组装、个人 LLM 网关、`/tmp` 探针会话等）未纳入。

**增量与重现**：首批 34 个文件的字节内容**未做任何改动**（导出后仍在继续的那个会话，其后续内容也不会回写旧文件）；
本次仅新增 214 个文件并重建 `manifest.json`。ZCode 侧的导出字段映射已用首批 34 个文件做回归验证：
23 个逐字节一致，其余 11 个的差异全部来自「当初导出时工具调用尚无输出、数据库后来补上了结果」这一数据漂移。

开发期使用的 AI 工具：

| 工具 | 用途 | 是否在官方采集范围内 |
| --- | --- | --- |
| **ZCode**（Z.ai，模型 GLM-5.3 / GLM-5.3-Flash，1M 上下文；后期切换为 `DeepSeek-V4-Flash` / `deepseek-v4.1-flash`）+ mimosa 1.0.3 插件 | 主力开发：驱动改写、地图工具链、协议实现、文档 | ❌ 不在官方支持列表 |
| **Codex**（`gpt-6.1-sol`） | 独立只读代码审查（验收前复核），少量工具链改动 | ✅ **在官方支持列表内** |
| **Cursor** | 部分改动的编辑与复核（提交带有 `Co-authored-by: Cursor`） | ❌ 不在官方支持列表 |

官方《AI Coding 日志归集与提交手册》第七节 Q9 明确：本届可自动采集的工具为
**Claude Code / AIoT-IDE / OpenCode / Codex** 四种，其他三方工具产生的对话无法被采集。

因此本目录里 **67 个 Codex 会话属于官方可采集工具**，其 1 069 条记录是按同一格式从 `~/.codex/sessions/` 的
rollout 记录整理而来（字段映射见下节），可作为合规日志的一部分；其余 181 个 ZCode 会话仍属第 3 条路径。

## 建议的处理方式

按大赛规则，日志的**物理上传完全由参赛者自行决定**（采集器只写本机文件，从不执行 `git push`）。
按合规优先的顺序，可选：

1. **保持为空，并如实说明**——技术报告 **3.6 节**已逐项填写
   「AI Coding 代码占比 / 使用的 AI 工具 / MCP 工具使用情况 / Skills 使用与新增情况 / Token 使用总量」，
   并补充了对开发效率的提升、三个真实案例（AI 如何定位根因）与验证方法。
   评审可据此了解本作品的 AI 参与方式与规模。
2. **补充官方工具日志**——开发期若在 openvela 工作区内使用过 Claude Code / OpenCode / Codex，
   可执行 `contest-snapshot --backfill` 从 `~/.claude/projects/` 一键补回历史会话
   （官方手册 Q11 明确支持，可多次执行且不会重复）。
3. **整理非官方工具日志**（**当前采用**）——把 ZCode / Cursor 的会话按下方格式整理进本目录。
   ⚠️ **风险提示**：官方手册 Q9 说明非列表内工具「无法计入有效工时」，
   且 `validate-log.py` 会校验结构与序号一致性。**采取此路径前应先与组委会确认是否接受**，
   否则可能被判定为不合规，反而影响评审。若组委会不接受，删除 `logs/<github_login>/` 整个目录即可撤回，
   此时本目录回到第 1 条路径（技术报告 3.6 节已按原样如实声明所用工具，两种口径不冲突）。

### 目录结构（已按此结构入库）

```text
logs/
└── <github_login>/              # 一人一目录
    ├── manifest.json            # 会话清单
    └── <date>/                  # 日期 YYYY-MM-DD（UTC，取会话首条记录时间）
        └── <tool>__<sid>.jsonl  # 一个会话一个文件
```

`<tool>` 取值 `zcode` / `codex`；`<sid>` 为 ZCode 的 `sess_*` / 旧版 `session_*`，或 Codex 的线程 UUID。

`manifest.json` 字段（取自原示例）：

```json
{
  "schema_version": "1.0",
  "team_id": "contest2026_048_dijiugexiaxianyue",
  "github_login": "<你的 GitHub 用户名>",
  "generator": "zcode-export+codex-export",
  "sessions": [
    {
      "session_id": "<uuid>",
      "tool": "zcode",
      "started_at": "2026-06-01T08:00:00.000Z",
      "last_event_at": "2026-06-01T09:30:00.000Z",
      "event_count": 1234,
      "file_path": "logs/<github_login>/2026-06-01/zcode__<uuid>.jsonl",
      "collection_mode": "local-export",
      "health": "ok"
    }
  ],
  "updated_at": "2026-09-20T12:00:00.000000+00:00"
}
```

每行一个事件，字段与官方一致：

| 字段 | 内容 |
| --- | --- |
| `text` | 与 AI 的对话正文 |
| `thinking` | AI 的思考过程 |
| `tool_name` / `input` / `output` | AI 调用的工具（read / edit / bash 等） |
| `model` / `tokens_in` / `tokens_out` | 所用模型与使用统计 |
| `seq` | 会话内递增序号（用于验证一致性） |

另附 `role`（`user` / `assistant`）、`timestamp`、`session_id`、`turn_id` 四个字段便于人工核对与还原时序。
`model` 每条 assistant 记录都写；`tokens_in` / `tokens_out` 只写在每条回复的首行，避免求和时重复计数。
实际记录（截断显示）：

```json
{"seq": 4, "timestamp": "2026-09-20T07:57:43.301Z", "role": "assistant", "text": null, "thinking": null,
 "tool_name": "Bash", "input": "{\"command\": \"ls -l /dev/ttyACM* /dev/ttyUSB* 2>&1…\"}",
 "output": "crw-rw---- 1 root dialout 166, 0  9月 20 15:50 /dev/ttyACM0…",
 "model": "DeepSeek-V4-Flash", "tokens_in": null, "tokens_out": null,
 "session_id": "sess_11c4e8c4-…", "turn_id": "turn_f0924cf2-…"}
```

### Codex 会话的字段映射

Codex 原始记录是 `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`，按同一字段格式整理，映射关系：

| 官方字段 | 取自 Codex rollout |
| --- | --- |
| `text`（user） | `response_item` 中 `payload.type == "message"` 且 `role == "user"` 的 `input_text` |
| `text`（assistant） | 同上 `role == "assistant"` 的 `output_text` |
| `tool_name` / `input` / `output` | `function_call`（`arguments`）/ `custom_tool_call`（`{"code": …}`）+ 同 `call_id` 的 `*_output` |
| `model` | `event_msg.thread_settings_applied` 的 `thread_settings.model` |
| `thinking` | **恒为 `null`**：Codex 的 `reasoning` 项只存 `encrypted_content`，本机无法解密 |
| `tokens_in` / `tokens_out` | **恒为 `null`**：`token_usage_record` 的 `input_tokens` 是「该请求的上下文总量」而非增量，无法归到单条记录 |

跳过的记录类型：`developer` 角色消息（技能/多代理提示）、注入的 `# AGENTS.md` 与 `<environment_context>` 文本
（这些不是与 AI 的对话正文）、`world_state` / `compacted`（巨型上下文快照）、`event_msg` 里的重复通知。
`session_id` 取 rollout 文件名里的**线程 id**（同一会话被续写时会产生多个 rollout 文件，用后者以免互相覆盖）。

## 本机留存的原始会话规模（供参考）

- ZCode 本机会话库（`~/.zcode/cli/db/db.sqlite`）：**629 个会话**（跨多个项目，106 411 条消息 / 408 335 个 part）
- 其中本作品相关：**181 个**（34 主会话 + 147 子代理会话）→ 74 601 条事件 / 约 180 MB
- Codex 会话库（`~/.codex/sessions/`）：117 个 rollout 文件 / 约 107 MB
- 其中本作品相关：**67 个**（44 MB 原始记录 → 本目录 1 069 条事件 / 约 15 MB）
- ZCode 原始运行日志：`~/.zcode/cli/log/zcode-YYYY-MM-DD.jsonl`，现存 2026-10-29 ~ 11-04 共 7 个文件、约 254 MB
  （09-14 ~ 09-20 的 6 个文件 / 197 896 行 / 约 138 MB 已在首批统计时记录，现已轮转）
- Cursor：`~/.cursor/projects/` 下**没有** openvela 相关工程，故无 Cursor 会话可导出

技术报告 3.6 节已记录该规模作为 AI 使用强度的佐证。

> **合规提醒**：请勿修改已导出日志的**内容**（校验脚本会检测序号断档或篡改）。
> 如需撤回某次对话，应在 `git commit` 之前直接删除对应的 `.jsonl` 文件。
