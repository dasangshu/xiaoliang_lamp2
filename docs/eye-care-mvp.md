# 护眼提醒 MVP

本文档记录 AI 台灯护眼功能第一版的落地范围。当前定位是“学习场景护眼习惯提醒”，不做医学诊断或近视治疗承诺。

## 已实现能力

- 通过坐姿检测结果判断孩子是否在书桌前学习。
- 进入学习状态后累计：
  - 今日学习时长
  - 当前连续用眼时长
  - 学习会话次数
- 默认连续用眼 `20 分钟` 后触发远眺提醒。
- 默认远眺建议 `20 秒`，屏幕提示“看 6 米外”。
- 离开座位并持续达到远眺时长后，记为一次远眺完成，并清零连续用眼时长。
- 每天首次学习开始后推送一条护眼小科普。
- 学习过程中推送握笔姿势科普提醒。
- 学习过程中推送视力表方向识别练习提醒，结果定位为家庭参考。
- 学习过程中推送 3-5 秒科普小动效提示；是否真正播放取决于 SD 卡/资源包中是否有对应 `.mjpeg`。
- 按配置周期推送定期视力筛查提醒。
- 家长小程序/后台可通过 MCP 读取状态、下发配置、重置当天统计。
- 坐姿提醒继续复用现有 `PostureDetector` 规则和健康分。

## 默认规则

默认规则和提醒文案维护在：

- 固件内置配置：[main/eye_care/eye_care_rules.json](/Users/swf/www/xiaoliang/xiaozhi-esp32-kevin/main/eye_care/eye_care_rules.json)
- 设备现场覆盖配置：`/sdcard/eye_care_rules.json`

启动时加载顺序：

1. 先加载固件内置 `eye_care_rules.json`
2. 如果 SD 卡存在 `/sdcard/eye_care_rules.json`，再用 SD 卡配置覆盖
3. 最后加载家长小程序通过 MCP 保存到 NVS 的个性化配置

| 配置项 | 默认值 | 说明 |
| --- | ---: | --- |
| `enabled` | `true` | 护眼功能总开关 |
| `far_sight_enabled` | `true` | 远眺提醒开关 |
| `daily_knowledge_enabled` | `true` | 每日护眼科普开关 |
| `posture_enabled` | `true` | 坐姿检测和坐姿提醒开关 |
| `screening_enabled` | `true` | 定期视力筛查提醒开关 |
| `grip_tip_enabled` | `true` | 握笔姿势科普提醒开关 |
| `vision_chart_tip_enabled` | `true` | 视力表练习提醒开关 |
| `science_video_tip_enabled` | `true` | 科普小动效提醒开关 |
| `screen_video_enabled` | `true` | 是否允许屏幕显示提醒动画 |
| `rest_music_enabled` | `false` | 远眺/训练音乐开关，当前仅保存配置 |
| `continuous_use_minutes` | `20` | 连续用眼提醒阈值 |
| `far_sight_seconds` | `20` | 远眺建议时长 |
| `absence_reset_seconds` | `180` | 离座多久后清零连续用眼 |

JSON 中可维护的内容：

- `default_config`：默认开关和阈值
- `timing`：学习状态识别 TTL、每日科普延迟、远眺提醒最小间隔、筛查/握笔/视力表/动效提示时机
- `messages`：学习开始、远眺提醒、远眺完成、每日科普、筛查、握笔、视力表、科普小动效的屏幕文案/动画/展示时长
- `daily_knowledge`：每日护眼科普文案列表

## MCP 工具

### `self.eye_care.get_status`

读取护眼配置和当天统计。

返回内容为 JSON 字符串，包含：

- `config`
- `stats`

### `self.eye_care.set_config`

下发完整配置。小程序应一次提交所有字段：

- `enabled`
- `far_sight_enabled`
- `daily_knowledge_enabled`
- `posture_enabled`
- `screening_enabled`
- `grip_tip_enabled`
- `vision_chart_tip_enabled`
- `science_video_tip_enabled`
- `screen_video_enabled`
- `rest_music_enabled`
- `continuous_use_minutes`
- `far_sight_seconds`
- `absence_reset_seconds`

### `self.eye_care.reset_daily_stats`

重置当天护眼统计。

## 仍未进入第一版

- 握笔姿势实时识别。
- 视力表交互式筛查/训练流程。
- 3-5 秒科普小视频资产包制作。
- 家长小程序 UI。
- 服务端长期统计报表。
- 医疗筛查结论或近视治疗类文案。
