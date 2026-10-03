# 模块入口 — TASK-20261003-05

Primary Module: Agent Runtime。本公开快照不改变正在运行的私人镜像，所有模块成熟度按 VALIDATION.md 区分。

- Agent Runtime：`overlay/packages/ai_agent/src/core`、`infra`、`gateway`、`tools`；持久聊天请求账本、工具执行、管理员保护、统一 TLS。完成/取消/删除经可信层确认，确认队列有界、单调过期、序号复核、一次性消费；无法判断结果不自动重试。
- LabTwin Core：`src/labtwin`；schema v2、事件先提交、快照失败待修复、磁盘历史与状态机；开始后结构字段锁定，来源为 portal / portal-agent。
- Management Dashboard：`overlay/tools/labtwin-dashboard/board` 和 `public/assets`；Agent、任务、日历、录音、语音设置。板卡为唯一事实源；无 demo 初始事实，历史分页、计数/最新优先、内容版本防旧缓存。
- Voice Interaction：`src/voice`、BSP `sunxi_alsa.c`；模拟 MIC1/ADC 初始化、无额外增益流式 WAV、单采集所有者、唤醒暂停恢复、异常/断电残留修复。上限 1 小时/120 MiB 是实现约束，不是长期验收结论。
- Device UI：`overlay/vendor/allwinnertech/apps/luncher_mini`；单一 LVGL 所有者、实验流程与语音前台页面，不从网页复制另一个状态源。
- Environment Monitoring：`src/labtwin/labtwin_environment*`、BSP SHTC3；真实传感器点、5 分钟与小时存储层、阈值事件；时间未知不伪造发生时间。

构建入口见 BUILD.md；组件/C 主机测试及既有私有实板证据见 VALIDATION.md。公开许可不包含私人语音模型或原对话日志。

TASK-20261003-06（Primary Module: Management Dashboard）只补充作品说明和证据呈现，不改变模块实现：[项目设计](PROJECT.md)、[开发数据流与代码入口](DEVELOPMENT.md)、[演示流程](DEMO.md)、[带来源的实板图集](SCREENSHOTS.md)。新增 JPEG 不进入 Portal 或 ROMFS，成熟度仍按 VALIDATION.md。
