# 开发说明：源码入口、数据流与验证

Task: TASK-20261003-06；Primary Module: Management Dashboard。阅读顺序：[作品设计](PROJECT.md) → 本页 → [构建指南](BUILD.md) → [验收记录](VALIDATION.md)。完整 openvela 从固定 manifest 获取，本仓不是整个 SDK。

## 定位代码

- 实验核心：[labtwin_service.c](../overlay/packages/ai_agent/src/labtwin/labtwin_service.c)。任务 schema、状态流转、事件提交、快照与历史恢复在此汇合。
- 环境采集与存储：[labtwin_environment.c](../overlay/packages/ai_agent/src/labtwin/labtwin_environment.c)。聚合、保留、规则和事件不是浏览器本地计数。
- 管理接口：[admin_api.c](../overlay/packages/ai_agent/src/infra/admin_api.c)，认证：[admin_auth.c](../overlay/packages/ai_agent/src/infra/admin_auth.c)。实验、录音、配置和确认接口在板端执行权限校验。
- 网页聊天：[portal_chat.c](../overlay/packages/ai_agent/src/infra/portal_chat.c)；确认：[portal_operations.c](../overlay/packages/ai_agent/src/infra/portal_operations.c)；工具：[tool_registry.c](../overlay/packages/ai_agent/src/tools/tool_registry.c)。模型层不能自行跳过可信确认层。
- 录音：[recording_service.c](../overlay/packages/ai_agent/src/voice/recording_service.c)，共享音频采集/所有权在同目录；底层 MIC1/ADC 路由属于 BSP 音频适配。
- TLS：[tls_trust.c](../overlay/packages/ai_agent/src/infra/tls_trust.c)、[vela_tls.c](../overlay/packages/ai_agent/src/infra/vela_tls.c)。信任根、时间门控、主机名和验证结果统一处理。
- 网页：[Portal 维护源码](../overlay/tools/labtwin-dashboard)，`board` 为板端应用，`public/assets` 为维护的录音/语音扩展资源；本机 UI：[luncher_mini](../overlay/vendor/allwinnertech/apps/luncher_mini)。不要修改构建后的哈希 bundle。

更多分工见 [modules.md](modules.md)。overlay 是增量输入，其他依赖由上游提供；按 [SOURCE_MANIFEST.json](../SOURCE_MANIFEST.json) 恢复来源和文件模式。

同源 C/LVGL 桌面离屏截图和复现工具见 [SIMULATOR_UI.md](SIMULATOR_UI.md)。直接编译当前 UI 原码，使用既有 demo 场景；它不是 Goldfish/R528 固件运行，也不调用真实板端服务。

## 实验与时间

schema v2 增加说明、计划起止、创建/开始/结束 epoch。旧记录缺失字段按兼容空值处理，不覆盖原历史。时间以 Unix 时间戳保存，网页按浏览器时区显示；没有可信时间就保留未知，不伪造实际时间。

生命周期主要为 `READY → RUNNING ↔ PAUSED → COMPLETED / CANCELLED`。READY 可编辑名称、说明、计划和步骤；开始后结构锁定。编辑携带 `last_event_seq`，冲突拒绝，不允许旧页面覆盖新状态。计划时间与实际时间独立：READY 在日历按计划显示，运行与历史优先使用实际发生时间。

修改先在临时对象完整校验，再写完整事件并 `fsync`。提交前失败不改变原对象或序号；部分写入尝试截回原偏移，恢复无法确认时隔离任务。事件成功但快照失败表示操作已生效、快照待修复，使用新内存状态并尝试修复；不可提示用户重新执行。快照缺失/损坏从完整事件恢复。缓存与活动任务有界，历史索引在磁盘；不能以“超过 16 条”删旧实验。

## Agent、确认与重连

网页 Agent 使用独立受保护通道及持久聊天记录。请求编号区分“查询原结果”和“提交新操作”，网页断线/刷新先恢复原编号的状态，不盲目重复执行。清空、发送、并发与迟到结果有回归测试；本地 `/time` 不需要模型凭据。

网页 Agent 的完成、取消和删除进入最多 8 项待确认队列，120 秒单调时钟期限；令牌绑定管理员身份、参数和实验序号。确认前在实验锁内复核，一次性消费并记录结果。刷新能查询待确认项，重启使未确认令牌失效。已执行的重复确认返回已有结果；无法确认执行结果时标记 `UNCERTAIN`，不自动重试。

删除由 API 与 Agent 共用服务。要求完整实验编号；是否还要求管理员密码由板端认证模式决定，不能信任请求声称“免密”。写接口延用会话、同源、CSRF、请求 ID 与审计，来源区分 `portal` 和 `portal-agent`。这些机制保护实验软件状态，并不代表具备物理设备的安全联锁。

## 录音与存储

服务状态为 `IDLE → STARTING → RECORDING → STOPPING → FINALIZING → IDLE`，原子预约启动且线程只回收一次。专属采集线程持续写 PCM，不与唤醒推理竞争；打开模拟采集时初始化 MIC1/ADC 路由，无额外软件增益。

每秒 32,000 字节 PCM，最多 3,600 秒，即 115,200,000 字节音频加 44 字节 WAV 头。120 MiB 配额还受实际空闲空间及保留空间约束，因此剩余时长并非总能达到一小时。录音不会自动删除历史。

`.part` 正常结束补头、同步并原子发布；用户停止引起的读取中断属于正常结束。采集异常但完整帧可保留时发布部分录音并给出原因；发布失败保留可恢复临时文件，不虚报成功。启动恢复残留。`GET /api/v2/recordings/{id}/audio` 返回 `audio/wav` 并支持 HTTP Range；录音结束恢复此前唤醒状态，而不是无条件开启唤醒。

## 构建与回归

按 [BUILD.md](BUILD.md) 创建干净固定基线、校验并应用 overlay、安装锁定 npm 依赖，再构建 Portal 和同步专用 ROMFS。确认四段链：维护源 → ROMFS 输入 → IMG → 实际浏览器执行资源；同名 URL 的旧缓存曾使页面行为落后于固件，不能只核对 HTTP 文件存在。

常用验证入口：

```bash
python3 labtwin/tools/check_release.py
python3 labtwin/tools/check_docs.py
python3 -m unittest discover -s labtwin/tools -p 'test_*.py' -v
```

上述在提交仓运行；C 主机测试与 Portal 命令在应用 overlay 后的 openvela 目录运行，完整命令见构建指南。公开 CI 检查精确文件清单/新增历史、发布工具、文档链接/图像来源、Portal 单元/组件、lint 和板端网页生产构建；不自动烧录板卡。

开发使用 Task ID、隔离分支/worktree 和单一集成责任人。功能交接记录基线、文件、同步、测试和未验收项；没有实板证据就不把主机通过写成硬件通过。秘密规则扫描只是门槛，不是完备保证；凭据、原音频、设备数据、私有 IMG 与训练模型不得混入公开源码。
