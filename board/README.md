# board/ — 板卡目录

按赛方「板卡形态」约定，板级代码在此处放了一份**只读镜像**，方便评审不进 vendor 树就能看板级实现。

| 目录 | 说明 |
| --- | --- |
| [`helmone/`](helmone/) | Helm One 板级适配目录，**内容与 [`helm-one/boards/sf32lb52/helmone/`](../helm-one/boards/sf32lb52/helmone/) 一致** |
| [`contest_board/`](contest_board/) | 赛方模板板卡占位（未使用） |

> ⚠️ **两份代码以 `helm-one/` 为准**：真正被编译的是 `helm-one/`（清单 linkfile 把它挂到
> 工作区的 `vendor/HelmOne/`）。本目录只是镜像副本，**不要在这里改代码**。
>
> 另外：`helmone/` 内部的相对链接（如 `../../../../docs/…`）是按 vendor 树的位置写的，
> 在这里点可能打不开；完整文档请看
> [`helm-one/docs/`](../helm-one/docs/)（或镜像的对应位置）。

板级结构、defconfig 职责与抽换层说明见：

- [`helm-one/README.md`](../helm-one/README.md) — 板卡包总览
- [`docs/build/board_guide.md`](../docs/build/board_guide.md) — 从官方 Sifli 板克隆新板的完整流程
