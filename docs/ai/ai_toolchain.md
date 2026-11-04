# AI 工具链：给 Agent 准备的手

> Skill 给 AI **知识**（该做什么、为什么），工具给 AI **手**（真的编出来、烧进去、读回日志）。
> 这一篇说明本项目为 AI 准备的那套命令行工具——**它们不是为了"人用着方便"写的，而是为了
> 让 Agent 能在没有人盯着终端的情况下把上板闭环跑通**。

## 为什么单独列这一项

通用 AI 编码助手在嵌入式项目上最容易卡在三件事：

1. **记不住构建顺序**——main / factory / 二级 boot / 文件系统 / 包装 / 打包，六个脚本六种参数；
2. **抄错地址**——分区偏移散在 `ptab.json` 里，手抄一次就可能烧错区；
3. **抢不到串口**——人和 Agent 都想读同一根 tty，谁先占谁赢，一抢就两边都瞎。

本项目的做法是把这三件事**固化进工具**，而不是写进提示词让 AI 自己小心。

## 工具清单

| 工具 | 规模 | 作用 | 没有它 AI 会遇到什么 |
| --- | --- | --- | --- |
| [`vela_my_vendor_tools.py`](../../helm-one/docs/tools/vela_my_vendor_tools.py) | **5 940 行** / 19 个子命令 | 构建 / 烧录 / 串口监视的**单一入口**：`build`、`build-all`、`build-boot`、`build-factory`、`build-fs`、`pack-sd-img`、`wrap`、`flash*`、`monitor`、`menuconfig`、`savedefconfig`、`clean*` | 得自己拼 6 个脚本的调用顺序与参数，且每次都要重新确认路径 |
| [`build_board.py`](../../helm-one/build_board.py) | 49 行 | 薄封装入口（`vendor/<树>/build_board.py`），与 `vendor/sifli` 同级习惯一致 | 树改名 / 多树并存时容易编错树 |
| [`serial_hub.py`](../../helm-one/docs/tools/serial_hub.py) | 1 012 行 | **串口租约 + 多路复用**：一个进程持有 TTY，其他人挂 abstract 端点 | 人和 AI 抢同一根串口，烧录时把人的监视踢掉 |
| [`pty_run.py`](../../helm-one/docs/tools/pty_run.py) | 125 行 | 给子进程分配 pty 再执行，输出按原样写日志 | `miniterm` 没有真 tty 会直接 `termios.error(25)`，**Agent 的后台任务根本起不了 monitor** |
| [`probe_boot.py`](../../helm-one/docs/tools/probe_boot.py) | 32 行 | 开串口 → 复位脉冲 → 读 3 秒 | 分不清"板子没起来"和"监视器的脉冲没生效" |

配套文档：[`helm-one/docs/tools/vela_my_vendor_tools.md`](../../helm-one/docs/tools/vela_my_vendor_tools.md)（全量命令）、
[`debug_workflow.md`](../../helm-one/docs/tools/debug_workflow.md)（现场口径，尤其"不要抢 tty"）。

## 五条"为 AI 而写"的设计

| 设计 | 对应 Agent 的失败模式 |
| --- | --- |
| **单一入口 + 幂等编排**：`build-all` 一次跑完 main → factory → 2SFBL → wrap → FS，不满足前置就自己补 | 顺序记混、漏一步、重复做 |
| **地址不落人手**：烧录地址由 `ptab.json` + `sftool_param.json` **实时计算**，并落一份 `flasher_args.json` 快照 | 手抄分区偏移 → 烧错区 |
| **串口租约**：`release`（让出 TTY、`state=yielded`）→ sftool 独占 → `acquire`+`pulse` | 烧录与监视互相踢，两边都没日志 |
| **自解释错误**：缺件时直接给下一步该跑的命令（"未找到 fs_root.bin，请先 …"） | Agent 拿到一句 traceback 就开始瞎试 |
| **可被非交互进程驱动**：pty + 日志文件 + 退出码，不依赖交互式输入 | 后台任务 / CI / Agent 会话里跑不起来 |

最后一条最容易被忽略：`debug_workflow.md` 里那条"**不要抢 tty**"的硬约束，落地形态正是
`serial_hub.py` 的租约——**约束写在文档里会忘，做进工具里不会**。

## 与两个 Skill 的关系

```text
知识层  vela-helm-one              该做什么：分区表、阈值、故障处置
方法层  single-conversation-subagent  怎么组织：任务边界、证据、验收
执行层  AI 工具链（本篇）           怎么落地：编、烧、看、解码
```

三层是配套的：方法层要求"证据可复核"，执行层就得能稳定产出证据；知识层里 23 节
`troubleshooting.md` 的每一条，都是先用这套工具取证、再写进去的。

## 怎么复核

```bash
# 在 openvela 工作区根目录，任意一条都能自证
python3 vendor/HelmOne/build_board.py help            # 19 个子命令
python3 vendor/HelmOne/build_board.py build           # 编出 nuttx.flash.bin
python3 vendor/HelmOne/build_board.py flash monitor   # 烧录 + 监视（走租约）
```

`flash` 的地址来源可直接对照快照：

```bash
cat cmake_out/helmone_nsh/flasher_args.json     # build 后生成，含每个镜像的 sbus 地址
```

## 与大赛报告 3.6 的对应

报告 3.6 节「Skills 使用与新增情况」记的是 `vela-helm-one` 与
`single-conversation-subagent`；同节「MCP 工具使用情况」旁列的
`serial_hub.py` / `pty_run.py` / `probe_boot.py` 属于**本篇的工具**，
`vela_my_vendor_tools.py`（及其封装 `build_board.py`）是这条工具链的主入口。
