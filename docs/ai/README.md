# 自定义 Skill：共 3 个（知识 / 方法 / 验收）+ 1 条 AI 工具链

大赛要求「**至少沉淀 1 个 Skill（硬性要求）**；AI 硬件产品创新赛道须说明运行时 Skill
（`/data/agent/skills/`）的定义与触发场景」。本目录即为该要求的交付物，共沉淀 **3 个** Skill
与 **1 条 AI 工具链**：管**产品知识**（做什么）、**开发方法**（怎么组织）、**独立验收**（谁来判），
外加让 Agent 能真正上板执行的一层工具。

## 文件

```text
ai/
├── README.md                        # 本文件
├── vela-helm-one.md                 # ① 运行时 Skill 本体（单文件）
├── vela-helm-one/                   # ② PC 侧深读知识库（同源，更细）
│   ├── README.md
│   └── references/
│       ├── board-facts.md           #    权威值：分区/引脚/时钟/阈值/地图参数/协议口径
│       ├── constraints.md           #    强制约束与禁止事项
│       ├── troubleshooting.md       #    故障恢复手册（现象 → 根因 → 处置 → 源）
│       └── commands.md              #    构建/烧录/监视/机上探针/地图工具链
├── ai_native_skill.md               # ③ 沉淀过程与维护约定
├── single-conversation-subagent.md  # ④ Skill ②（协作方法论，独立仓开源）
├── codex-review.md                  # ⑤ Skill ③（Codex 只读独立审查）
├── evidence/codex-review/           #    ③ 的原始记录：WP-VISUAL 6 轮 brief + log
└── ai_toolchain.md                  # ⑥ AI 工具链：给 Agent 准备的手（构建/烧录/串口）
```

## 四层分工

| 层 | 是什么 | 在哪 |
| --- | --- | --- |
| **知识** | 该做什么：板级权威值、强制约束、故障处置 | [`vela-helm-one`](vela-helm-one.md) |
| **方法** | 怎么组织：任务边界、证据、验收 | [`single-conversation-subagent`](single-conversation-subagent.md) |
| **验收** | 谁来判：另一模型只读复核，结论 `ACCEPT/REJECT` | [`codex-review`](codex-review.md) |
| **执行** | 怎么落地：编、烧、监视、读崩溃 | [`AI 工具链`](ai_toolchain.md)（`vela_my_vendor_tools.py` 等 5 个工具） |

## 三个 Skill 的分工

| Skill | 沉淀的是什么 | 形态 | 出处 |
| --- | --- | --- | --- |
| [`vela-helm-one`](vela-helm-one.md) | **产品知识**：板级权威值、强制约束、故障处置 | 运行时单文件 + 4 篇 references | 本仓 `docs/ai/` |
| [`single-conversation-subagent`](single-conversation-subagent.md) | **开发方法**：单对话两级协作、任务边界、验收闭环 | 用户级 Skill（`SKILL.md` + `protocol.md`） | 独立开源仓 <https://github.com/jinsc123654/single-conversation-subagent-skill>（v1.3.4） |
| [`codex-review`](codex-review.md) | **独立验收**：只读审查、基线冻结、逐项判定 | 用户级 Skill（`SKILL.md` + `brief-template.md`） | 本仓成果包（v1.2.0）；本作品 5 轮审查记录随仓 |

一句话：**方法论 Skill 沉淀知识，知识 Skill 沉淀结论**——前者保证过程不失控，后者保证
下次不重踩。


## 安装（运行时）

```bash
mkdir -p /data/agent/skills
cp docs/ai/vela-helm-one.md /data/agent/skills/vela-helm-one.md
```

## 为什么是这个格式

openvela 的 `packages/ai_agent` 对 Skill 的加载方式是固定的（见 `src/tools/skill_loader.c`）：

1. 启动时扫描 `/data/agent/skills/` 下的**扁平 `.md` 文件**（子目录会被直接忽略）；
2. 对每个文件取**首行 `# 标题`**与**其后到第一个空行/`##` 之间的描述**，
   拼成系统提示里的一行常驻索引：
   `- **<标题>**: <描述> (read with: read_file /data/agent/skills/<name>.md)`；
3. 助手命中某个 Skill 时用 `read_file` 读取正文（`When to use` / `How to use` / `Example`）；
4. 目录的文件名、大小与 mtime 参与哈希，变动即热重载并刷新工具注册表。

由此得到两条硬约束：**必须是扁平单文件**；**描述要短**（它常驻每次对话的系统提示）。
所以运行时那份写得紧凑自包含，而把更厚的、带唯一源出处的深读材料放在 `vela-helm-one/` 里。

## 定义与触发场景

| 触发场景 | 例子 | 加载后先读 |
| --- | --- | --- |
| 一、板级 / 驱动 | 新增或修改 LCD / SD / GNSS / 蓝牙 / USB / 看门狗驱动 | `vela-helm-one/references/board-facts.md` |
| 二、地图工程 | 重新生成全国分片、改吸附半径与折线上限 | `board-facts.md` §5 + 主仓 `docs/map/`、`docs/osm/` |
| 三、协议与 App | 加 Companion 特征、改通知/星历/OTA | `constraints.md` §1 协议行 |
| 四、稳定性排查 | 开机 15 s 复位、SD 整卷僵死、GNSS 静默、蓝牙断连 | `vela-helm-one/references/troubleshooting.md` |
| 五、构建 / 上板 | 只烧固件、AI 读串口 | `vela-helm-one/references/commands.md` |

## 内容组成

- **权威值速查**：卡分区偏移、引脚、时钟上限、界面与阈值口径、地图网格与双端协议要点
- **强制约束**：改阈值先改 `system_states.md`、AI Coding 日志不得手工删改、协议版本必须双端同步、
  不改上游 `nuttx/` 而走 `vela_override`、烧录调试必须走指定工具链
- **故障速查表**：现象 → 根因 → 处置，覆盖 SD `CMD_BUSY` 复位、DATA 态禁 CMD8、
  `lfs_alloc` 首次分配导致开机复位、静止假速度、蓝牙双角色 `-EACCES`、看门狗误触发判定等
- **机上探针清单**：`ptab` / `bootinfo` / `df -h` / `free` / `ps` 等设备端工具用法

详见 [`ai_native_skill.md`](ai_native_skill.md)。
