# logs/ — AI Coding 日志

本目录用于存放开发过程中与 AI 工具的对话日志。

## 当前状态

✅ **已按下方第 3 条路径整理入库**（本队主力工具不在官方采集列表内，风险提示见条目 3 与文末）：

| 项 | 值 |
| --- | --- |
| 目录 | `logs/jinsc123654/`（34 个 `.jsonl` + `manifest.json`） |
| 会话数 | **34**（9 个主会话 + 25 个子代理会话） |
| 事件行数 | **29 442** |
| 体积 / 时间范围 | 约 **68 MB**；2026-09-16 ~ 2026-09-20 |
| 生成方式 | `zcode-export`：从本机 ZCode 会话库一次性导出，只做字段映射，除下方脱敏外不改写内容 |
| 工具 / 模型 | ZCode（Z.ai），模型 `DeepSeek-V4-Flash` |

**脱敏声明**：入库前对 7 处做了掩码（App 的 OTA 访问 token、一处飞书邀请链接 token），统一替换为 `***REDACTED***`；
其余内容逐字未改，`seq` 无断档。另检索私钥、口令、邮箱、IP 等未发现需处理项。

**导出范围**：工作区为固件树 `/home/jinsc/SDK/vela/openvela` 与配套 App `.../my_work/sifli/02_sw/flutter`
的会话（含其子代理会话）；与作品无关的其他项目会话未纳入。

开发期使用的 AI 工具：

| 工具 | 用途 | 是否在官方采集范围内 |
| --- | --- | --- |
| **ZCode**（Z.ai，模型 GLM-5.3 / GLM-5.3-Flash，1M 上下文；后期切换为 `DeepSeek-V4-Flash`，本目录导出的 34 个会话记录的全部是该模型）+ mimosa 1.0.3 插件 | 主力开发：驱动改写、地图工具链、协议实现、文档 | ❌ 不在官方支持列表 |
| **Cursor** | 部分改动的编辑与复核（提交带有 `Co-authored-by: Cursor`） | ❌ 不在官方支持列表 |

官方《AI Coding 日志归集与提交手册》第七节 Q9 明确：本届可自动采集的工具为
**Claude Code / AIoT-IDE / OpenCode / Codex** 四种，其他三方工具产生的对话无法被采集。

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
    └── <date>/                  # 日期 YYYY-MM-DD
        └── <tool>__<sid>.jsonl  # 一个会话一个文件
```

`manifest.json` 字段（取自原示例）：

```json
{
  "schema_version": "1.0",
  "team_id": "contest2026_048_dijiugexiaxianyue",
  "github_login": "<你的 GitHub 用户名>",
  "generator": "zcode-export | cursor-export | manual",
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

## 本机留存的原始会话规模（供参考）

- ZCode 本机会话库：**46 个会话**（跨多个项目）
- 其中本作品相关：**34 个**（9 个主会话 + 25 个子代理会话）→ 即本目录的 **29 442 条事件 / 约 68 MB**
- 原始运行日志：`~/.zcode/cli/log/zcode-YYYY-MM-DD.jsonl`，2026-09-14 ~ 09-20 共 6 个文件、**197 896 行 / 约 138 MB**

技术报告 3.6 节已记录该规模作为 AI 使用强度的佐证。

> **合规提醒**：请勿修改已导出日志的**内容**（校验脚本会检测序号断档或篡改）。
> 如需撤回某次对话，应在 `git commit` 之前直接删除对应的 `.jsonl` 文件。
