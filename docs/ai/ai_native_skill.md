# vela-helm-one：本项目的 AI 知识沉淀

本文说明 Helm One 项目沉淀的自定义 Skill —— `vela-helm-one` —— 的**来历、结构、安装与维护约定**。
它是「把散落在仓库文档里的板级事实、强制约束与故障恢复手册固化成 AI 可直接调用的能力」这一步的产物。

- 运行时 Skill：`vendor/my_vendor/ai/skills/vela-helm-one.md` → 拷成 `/data/agent/skills/vela-helm-one.md`
- PC 侧深读知识库：`vendor/my_vendor/ai/skills/vela-helm-one/`（`README.md` + `references/`）
- 适用对象：在本仓（openvela + SF32LB52 骑行整机）上工作的 AI 助手与人类开发者

---

## 1. 为什么要沉淀

项目的知识本来就存在，但形态对 AI **不友好**：

| 现状 | 后果 |
|---|---|
| 事实散落在 30+ 份 `docs/*.md` 里 | AI 每次都要重新检索、拼装，容易漏 |
| 阈值、上限、偏移同时出现在代码与文档 | AI 凭常识写数字，与既有实现冲突 |
| 坑的结论只在某次调试记录里 | 同一个坑被重复踩（SD 假死、静止假速度、蓝牙时序） |
| 「不许做什么」是隐性的 | AI 顺手改上游 `nuttx/`、手工改生成产物 |

Skill 把这三类内容**前置**到对话开始处：读一次 Skill（标题 + 描述常驻索引、正文按需读取），就知道「去哪查、什么不许做、出事怎么办」，而不是在几十轮探索之后才知道。

## 2. 结构

```
vendor/my_vendor/ai/skills/
├── vela-helm-one.md              # ① 运行时 Skill（单文件，openvela ai_agent 规范）
└── vela-helm-one/                # ② PC 侧深读知识库
    ├── README.md                 #    入口：触发场景 + 工作方式 + 目录
    └── references/
        ├── board-facts.md        #    权威值：分区/引脚/时钟/阈值/地图参数/协议口径
        ├── constraints.md        #    强制约束与禁止事项
        ├── troubleshooting.md    #    故障恢复手册（现象 → 根因 → 处置 → 源）
        └── commands.md           #    构建/烧录/监视/机上探针/地图工具链
```

**为什么是两份**：运行时 Skill 必须符合 openvela `packages/ai_agent` 的加载约定 ——
它只扫描 `/data/agent/skills/` 下的**扁平 `.md` 文件**（子目录会被忽略），
并把每个文件的**首行标题 + 后面一段描述**拼进系统提示作为常驻索引，正文再由助手
`read_file` 按需读取；文件名、大小与 mtime 参与哈希，改动后热重载、无需重启。
所以运行时那份要**短而自包含**（索引常驻上下文，不能塞满）；而 PC 侧那份可以**厚**
（带唯一源出处与完整配方），供人在仓库里深查。两者同源，冲突时以仓库文档为准。

设计取舍（PC 侧知识库）：

- **运行时薄、知识库厚。** 运行时那份必须短（描述常驻每次对话的系统提示）且为扁平单文件；深读内容放 PC 侧知识库，按需读 `references/`。
- **只写结论，不复制细节。** 每节都标注「唯一源」文档；Skill 与文档冲突时**以文档为准**并回头修 Skill，避免出现第二份真相。
- **可判定。** `constraints.md` 的每条都能在 review 时直接判「违反 / 未违反」。

## 3. 触发场景

| 场景 | 例子 | 加载后先读 |
|---|---|---|
| 一、板级 / 驱动 | 新增或修改 LCD / SD / GNSS / 蓝牙 / USB / 看门狗驱动 | `references/board-facts.md` |
| 二、地图工程 | 重新生成全国分片、改吸附半径与折线上限 | `board-facts.md` §5 + `docs/map/`、`docs/osm/` |
| 三、协议与 App | 加 Companion 特征、改通知/星历/OTA | `constraints.md` §1 协议行 |
| 四、稳定性排查 | 开机 15 s 复位、SD 整卷僵死、GNSS 静默、蓝牙断连 | `references/troubleshooting.md` |
| 五、构建 / 上板 | 只烧固件、AI 读串口 | `references/commands.md` |

