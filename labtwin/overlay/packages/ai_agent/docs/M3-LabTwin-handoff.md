# M3：Gemini-S1 实验台 AI 终端开发交接

更新时间：2026-07-14（Asia/Shanghai）

## 1. 当前交付状态

M2「LabTwin 本地实验状态闭环」已经完成并在 Gemini-S1 实体板通过核心验收。当前固件无需网络或 LLM 即可完成实验创建、状态迁移、多任务计时、观察记录、持久化和重启恢复。

已验证能力：

- `READY`、`RUNNING`、`PAUSED`、`COMPLETED`、`CANCELLED` 确定性状态机；
- 非法迁移和已结束实验写操作返回稳定错误；
- 3 个实验计时器并行运行，暂停和恢复同步生效；
- 重复观察返回 `ALREADY_APPLIED`，不产生重复事实事件；
- 可信时间下重启后按离线时长恢复计时；
- 启动时系统时间为 1970 时等待 NTP，避免以错误时间恢复；
- 实验完成时活动计时器统一转为 `CANCELLED`；
- `luncher_mini` 启动时幂等拉起 LabTwin 服务，服务独立于单次 `ai_agent` 会话。

## 2. 源码与提交基线

实际源码位于 Ubuntu 虚拟机 `/home/li/openvela`。Windows `D:\openvela` 用于文档、固件和共享技能，不是 Git 仓库。

### packages/ai_agent

分支：`gemini-s1-labtwin-ui`

- `449b04683d24a1a96da19f2f5c3ef350fe73279a` — `fix(gemini-s1): guard timer recovery against clock rollback`
- `8bb515bc7b272dfbf548ed0d6328a3e5ca0f9d15` — `feat(gemini-s1): add LabTwin experiment runtime`

### vendor/allwinnertech

- `ab32b7bfd46f712f8eca41d052b9de8199a08965` — `feat(gemini-s1): connect LabTwin runtime UI`

交接时两个仓库 `git status --short` 均为空。未执行 `push`。

## 3. 关键代码入口

### LabTwin 核心

- `packages/ai_agent/include/labtwin/labtwin.h`
- `packages/ai_agent/src/labtwin/labtwin_service.c`
- `packages/ai_agent/tests/labtwin_host_test.c`
- `packages/ai_agent/tests/labtwin_recovery_test.c`

核心采用共享地址空间内的线程安全 C API，不使用网络 IPC。所有写操作经过统一服务锁、参数校验、事件追加和快照更新。

### Agent 工具与 CLI

- `packages/ai_agent/src/tools/tool_labtwin.c`
- `packages/ai_agent/src/tools/tool_labtwin.h`
- `packages/ai_agent/src/channels/cmd_labtwin.c`
- `packages/ai_agent/src/channels/cmd_labtwin.h`

已注册 8 个 P0 工具：

1. `experiment_create`
2. `experiment_get`
3. `experiment_list`
4. `experiment_transition`
5. `timer_start`
6. `timer_cancel`
7. `lab_log_add`
8. `sensor_snapshot`

无需 LLM 的调试入口为 `ai_agent` 交互命令 `lab ...`。Windows ADB 自动输入建议使用：

```powershell
cmd /c "(echo lab get <ID>& echo quit) | adb shell ai_agent"
```

`ai_agent` 初始化目前约需 44 秒，自动化命令超时应设为至少 80 秒。不要使用 PowerShell 对象管道直接向 `adb shell ai_agent` 连续写入，实测可能导致 Windows ADB 进入 `offline`。

### 板载 UI

- `vendor/allwinnertech/apps/luncher_mini/lab_ui.c`
- `vendor/allwinnertech/apps/luncher_mini/lab_ui.h`
- `vendor/allwinnertech/apps/luncher_mini/lab_ui_controller.c`
- `vendor/allwinnertech/apps/luncher_mini/lab_ui_controller.h`
- `vendor/allwinnertech/apps/luncher_mini/luncher_mini.c`

真机通过真实 controller 读取 LabTwin 快照；非 UI 线程不直接调用 LVGL。桌面预览仍使用确定性 Mock。

## 4. 数据与恢复约定

板端数据目录：

```text
/data/labtwin/experiments/<experiment_id>/snapshot.json
/data/labtwin/experiments/<experiment_id>/events.jsonl
```

恢复关键点：

- 先追加 JSONL 事件，再原子替换快照；
- 启动时载入快照并重放其后的事件；
- 最后一个截断 JSONL 行可忽略，中间损坏会进入恢复错误；
- 运行期使用单调时钟，可信墙钟同时保存 `due_epoch`；
- 服务启动时先从所有持久化实验建立墙钟高水位，再恢复计时器；
- 当前时间低于持久化高水位时视为不可信，最长等待 120 秒；
- 超时后安全暂停并标记 `time_untrusted`，由人工 `resume` 恢复。

