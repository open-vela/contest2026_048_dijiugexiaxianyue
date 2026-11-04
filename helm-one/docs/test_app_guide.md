# test_app 启动方式说明（my_vendor）

本文说明板级演示应用 **`test_app`** 的启动路径：**各自优劣**、**共同注册方式**、以及每种方式 **如何注册、如何被调用**。与 `test`（`test nand` / `test led`）无关。

更完整的 NuttX 上电顺序、ROMFS、Shell/rcS、`board_late_initialize` 见 [nuttx_boot_flow.md](nuttx_boot_flow.md)。

**当前仓库默认**：**方式 D — board bringup 异步**（保留 `nsh_main`，在 `sf32lb52_devkit_lcd_bringup()` 末尾 `task_spawn(test_app_main)`，`launch=board`；`rcS` 不自启 test_app）。

---

## 目录

1. [test_app 是什么](#1-test_app-是什么)
2. [四种启动方式与优劣](#2-四种启动方式与优劣)
3. [共同部分：应用如何「注册」进系统](#3-共同部分应用如何注册进系统)
4. [方式 D：board bringup 异步（当前默认）](#4-方式-dboard-bringup-异步当前默认)
5. [方式 A：rcS 自启（保留 Shell）](#5-方式-arcs-自启保留-shell)
6. [方式 B：代码启动（替换 nsh_main）](#6-方式-b代码启动替换-nsh_main)
7. [方式 C：NSH 手动启动](#7-方式-cnsh-手动启动)
8. [涉及文件一览](#8-涉及文件一览)
9. [从零添加 test_app（检查清单）](#9-从零添加-test_app检查清单)
10. [CMake / Kconfig 速查](#10-cmake--kconfig-速查)
11. [编译配置同步与验证](#11-编译配置同步与验证)
12. [常见问题](#12-常见问题)

---

## 1. test_app 是什么

| 项目 | 说明 |
|------|------|
| 类型 | NuttX **builtin 应用**（编进固件，不是从文件系统 exec 的 ELF） |
| NSH 命令名 | `test_app`（与 `nuttx_add_application(NAME test_app)` 一致） |
| 实际入口 | `test_app_main`（构建时把 `main` 重命名） |
| 运行行为 | 打印 `started` + 10 次 `tick`（间隔 2 s）→ `done, exiting` 后任务结束 |
| 参数 | `board` / `rcs` / `entry` / `manual`，仅用于日志区分启动路径 |

四种方式用的是 **同一份固件、同一个 `test_app_main`**，差别只在 **谁、在何时、用什么参数** 创建这个任务。

---

## 2. 四种启动方式与优劣

| | **D. board 异步（默认）** | **A. rcS + NSH** | **B. 代码启动** | **C. NSH 手动** |
|--|---------------------------|------------------|-----------------|-----------------|
| **一句话** | bringup 末尾 `task_spawn`，仍起 `nsh_main` | rcS 里 `test_app rcs &` | `INIT_ENTRYPOINT=test_app_main` | `nsh>` 手敲命令 |
| **INIT_ENTRYPOINT** | `nsh_main` | `nsh_main` | `test_app_main` | `nsh_main`（同 D/A） |
| **串口** | `launch=board` + `rcS start` + `nsh>` | `launch=rcs` + `nsh>` | 仅 test_app，`launch=entry`，无 `nsh>` | `launch=manual` |
| **与 rcS** | rcS **不写** test_app | rcS 写 `test_app rcs &` | 通常无 NSH | 不依赖 rcS |

### 优劣简表

**D. board bringup 异步（当前默认）**

| 优点 | 缺点 |
|------|------|
| **保留 `nsh>`**，调试方便 | 需改 C 代码（`sifli_ap.c`），不如 rcS 改脚本直观 |
| 在板级资源就绪后立刻起业务，**不依赖 rc 脚本** | 与 rcS 同时写会 **起两个** test_app |
| 与 `lcd_async_init` 同类，适合产品后台任务 | `task_spawn` 的 argv 规则易踩坑（见第 4.4 节） |

**A. rcS + NSH**

| 优点 | 缺点 |
|------|------|
| 改 `rcS` 即可调自启，无需改 bringup C 代码 | 依赖 `CONFIG_ETC_ROMFS`、rcS 内容正确 |
| 保留 `nsh>` | 启动略晚于 board 方式（等 NSH 解析 rcS） |

**B. 代码启动**

| 优点 | 缺点 |
|------|------|
| 路径最短，适合无 shell 产品 | 无 `nsh>` |
| 不依赖 NSH/rcS | 占用唯一 `INIT_ENTRYPOINT` 槽位 |

**C. NSH 手动**

| 优点 | 缺点 |
|------|------|
| 单次实验可控 | 不能作开机策略 |

### 如何选型

- **当前工程默认**：**D**（board 异步 + NSH）
- **只想改脚本、不动 C**：改用 **A**，并关掉 D（见第 5 节）
- **最终产品、不要 shell**：**B**
- **临时试一次**：**C**（在 D 或 A 下）

**勿同时启用 D 与 A**（board 里 spawn + rcS 里 `test_app rcs &`）。

---

## 3. 共同部分：应用如何「注册」进系统

四种方式的 **注册（编译期）完全相同**；差别只在 **运行期谁调用 `test_app_main`**。

### 3.1 注册链路（编译 → 链接）

```text
chips/sf32lb52/Kconfig
  MYVENDOR_TEST_APP / _STACKSIZE / _PRIORITY
           │
           ▼
configs/nsh/defconfig
  CONFIG_MYVENDOR_TEST_APP=y
           │
           ▼
boards/.../nsh/test_app/CMakeLists.txt
  nuttx_add_application(NAME test_app ...)
           │
           ├─► apps_test_app → test_app_main
           └─► builtin_list.h: { "test_app", ..., test_app_main }
```

### 3.2 必须满足的条件

| 层级 | 配置 / 文件 |
|------|-------------|
| 开关 | `CONFIG_MYVENDOR_TEST_APP=y` |
| Builtin | `CONFIG_BUILTIN=y` |
| 板级 CMake | `nsh/CMakeLists.txt` → `add_subdirectory(test_app)` |
| board 启动（方式 D） | `bsp/sifli_ap.c` 中 `board_start_test_app_async()` |

方式 D/B 用 **`task_spawn` 明文调 `test_app_main`**，不查 builtin 表；但仍需 **`nuttx_add_application`** 把符号链进固件。方式 A/C 走 **`nsh_builtin`** 查表。

---

## 4. 方式 D：board bringup 异步（当前默认）

在 **板级资源初始化完成之后**、**`nsh_main` 被 spawn 之前**，由 `sf32lb52_devkit_lcd_bringup()` 末尾异步拉起 `test_app`。与 LCD 的 `lcd_async_init` 任务同类。

### 4.1 涉及代码与配置

| 项目 | 值 / 位置 |
|------|-----------|
| `CONFIG_INIT_ENTRYPOINT` | `"nsh_main"`（不变） |
| `CONFIG_SYSTEM_NSH` | `y` |
| `CONFIG_BOARD_LATE_INITIALIZE` | `y` |
| 启动实现 | `bsp/sifli_ap.c` → `board_start_test_app_async()` |
| 调用点 | `sf32lb52_devkit_lcd_bringup()` 末尾、`return` 之前 |
| `rcS` | 仅 `echo rcS start`（**不要** `test_app rcs &`） |

`defconfig` 注释示例：

```kconfig
CONFIG_INIT_ENTRYPOINT="nsh_main"
CONFIG_SYSTEM_NSH=y
CONFIG_BOARD_LATE_INITIALIZE=y
CONFIG_MYVENDOR_TEST_APP=y
```

### 4.2 调用链（运行期）

```text
nx_bringup()
  → AppBringUp 内核线程
      → nx_start_application()
            → nx_romfsetc()
            → board_late_initialize()
                  → sf32lb52_devkit_lcd_bringup()
                        ├─ 驱动、挂载、NAND、LCD 异步任务等
                        └─ board_start_test_app_async()
                              → task_spawn("test_app", test_app_main,
                                           argv 经内核整理后 argv[1]="board")
            → task_spawn("nsh_main")
                  → nsh_initscript() → rcS（仅 echo）
                  → nsh>
```

| 步骤 | 说明 |
|------|------|
| `board_late_initialize` | NuttX 在起首个应用任务前调用；调度器 **已就绪** |
| `task_spawn` | **明文**函数指针，非 `nsh_builtin` |
| `nsh_main` | 在 bringup 返回后 spawn；与 test_app **并行** |

### 4.3 核心代码（`sifli_ap.c`）

```c
static void board_start_test_app_async(void)
{
  posix_spawnattr_t attr;
  FAR char *argv[] = { "board", NULL };   /* 只传真实参数，见 4.4 */
  ...
  task_spawn("test_app", test_app_main, NULL, &attr, argv, NULL);
}

/* sf32lb52_devkit_lcd_bringup() 末尾 */
#ifdef CONFIG_MYVENDOR_TEST_APP
  board_start_test_app_async();
#endif
```

### 4.4 `task_spawn` 与 argv（重要）

NuttX `task_spawn(name, ...)` 会把 **`name` 写入 `argv[0]`**，再把调用者传入的数组依次拷到 `argv[1]`、`argv[2]`…

| 错误写法 | 实际 argv | `test_app` 看到的 `argv[1]` | 结果 |
|----------|-----------|------------------------------|------|
| `{ "test_app", "board", NULL }` | `[0]=test_app [1]=test_app [2]=board` | `"test_app"` | **Usage**，校验失败 |
| **正确** `{ "board", NULL }` | `[0]=test_app [1]=board` | `"board"` | `launch=board` |

`test_app` 里 `resolve_launch_mode()` 使用 **`argv[1]`**（与 NSH/rcS 一致）。

### 4.5 串口预期

```text
INFO: test_app async started pid=...
test_app: started pid=... tid=... launch=board
test_app: tick=0/10 ...
...
rc.sysinit start
rcS start
nsh>
```

### 4.6 关闭 board 自启、改回 rcS

1. 注释或删除 `sifli_ap.c` 里 `board_start_test_app_async()` 的调用。
2. 在 `bsp/etc/init.d/rcS` 增加：`test_app rcs &`
3. 全量重编、烧录。

### 4.7 优劣

见 [第 2 节](#2-四种启动方式与优劣) 表 D。

---

## 5. 方式 A：rcS 自启（保留 Shell）

与方式 D **二选一**。开发时若更习惯改脚本，用本方式。

### 5.1 要改哪些配置

`configs/nsh/defconfig`：`INIT_ENTRYPOINT="nsh_main"`，`CONFIG_SYSTEM_NSH=y`。

`bsp/etc/init.d/rcS`（**勿用行首 `#`**）：

```text
echo rcS start
test_app rcs &
```

并 **关闭** `sifli_ap.c` 中的 `board_start_test_app_async()`。

### 5.2 调用链

```text
task_spawn("nsh_main")
  → nsh_initscript() 读 rcS
  → nsh_builtin("test_app", ...) → task_spawn(test_app_main)
```

`argv[1]` 为 **`rcs`**。

### 5.3 串口预期

```text
rcS start
test_app: started ... launch=rcs
nsh>
```

---

## 6. 方式 B：代码启动（替换 nsh_main）

`CONFIG_INIT_ENTRYPOINT="test_app_main"`，`# CONFIG_SYSTEM_NSH is not set`。无 `nsh>`。

调用链：`nx_start_application()` → `task_spawn(test_app_main)`，无参时 `launch=entry`。

详见前文逻辑；与 board 方式不同：占用 **唯一** 开机入口，不再起 NSH。

---

## 7. 方式 C：NSH 手动启动

前提：`nsh_main` + `CONFIG_SYSTEM_NSH`（可与 D 或 A 并存）。

```text
test_app manual
test_app manual &
```

`launch=manual`。

---

## 8. 涉及文件一览

路径相对于 `vendor/HelmOne/boards/sf32lb52/helmone/`。

| 文件 | 角色 |
|------|------|
| `nsh/test_app/test_app.c` | 应用逻辑；`board` / `rcs` / `entry` / `manual` |
| `nsh/test_app/CMakeLists.txt` | `nuttx_add_application` |
| `bsp/sifli_ap.c` | **方式 D**：`board_start_test_app_async()`、`sf32lb52_devkit_lcd_bringup()` |
| `configs/nsh/defconfig` | `INIT_ENTRYPOINT`、`SYSTEM_NSH`、`MYVENDOR_TEST_APP` |
| `bsp/etc/init.d/rcS` | **方式 A**：`test_app rcs &`；**方式 D**：仅 echo |
| `bsp/etc/init.d/rc.sysinit` | 可选，早于 rcS |
| `../../chips/sf32lb52/Kconfig` | `MYVENDOR_TEST_APP` 开关 |

---

## 9. 从零添加 test_app（检查清单）

1. `nsh/test_app/` + `nuttx_add_application` + `nsh/CMakeLists.txt` 里 `add_subdirectory(test_app)`。
2. Kconfig：`MYVENDOR_TEST_APP`。
3. `defconfig`：`CONFIG_MYVENDOR_TEST_APP=y`。
4. 选启动方式：**D**（`sifli_ap.c` + rcS 不写 test_app）或 **A**（rcS）或 **B**（`INIT_ENTRYPOINT`）。
5. 编译烧录；`help` 无命令见第 11 节。

---

## 10. CMake / Kconfig 速查

```cmake
if(CONFIG_MYVENDOR_TEST_APP)
  nuttx_add_application(
    NAME test_app
    STACKSIZE ${CONFIG_MYVENDOR_TEST_APP_STACKSIZE}
    PRIORITY ${CONFIG_MYVENDOR_TEST_APP_PRIORITY}
    SRCS test_app.c)
endif()
```

| 启动方式 | 关键配置 |
|----------|----------|
| **D board（默认）** | `nsh_main` + `MYVENDOR_TEST_APP` + `sifli_ap.c` 里 spawn；rcS 无 test_app |
| A rcS | `nsh_main` + rcS: `test_app rcs &`；关掉 board spawn |
| B 代码 | `INIT_ENTRYPOINT=test_app_main`，关 NSH |
| C 手动 | 同 D/A，串口 `test_app manual` |

---

## 11. 编译配置同步与验证

```bash
cmake --build cmake_out/helmone_nsh --target resetconfig
./build.sh vendor/HelmOne/boards/sf32lb52/helmone/configs/nsh/ --cmake -j$(nproc)
```

| 方式 | 检查 |
|------|------|
| **D board** | `launch=board` + `rcS start` + `nsh>`；无 Usage |
| A rcS | `launch=rcs` + `nsh>` |
| B | `launch=entry`，无 `nsh>` |
| C | `nsh> test_app manual` |

若 `config.h` 报 `unterminated #ifndef`：`resetconfig` 后先 `cd cmake_out/helmone_nsh && cmake .` 再编，避免并行编打断 `config.h` 生成。

---

## 12. 常见问题

| 问题 | 原因 / 处理 |
|------|-------------|
| 打印 **Usage**、`launch` 不对 | `task_spawn` 多传了 `"test_app"` 作 argv[0]；应只传 `{ "board", NULL }`，见 [4.4](#44-task_spawn-与-argv重要) |
| 两个 test_app | D 与 A 同时启用；只保留一种 |
| `help` 无 `test_app` | `MYVENDOR_TEST_APP` 未开或未 `resetconfig` |
| rcS `#` 编译失败 | `RCSRCS` 预处理；用 `//` 或改 CMake |
| board 与 **INIT_ENTRYPOINT** 关系 | board 方式 **不改** `INIT_ENTRYPOINT`，仍 `nsh_main`；仅在 bringup 里额外 spawn |

---

## 相关文档

- [nuttx_boot_flow.md](nuttx_boot_flow.md) — 上电时间线、**§3.8 board bringup 拉起 test_app**、`board_late_initialize`、ROMFS、rcS
- [board_guide.md](board_guide.md) — 板级编译
- [build_guide.md](build_guide.md) — 编译烧录
