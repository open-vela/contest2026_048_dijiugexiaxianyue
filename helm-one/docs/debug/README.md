# debug/ — 单点排障复盘

**这不是排障手册**，是两次现场问题的完整复盘（历史记录，结论已落地）。
现在遇到问题，从 [../tools/debug_workflow.md](../tools/debug_workflow.md) 起步。

| 文档 | 当时的症状 | 结论 |
| --- | --- | --- |
| [mklfs.md](mklfs.md) | SD 卡上 LittleFS 挂载要 8 s | 降到 0.47 s；根因与改动记录在文内（2026-08-13） |
| [mtp_upload.md](mtp_upload.md) | MTP 上传慢、gvfs 报错 | 分层定位方法 + 各层判据（2026-09-07） |

## 现行排障入口（按顺序）

| 症状 | 去哪 |
| --- | --- |
| 死机、卡顿、线程停摆 | [../diag.md](../diag.md) → [../diag_log.md](../diag_log.md) |
| SD 卡读失败 / 变哑 / 复位 | [../sd_recovery.md](../sd_recovery.md) → [../sd_partition.md](../sd_partition.md) |
| GNSS / BLE 日志看不懂 | [../gnss_ble_diag_log.md](../gnss_ble_diag_log.md) |
| 看门狗、中断优先级、clip | [../watchdog_irq_priority.md](../watchdog_irq_priority.md) |
| 一次已知异常的清单式扫尾 | [../board_anomaly_scan_2026-10-14.md](../board_anomaly_scan_2026-10-14.md) |
