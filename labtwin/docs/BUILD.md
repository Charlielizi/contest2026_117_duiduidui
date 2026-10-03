# 从公开源码重建

## 环境与基线

Ubuntu 22.04 / Python 3.10 / CMake 3.22，Node >=22.13（验证使用 22.23.1 / npm 10.9.8）。Gemini-S1 使用完整 `dev-ai-contest-2026` manifest，不能只切换 BSP；本包将全部 247 个上游项目锁定为 commit SHA。实际私有参考构建使用项目内 ARM GNU 13.4.0 工具链，不能误用 PATH 上另一个版本。

公开 overlay 在干净基线应用。不要在有私人修改、凭据或已有构建状态的开发目录执行。

```bash
mkdir openvela-public && cd openvela-public
repo init -u https://github.com/Charlielizi/contest2026_117_duiduidui.git \
  -b dev-ai-contest-2026 -m labtwin/manifests/upstream.xml --no-repo-verify
repo sync -c -j4
git clone --branch dev-ai-contest-2026 \
  https://github.com/Charlielizi/contest2026_117_duiduidui.git submissions/labtwin
python3 submissions/labtwin/labtwin/tools/check_release.py
python3 submissions/labtwin/labtwin/tools/apply_overlay.py "$PWD" --dry-run
python3 submissions/labtwin/labtwin/tools/apply_overlay.py "$PWD"
```

`apply_overlay.py` 先校验全部输入与每个改动仓的基线/干净状态，再覆盖清单里的文件并验证输出。两个兼容头按清单还原为相对符号链接。对无关应用/其他板卡的上游删除不传播；已生成的旧 Portal bundle 不传播，而是从维护源重建。应用后各上游仓会出现有意的未提交 overlay，不执行 repo sync 或 reset 覆盖它们。脚本不保证断电时多文件原子化；若中断，保留目录并在新的干净目录重做。

## 网页维护源码与 ROMFS

```bash
cd tools/labtwin-dashboard
npm ci
npm run test:unit
npm run lint
npm test
cd ../..
python3 submissions/labtwin/labtwin/tools/sync_portal.py "$PWD"
```

同步脚本重新 `build:board`，只更新专用 `labtwin-admin` ROMFS 输入，写入 `RESOURCE_SHA256SUMS` 和公开源码清单内容标识。不得手工改带哈希的 bundle。`Sync-BoardPortalToVm.ps1` 是原私有维护环境的同步工具，公开重建应使用上述无需特定用户名/SSH 路径的入口。

## 主机测试与目标编译

```bash
export OPENVELA_ROOT="$PWD"
bash packages/ai_agent/tools/Run-M5HostTests.sh
bash packages/ai_agent/tools/Run-PriorityHostTests.sh
bash packages/ai_agent/tools/Run-PortalChatHostTests.sh
bash packages/ai_agent/tools/Run-WebStaticHostTests.sh
bash packages/ai_agent/tools/Run-TlsHostTests.sh
bash apps/testing/ntpclient_host/run.sh
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay -j4
```

TLS 主机测试会用 OpenSSL 生成临时测试证书，在本机 29443 端口测试合法/错误主机名/不可信/过期证书；这不是调用云服务。Goldfish overlay 仅用于业务模拟，不模拟 R528 的音频、屏幕、Wi-Fi 和传感器。

构建后仍须按 BSP 规范打包、校验并用 PhoenixSuit 手动烧录；本发布不提供 IMG、不自动烧录或全盘擦除。擦除前需明确备份/数据保留授权。

## 公开默认与现场设置

公开源码关闭免密管理员，默认 Wi-Fi 名称/密码为空，也没有模型、ASR、TTS API 凭据。按板端/网页提示配置自己的网络与管理员；在网页保存服务配置。有效时间与可信 TLS 校验是云请求前提，失败不退回跳过验证。

这是经过脱敏的源码版，不与私人实验室镜像逐字节相同：未训练占位唤醒模型、认证模式和网络默认不同。公开版按键语音/本地录音可作为后续验收路径，不能将私有参考镜像的离线唤醒或免密网络结果套用为公开版已验收。