## 4. 安装与加载

```bash
# ① 运行时 Skill：拷成扁平文件即可（openvela ai_agent 只认 .md，子目录会被忽略）
mkdir -p /data/agent/skills
cp vendor/my_vendor/ai/skills/vela-helm-one.md /data/agent/skills/vela-helm-one.md

# ② PC 侧知识库：留在仓库里，供人或 PC 上的 AI 查阅
```

加载链路（`packages/ai_agent/src/tools/skill_loader.c`）：

1. 启动时 `mkdir /data/agent/skills/`，并在缺失时写入内置 Skill；
2. 扫描该目录下的扁平 `.md`（跳过隐藏文件与子目录），对每个文件取**首行 `# 标题`**与
   **其后到第一个空行/`##` 之间的描述**，拼成系统提示里的一行索引：
   `- **<标题>**: <描述> (read with: read_file /data/agent/skills/<name>.md)`；
3. 助手命中某个 Skill 时用 `read_file` 读取正文（`When to use` / `How to use` / `Example`）；
4. 目录的文件名、大小与 mtime 参与哈希，变动即热重载并刷新工具注册表。

由此得到两条硬约束：**运行时 Skill 必须是扁平单文件**；**描述要短**（它常驻每次对话的
系统提示）。此外，也可以直接对助手说「把这次的处置写成一个 Skill」，由内置的
`skill-creator` 在运行时生成同格式的文件 —— 这也是本项目沉淀该 Skill 的方式之一。

仓库里同时保留 `.mimosa/`（钩子式代码审查）与 `docs/tools/serial_hub.py`、`pty_run.py`、
`probe_boot.py`（AI 上板/读串口），三者与 Skill 配套使用。

## 5. 维护约定

| 什么时候 | 改哪里 |
|---|---|
| 改阈值 / 上限 / 文案口径 | 先 `docs/system_states.md`，再代码，最后 `board-facts.md` §4 |
| 改分区布局 | 先 `ptab.sdmmc.json`，再两份 `ptab_sdmmc.h` + 重新生成表，最后 `board-facts.md` §2 |
| 改引脚 / 时钟 | `docs/gpio_pinmux_guide.md` + `bsp_pinmux.c`，最后 `board-facts.md` §3 |
| 改地图网格 / 打包规则 | 固件 `vmap_grid.c` 与 `docs/osm/build_vmap.py` 必须同源，最后 `board-facts.md` §5 |
| 改双端协议 | `companion_proto.h` ↔ `companion_proto.dart` 同版本同步，最后 `board-facts.md` §6 |
| 解决一个非平凡问题 | 写进对应 `docs/*.md`，并在 `troubleshooting.md` §E 索引表加一行 |
| 发现文档之间互相矛盾 | **不要静默选一个**：在 Skill 中标注冲突与仲裁结论（例：地图挂载点 `/mnt/fat` vs `/mnt/lfs`，以 `sd_partition.md` 为准），并提 issue 统一 |

**纪律**：Skill 里出现的每个数字都必须能追溯到代码常量、文档小节或实测日志。不允许「大概是这个量级」。

## 6. 沉淀的三个真实案例（Skill 的直接来源）

这三条现在都在 `troubleshooting.md` 里，各自从「几小时现场调试」压缩为「一条目 + 一次复位」。

1. **SD 控制器死锁（`CMD_BUSY`）** —— 现象是整卡假死、1 Hz 反复重识别；读控制器状态与参考实现后定位，方案是模块级 RCC 复位。→ A1
2. **LittleFS 挂载 0.474 s** —— 统计挂载期实际读取扇区数后发现缓存/预读参数过大。方法本身（先量再调）也写进了条目。→ A4
3. **蓝牙双角色冲突** —— 读 zblue 源码定位到 LCPU 在已有连接时禁止设置随机地址并返回 `-EACCES`，据此产出上游补丁，实现「连手机的同时扫描传感器」。→ C1

## 7. 与大赛报告的关系

本文件与 Skill 源对应作品提交报告中 **3.4 节「沉淀的自定义 Skill：vela-helm-one」** 的描述：报告讲**是什么、为什么**，本目录给出**可运行的完整定义**。评审可直接 `cp -r` 到 `/data/agent/skills/` 验证。
