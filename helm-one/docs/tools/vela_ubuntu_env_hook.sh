#!/bin/bash
# 进/出工作区时整体加载/还原环境（一个最小号的 direnv）。
#
# 为什么需要：默认那个 cd 钩子只会**进**不会**出** —— 进了 openvela 之后 PATH、补全、
# 变量就一直带着，去别的项目也还在（我们的 PATH 还因为不幂等越进越长）。这里把钩子换成
# 成对的：进工作区时先记住当前 PATH，source 环境；离开时还原。
#
# 怎么用（二选一）：
#   A) 不想改启动脚本：每到工作区先执行一次
#        source vendor/my_vendor/docs/tools/vela_ubuntu_env_hook.sh
#      它会接管当前 shell 的 cd。
#   B) 持久：把启动脚本里那段 _load_ubuntu_env/cd 换成
#        source /home/jinsc/SDK/vela/openvela/vendor/my_vendor/docs/tools/vela_ubuntu_env_hook.sh
#
# 行为：
#   - 进入含 *ubuntu_get_env.sh 的目录（比如 openvela 根）⇒ 保存 PATH 后 source；
#   - 离开那个目录 ⇒ 还原 PATH，卸载补全里带的函数（只清我们加载过的）；
#   - 反复 cd 进/出不会重复叠加（幂等）。

_vela_hook_saved_path=""
_vela_hook_active=0
# 每个工作区一个标记变量（这里只关心本工作区；多个工作区可自行加行）
_vela_hook_root="/home/jinsc/SDK/vela/openvela"

_vela_env_probe() {
    # 输出当前目录下要 source 的环境文件（按 SiFli 约定：./*ubuntu_get_env.sh 或 ./tools/...）
    shopt -s nullglob
    local s
    for s in ./*ubuntu_get_env.sh ./tools/*ubuntu_get_env.sh; do
        [ -f "$s" ] && printf '%s\n' "$s"
    done
    shopt -u nullglob
}

_vela_env_enter() {
    [ "$_vela_hook_active" = 1 ] && return 0
    local scripts
    scripts="$(_vela_env_probe)"
    [ -n "$scripts" ] || return 0
    _vela_hook_saved_path="$PATH"
    _vela_hook_active=1
    local s
    while IFS= read -r s; do
        # shellcheck disable=SC1090
        . "$s"
    done <<< "$scripts"
}

_vela_env_leave() {
    [ "$_vela_hook_active" = 1 ] || return 0
    PATH="$_vela_hook_saved_path"
    _vela_hook_active=0
    # 补全函数是 source 出来的，留着无害；要更干净可 unset -f 掉那几个。
}

_vela_env_sync() {
    if [ -n "$(_vela_env_probe)" ]; then
        _vela_env_enter
    else
        _vela_env_leave
    fi
}

cd() {
    builtin cd "$@" || return
    _vela_env_sync
}

_vela_env_sync
