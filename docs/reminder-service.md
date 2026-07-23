# Reminder Service

AI 台灯本地提醒服务用于把中文提醒语句转成具体提醒任务，并在到点后进行屏幕文字提醒和提示音提醒。

## 支持的输入

- `1分钟提醒我喝水`
- `3分钟提醒我`
- `2小时后提醒我休息`
- `明天提醒我`
- `明天下午3点提醒我写作业`
- `12月1日提醒我复查视力`
- `12月1日20:30提醒我准备材料`
- `下周三提醒我`
- `下周三晚上8点提醒我整理书包`

未指定具体时间时默认 `09:00`，其中 `中午` 默认 `12:00`，`下午` 默认 `15:00`，`晚上` 默认 `20:00`。未指定提醒内容时使用 `提醒时间到了`。

## 任务结构

每个提醒任务包含：

- `id`: 本地自增任务 ID。
- `type`: `relative`、`tomorrow`、`date`、`next_weekday`、`absolute`。
- `remind_at`: Unix 秒级时间戳。
- `content`: 到点提醒内容。
- `original_text`: 用户原始语句。
- `fired`: 是否已触发。

任务存储在 NVS 的 `reminders` 命名空间，重启后仍会保留。已触发任务保留 24 小时后清理。

## MCP 工具

- `self.reminder.create_from_text`: 从中文文本创建提醒。
- `self.reminder.create_at`: 使用指定 Unix 秒级时间戳和内容创建提醒。
- `self.reminder.list`: 查询待提醒和近期已触发提醒。
- `self.reminder.delete`: 删除指定提醒任务。

## 服务端同步建议

后续如果要让设备定时向服务端请求提醒数据，可以保留本地 `ReminderService` 作为统一执行层：

1. 设备每隔固定时间调用服务端接口，如 `GET /api/device/reminders?device_id=...&since=...`。
2. 服务端返回标准任务列表：`server_id`、`remind_at`、`content`、`updated_at`、`deleted`。
3. 设备把服务端任务转换成本地 `absolute` 任务，写入 NVS。
4. 设备上报执行结果：`POST /api/device/reminders/{server_id}/events`，事件包含 `fired_at`、`status`。
5. 本地语音和屏幕提醒仍由 `ReminderService` 到点触发，避免网络延迟影响提醒准时性。

