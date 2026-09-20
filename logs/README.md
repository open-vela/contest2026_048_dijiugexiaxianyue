# logs/ — AI Coding 日志

本目录用于存放开发过程中与 AI 工具的对话日志。

## 当前状态

⚠️ **本队使用了大赛采集器暂不支持的工具，因此本目录暂未自动归集到日志。**

开发期使用的 AI 工具：

| 工具 | 用途 | 是否在官方采集范围内 |
| --- | --- | --- |
| **ZCode**（Z.ai，模型 GLM-5.3 / GLM-5.3-Flash，1M 上下文）+ mimosa 1.0.3 插件 | 主力开发：驱动改写、地图工具链、协议实现、文档 | ❌ 不在官方支持列表 |
| **Cursor** | 部分改动的编辑与复核（提交带有 `Co-authored-by: Cursor`） | ❌ 不在官方支持列表 |

官方《AI Coding 日志归集与提交手册》第七节 Q9 明确：本届可自动采集的工具为
**Claude Code / AIoT-IDE / OpenCode / Codex** 四种，其他三方工具产生的对话无法被采集。

## 建议的处理方式

按大赛规则，日志的**物理上传完全由参赛者自行决定**（采集器只写本机文件，从不执行 `git push`）。
按合规优先的顺序，可选：

1. **保持为空，并如实说明**（当前采用）——技术报告 **3.6 节**已逐项填写
   「AI Coding 代码占比 / 使用的 AI 工具 / MCP 工具使用情况 / Skills 使用与新增情况 / Token 使用总量」，
   并补充了对开发效率的提升、三个真实案例（AI 如何定位根因）与验证方法。
   评审可据此了解本作品的 AI 参与方式与规模。
2. **补充官方工具日志**——开发期若在 openvela 工作区内使用过 Claude Code / OpenCode / Codex，
   可执行 `contest-snapshot --backfill` 从 `~/.claude/projects/` 一键补回历史会话
   （官方手册 Q11 明确支持，可多次执行且不会重复）。
3. **整理非官方工具日志**——把 ZCode / Cursor 的会话按下方格式整理进本目录。
   ⚠️ **风险提示**：官方手册 Q9 说明非列表内工具「无法计入有效工时」，
   且 `validate-log.py` 会校验结构与序号一致性。**采取此路径前应先与组委会确认是否接受**，
   否则可能被判定为不合规，反而影响评审。
   技术报告 3.6 节已按原样如实声明所用工具，不会造成口径冲突。

### 若选择第 3 条路径，目录结构如下

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
      "collection_mode": "cli",
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

## 本机留存的原始会话规模（供参考）

- 会话日志：**6 个 JSONL、191 579 行、约 135 MB**
- 会话数：**41 个**（16 个主会话 + 25 个子代理）

技术报告 3.6 节已记录该规模作为 AI 使用强度的佐证。

> **合规提醒**：请勿修改已导出日志的**内容**（校验脚本会检测序号断档或篡改）。
> 如需撤回某次对话，应在 `git commit` 之前直接删除对应的 `.jsonl` 文件。
