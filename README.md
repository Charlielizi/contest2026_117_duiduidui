# Gemini-S1 LabTwin：实验台 AI 终端

面向实验台的 AI 硬件作品，基于 Gemini-S1 / Allwinner R528 / openvela 与 AI Agent，把实验步骤、并行倒计时、观察记录和温湿度事件汇入同一板端事实源，并提供局域网网页工作台。赛道：AI 硬件产品创新，队伍 117「duiduidui」。

![LabTwin](labtwin/overlay/tools/labtwin-dashboard/public/assets/labtwin-logo.png)

## 作品亮点：让实验过程可控制、可追溯

LabTwin 面向“同时进行多个观察、倒计时和记录，结束后还要回查过程”的实验台场景，将 AI 交互与结构化实验管理结合起来：板端管理实验状态、计时、观察、环境事件和录音，网页与现场屏幕共同呈现完整过程。

- **对话与任务联动**：Agent 通过实验工具查询和操作任务，完成、取消、删除等重要动作提供摘要与确认，交互直观、执行可控。
- **排期与实际过程分开**：任务开始前编辑计划和步骤，开始后锁定结构；月/周/列表日历同时承载计划与历史，实际开始、结束时间优先。
- **现场记录便于回查**：网页控制板载麦克风，温湿度趋势与阈值事件帮助还原实验环境，文字观察与音频记录相互补充。
- **面向小内存设备的恢复设计**：有界活动任务/确认队列、磁盘历史、事件提交点、录音临时文件修复和持久请求编号，避免把浏览器刷新当成新的执行命令。

### 现场主页：把关键状态放在一屏

![板端 UI：实验台主页与环境摘要](labtwin/docs/images/sim-ui-idle.png)

板端 UI 将时间、环境摘要和最近实验集中显示，底部提供实验、语音与多任务入口，适合实验台上的快速查看。

### 实验步骤与并行计时：随时知道做到哪一步

![板端 UI：实验步骤与倒计时](labtwin/docs/images/sim-ui-running.png) ![板端 UI：并行任务与计时焦点](labtwin/docs/images/sim-ui-timers.png)

运行页突出当前步骤、进度和剩余时间；多任务页保留其他任务状态，切换焦点即可查看不同倒计时。暂停、下一步和结束有独立入口。

### 语音交互与结束确认：文字可读，动作明确

![板端 UI：语音状态与对话文字](labtwin/docs/images/sim-ui-voice.png) ![板端 UI：完成实验确认](labtwin/docs/images/sim-ui-complete-confirm.png)

语音前台将状态提示与对话文字分区，长内容在独立卡片阅读；完成实验另有确认入口，网页 Agent 的重要操作也提供摘要与确认，让任务控制更清晰。

### 环境告警：把变化带回实验过程

![板端 UI：温度告警与确认入口](labtwin/docs/images/sim-ui-alert.png)

告警页并列展示关联实验、当前值与阈值；网页提供温湿度趋势和事件回查，把现场环境变化与实验过程联系起来。

### 网页录音与历史日历：回查现场证据

![Gemini-S1 实板：三条历史录音与浏览器回放](labtwin/docs/images/recordings-board.jpg)

历史录音按最新优先排列，显示录制时间、时长和大小；原生播放器支持播放、暂停和进度拖动，便于回听现场声音。

![Gemini-S1 实板：周日历中的本地测试任务](labtwin/docs/images/calendar-board.jpg)

月、周、列表三种日历视图覆盖排期与历史，结合状态筛选、任务详情和时间线，从计划到复盘贯穿同一条实验记录。

阅读入口：[项目设计与亮点](labtwin/docs/PROJECT.md) · [网页 UI 展示](labtwin/docs/SCREENSHOTS.md) · [现场演示流程](labtwin/docs/DEMO.md) · [开发说明与图片来源](labtwin/docs/DEVELOPMENT.md) · [测试记录](labtwin/docs/VALIDATION.md)。

## 主要能力

- 板端实验流程状态、最多 8 个活动实验、多计时器、观察与报告；磁盘历史独立于有限内存槽位，事件提交与恢复保留旧数据兼容。
- 网页 Agent 独立会话、历史恢复与请求去重；任务编辑、月/周/列表日历与历史筛选；完成/取消/删除经可信执行层确认，删除按板端认证模式校验。
- 温湿度 5 分钟聚合、约一月的保留策略和较旧数据按小时压缩，结合阈值规则与时间可信性标记。
- 网页板端麦克风录音、历史列表、原生 WAV 播放/拖动/删除；16 kHz / 16-bit / 单声道，单条上限 1 小时、总配额 120 MiB，录音时暂停唤醒。
- 板端 LVGL 实验/语音页面、按键对话入口、识别文字与回复阅读、ASR/TTS 配置管理及统一可信 TLS 验证。

本仓提供项目源码、可重建的跨仓修改、测试工具和界面展示资料。部署配置、图片来源及测试状态统一见[开发说明](labtwin/docs/DEVELOPMENT.md)与[测试记录](labtwin/docs/VALIDATION.md)。

## 源码布局

```text
labtwin/overlay/packages/ai_agent/    Agent、实验核心、录音、网关、安全、测试
labtwin/overlay/tools/labtwin-dashboard/  可维护 React Portal、扩展、资源、组件测试
labtwin/overlay/vendor/allwinnertech/    Gemini-S1 UI、BSP、音频/网络、配置、CA
labtwin/overlay/apps/                   NTP/WAPI/TFLM/LVGL 及回归测试
labtwin/overlay/frameworks/system/utils/ KVDB 持久化
labtwin/overlay/vendor/openvela/        Goldfish 业务模拟配置
labtwin/manifests/upstream.xml       固定 247 个上游项目 revision
labtwin/SOURCE_MANIFEST.json         来源、公开调整、逐文件 SHA 与模式
labtwin/tools/                      校验、基线安全应用、Portal ROMFS 同步
labtwin/docs/                       构建、验收边界、许可证与模块说明
app/ board/ quickapp/ logs/          原比赛模板保留；不是本作品入口
```

必要的跨仓修改随 overlay 提交，上游完整源码由固定 manifest 获取；不带私有 Git 历史。公开重建从维护源生成 ROMFS，不手工修改带哈希的 bundle。

## 重建与使用

完整环境、逐步命令与安全默认见[构建指南](labtwin/docs/BUILD.md)。拉取固定上游基线并校验/应用 overlay，再运行 Portal 测试、生产构建与 ROMFS 同步，最后编译：

```bash
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay -j4
```

Portal 使用 `npm ci`、`npm run test:unit`、`npm run lint`、`npm test`、`npm run build:board`。C 主机测试涵盖事务故障/重放、Agent 确认与聊天账本、录音发布与 Range、TLS 和 NTP UDP。

## AI Coding 与协作开发

使用 Codex 辅助需求拆解、实现、跨仓集成、回归、ADB/网页故障定位和文档维护；采用 Task ID、隔离分支及单一集成责任人，校验“维护源 → ROMFS → IMG → 实体页面”避免多对话修改遗漏。

固定上游 revision、逐文件 SHA 和持续集成共同支撑源码追溯与重建；研发记录与发布资料按任务整理，相关说明见[开发文档](labtwin/docs/DEVELOPMENT.md)。

## 许可

新增自研 Portal、发布工具和文档采用 [Apache-2.0](LICENSE)。AI Agent 原 LICENSE/NOTICE、MimiClaw MIT、BSP 和其他第三方原许可均保留，不统一重新授权。[第三方说明](labtwin/docs/THIRD_PARTY.md)列出来源与公开范围。
