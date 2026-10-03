# 许可证与第三方来源

本仓新增自研 Portal、发布工具和项目文档采用 Apache-2.0。根 LICENSE 不替代现有文件/依赖的原许可证。Portal 的依赖通过 `package-lock.json` 固定，`npm ci` 后可在各包中查看 LICENSE；不把 node_modules 或其二进制整体提交。

| 组件 | 来源和许可归属 |
| --- | --- |
| openvela AI Agent | `overlay/packages/ai_agent/LICENSE`（Apache-2.0），`NOTICE` 保留 Xiaomi/MimiClaw 归属和 MimiClaw MIT 全文 |
| openvela / NuttX apps / KVDB | 固定 manifest 对应上游仓，保留 Apache-2.0 等原文件头和许可证 |
| Allwinner BSP / Wi-Fi 驱动 | `vendor_allwinnertech` 固定上游与各文件的原版权/许可；不统一改成 Apache-2.0 |
| LVGL | `apps_graphics_lvgl` 固定上游，保留 MIT 原许可 |
| TensorFlow Lite Micro | `apps_mlearning_tflite-micro` 固定上游，保留 Apache-2.0 等原归属 |
| Mozilla CA | 未修改的 Mozilla NSS 信任根提取包，MPL-2.0；PEM 原头与 `CA-PROVENANCE.md` 一并保留 |
| FFmpeg 等固件依赖 | 由完整上游 manifest 获取，适用各自许可证；若公开二进制，另需履行相应源代码/重链接等义务 |

确切上游来源与 revision 见 `../manifests/upstream.xml`；项目修改的来源 revision、公开调整和逐文件 SHA-256 见 `../SOURCE_MANIFEST.json`。本次不发布固件二进制，不表示已完成未来二进制发布的许可义务。

作者授权的是源码公开和 Portal 许可；原始人声录音、私有训练模型、实际实验记录、聊天和设备配置均不在本发布范围。公开占位模型版本为 `untrained`，长度为 0，唤醒模块失败关闭，不向网络自动发送音频。现有训练工具保留但不宣称能复现私有 MF32 调试模型。
