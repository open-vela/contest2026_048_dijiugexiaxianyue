# tools/ — 板级工具脚本

这里放**构建/烧录/串口/环境**的实际脚本；文档只有两篇，先看哪篇取决于你要干什么：

| 你想 | 读这篇 |
| --- | --- |
| 现场调试（插板子、看日志、不抢 tty） | [debug_workflow.md](debug_workflow.md) —— **日常口径** |
| 查某个命令的完整参数 | [vela_my_vendor_tools.md](vela_my_vendor_tools.md) —— **全量命令表** |

## 脚本清单

| 脚本 | 用途 | 备注 |
| --- | --- | --- |
| `vela_my_vendor_tools.py` | 板级主工具：`build` / `flash` / `monitor` / `clean` 等子命令 | 主入口，219 KB；命令说明见上面那篇 |
| `vela_sifli_tools.py` | 通用 SiFli 板级构建辅助（不带本板定制） | 上游风格版本 |
| `vela_emulator_tools.py` | Vela Emulator 的构建与运行辅助 | 与 QEMU 模拟器配套 |
| `serial_hub.py` | 串口租约枢纽：多终端共享一个串口，`/tmp` 只放 lease 信物 | 2026-09-22 起租约化 |
| `pty_run.py` | 在 pty 里跑命令并把输出同时写日志 | 解决 miniterm 的 Console 问题 |
| `probe_boot.py` | 复位脉冲 + 读 3 秒，区分"板子没起来"与"monitor 收回没生效" | 用前先 `monitor-ctl release` |
| `make_sd_2g.sh` / `4g` / `8g` / `16g` | 出对应容量的整盘 SD 镜像 | `-o /tmp/xxx.img` 换输出路径 |
| `vela_ubuntu_get_env.sh` | 按 SiFli 约定加载工作区环境（进目录自动 source） | 配套下面的 hook |
| `vela_ubuntu_env_hook.sh` | 进/出工作区时整体加载/还原环境（最小号 direnv） | 默认 cd 钩子只会进不会出 |
| `local_manifests/my_vendor.xml` | `repo` manifest 片段（把本板卡挂进 openvela） | 接入步骤见 [../board_guide.md](../board_guide.md) |

> ⚠️ **`vela_ubuntu_re.sh` 是作者的破坏性本机脚本**：硬编码 `cd /home/jinsc/SDK/vela/openvela`，
> 然后 `find . -maxdepth 1 ! -name .repo -exec rm -rf` 清空工作区再 `repo sync -d --force-sync`。
> 只留作记录，**不要在其他机器或别的目录跑**。

全部脚本与文档相对路径都以本目录为基准；仓库根或别的 vendor 树里的路径写法见
[vela_my_vendor_tools.md](vela_my_vendor_tools.md) 开头「本工具链不要求这棵树叫 `my_vendor`」。