## 5. 实体板验收记录

最终复测样例 `M2C`：

- 创建后 `READY → RUNNING`；
- 启动 `300/330/360` 秒三个计时器；
- 暂停后全部为 `PAUSED`，恢复后全部为 `RUNNING`；
- 相同观察第二次写入返回 `ALREADY_APPLIED`，事件序号保持不变；
- 实体板重启后剩余时间为 `222/252/282` 秒，三个间隔保持 30 秒；
- `RUNNING → start` 返回 `INVALID_STATE`；
- `complete` 后三个计时器均为 `CANCELLED`；
- 完成后继续写日志返回 `INVALID_STATE`。

此前样例 `M2R2` 的 180 秒计时器在重启后恢复为 104 秒，确认时间回退修复有效。

## 6. 最终固件

固定烧录文件：

```text
D:\openvela\rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img
```

历史归档：

```text
D:\openvela\firmware_history\20260714-152259_CST_gemini-s1_m2-clock-guard_17e52853.img
/home/li/openvela/firmware_history/20260714-152259_CST_gemini-s1_m2-clock-guard_17e52853.img
```

- 大小：`27,396,096` bytes
- SHA-256：`17e528531799e6375838211ec19aacd1cdef3ae313b7188edb952db3580a46e2`

## 7. 已知问题与边界

- `ai_agent` 启动会重复打印 `[ble_gatt] FAILED: no BT instance`，不影响 LabTwin CLI 和工具，但会增加启动等待时间；
- Windows ADB 在长时间批量交互后偶发 `offline`，拔插 USB 或重启 ADB server 可恢复；
- 温湿度工具在一次板测中返回不可用，接近传感器可用但读数为 0；M3 应确认具体 uORB topic、设备注册和采样频率；
- 核心 UI controller 与进程已验证，完整触摸按钮路径仍建议在每次 UI 变更后人工回归；
- M2 不包含 ASR/TTS、环境阈值告警、自动实验报告和 PC 看板。

## 8. M3 建议目标

建议将 M3 定义为「本地语音交互与实验事件感知」，在不破坏 M2 确定性核心的前提下增加：

1. ASR 输入适配：把语音文本映射到现有 8 个工具，任何状态修改仍只能通过 LabTwin API；
2. TTS/提示音反馈：覆盖创建、开始、暂停、到期、时间不可信和非法操作；
3. 环境事件规则：温度、湿度和接近传感器只产生确定性事件，不让 LLM 直接改实验状态；
4. 到期通知闭环：屏幕、提示音和 Agent 消息共享同一事件 ID，避免重复提醒；
5. 原始语音文本和人工记录关联到实验及步骤，同时保留未经 LLM 改写的原文；
6. 离线降级：无网络或 LLM 不可用时，计时、规则、UI 和本地命令继续运行。

## 9. M3 推荐实施顺序

1. 先确认音频输入、输出、传感器 uORB topic 和板端资源基线；
2. 定义语音意图、环境规则、通知事件的 C 接口和幂等键；
3. 完成本地事件总线和提示音，不接 LLM 做主链路测试；
4. 接入 ASR/TTS，再把结构化意图调用到现有 LabTwin 工具；
5. 更新板载 UI 的录音、识别、确认、告警状态；
6. 进行桌面、主机、固件和实体板逐层验收；
7. 按项目规范归档打包前后固件，并报告完整 SHA-256。

## 10. M3 建议验收门槛

- 语音完成一次“创建实验→开始→启动计时→暂停→恢复→记录→完成”；
- 同一 ASR 结果重复投递不产生重复状态事件；
- 误识别或低置信度命令不得改变实验状态；
- 计时到期只通知一次，重启恢复后不重复播报同一到期事件；
- Wi-Fi 和 LLM 断开时，本地状态机、计时、环境事件和屏幕继续工作；
- 连续运行 1 小时，无崩溃、死锁或持续堆增长；
- 实体板验证麦克风、扬声器、触摸、显示和传感器，Goldfish 结果不得替代硬件验收。

## 11. 常用命令

源码连接优先尝试项目文档中的：

```powershell
ssh openvela-vm "cd /home/li/openvela && pwd"
```

交接当日 `192.168.1.47` 不可达，实际使用 `li@192.168.0.101` 成功连接。应在开始 M3 前确认 VM 当前地址，不要把临时地址写入源码。

构建：

```bash
cd /home/li/openvela
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay -j4
```

板端检查：

```powershell
adb devices -l
adb shell "date; ps"
cmd /c "(echo lab list& echo quit) | adb shell ai_agent"
```

