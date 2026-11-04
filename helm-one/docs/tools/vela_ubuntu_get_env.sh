#!/bin/bash
# vela / openvela 工作区环境（按 SiFli 约定：cd 进含 *ubuntu_get_env.sh 的目录时被钩子 source）
#
# 2026-09-19 改写，解决三件事：
#   1) **每次 cd 进工作区要起 3 个 Python**（三个工具各 eval 一次补全，实测 ~0.25 s/次）
#      → 改成"补全先生成成静态文件、之后只 source"（纯 bash，~0ms），并且工具文件更新后
#      自动重生成 —— 顺带解决了"补全抄在这里、和工具自己的 complete install 两处漂移"的问题；
#   2) **PATH 不幂等**（原来 `PATH=$PATH:$HOME/SDK/bloaty`，每进一次多一条，实测连进 3 次
#      就出现 3 份）→ 改成只加一次；
#   3) 环境加载是单向的：离开工作区不会还原（会污染别的项目）。要 enter/leave 成对，
#      配 docs/tools/vela_ubuntu_env_hook.sh（把 bashrc 里那个 cd 钩子换掉即可）。

###################################################思澈（SF32LB52）###################################################
# 本工作区手工调用时的 cmake 选项（build/flash 走 vela_my_vendor_tools.py 时不需要）：
#   -DBOARD_CONFIG=../vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/
#   -DCUSTOM_MODULE_PATH=/home/jinsc/SDK/vela/openvela/build/cmake
#   -DEXTRA_FLAGS=-Wno-cpp -Wno-deprecated-declarations
#   目标: --target nuttx-bin
# 参考 PATH（构建工具链，脚本内部用绝对路径，这里只为手工调用兜底）：
#   ~/SDK/vela/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin
#   ~/SDK/vela/openvela/prebuilts/tools/cmake/bin
#   ~/SDK/vela/openvela/prebuilts/build-tools/linux-x86_64/bin
#   ~/SDK/vela/openvela/prebuilts/tools/ninja/bin

###################################################模拟器 emulator ###################################################
#   -DBOARD_CONFIG=../vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/
#   PATH 用 prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin

# ---- PATH：只加一次（反复 source / 嵌套 shell 不会膨胀）----
path_add_once() {
    case ":$PATH:" in
        *":$1:"*) ;;
        *) PATH="${PATH}:$1" ;;
    esac
}
path_add_once "$HOME/SDK/bloaty"

# ---- 补全：静态文件 + 按需重生成（不再每次 cd 起 Python）----
_vela_completion_dir="${XDG_CACHE_HOME:-$HOME/.cache}/vela-completions"

_vela_completion_load() {
    local tool="$1" base cache
    [ -f "$tool" ] || return 0
    base="$(basename "${tool%.py}")"
    cache="$_vela_completion_dir/$base.bash"
    if [ ! -s "$cache" ] || [ "$tool" -nt "$cache" ]; then
        mkdir -p "$_vela_completion_dir" || return 0
        python3 "$tool" complete bash > "$cache" 2>/dev/null || return 0
    fi
    # shellcheck disable=SC1090
    . "$cache"
}

_vela_env_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for _t in \
    "$_vela_env_root/vela_emulator_tools.py" \
    "$_vela_env_root/vela_sifli_tools.py" \
    "$_vela_env_root/vela_my_vendor_tools.py" \
    "$_vela_env_root/vendor/my_vendor/docs/tools/vela_my_vendor_tools.py"; do
    _vela_completion_load "$_t"
done
unset _t _vela_env_root _vela_completion_dir
