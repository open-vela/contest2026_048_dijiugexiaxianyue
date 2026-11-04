cd /home/jinsc/SDK/vela/openvela

# 删除除 .repo 以外的所有内容
find . -maxdepth 1 ! -name '.' ! -name '.repo' -exec rm -rf {} +

# 重新检出所有子项目
repo sync -d --force-sync -j$(nproc)

#  r切换分支
cd /home/jinsc/SDK/vela/openvela/vendor/sifli

git fetch openvela dev-ai-contest-2026
git checkout -B dev-ai-contest-2026 openvela/dev-ai-contest-2026