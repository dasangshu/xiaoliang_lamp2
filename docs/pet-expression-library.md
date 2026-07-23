# 宠物表情动作库说明

本文档用于生成一套新的宠物形象 MJPEG 表情库。当前系统按“皮肤目录 + 统一文件名”读取动画，例如当前皮肤为 `jinglingshu` 且收到 `happy` 时，只允许播放 `/sdcard/style/jinglingshu/happy.mjpeg`。旧路径 `/sdcard/<文件名>.mjpeg` 已废弃，不再作为资源路径使用。

## 生成规范

- 每套皮肤必须放在独立目录中，例如 `/sdcard/style/jinglingshu/`。
- 不同皮肤的文件名必须统一，例如每套皮肤都使用 `happy.mjpeg`、`sad.mjpeg`。
- 建议画面尺寸保持 `480x800`，与当前屏幕和 MJPEG 播放链路一致。
- 主体建议居中，占画面高度约 `65% ~ 80%`。
- 背景尽量干净，不要高频闪烁，避免屏幕雪花和视觉疲劳。
- 动作循环要自然，首尾帧要能无缝衔接。
- `talk.mjpeg`、`listen.mjpeg`、`idle.mjpeg` 是系统状态动画，不建议做成强烈情绪。
- `laughing` 不要过度夸张，当前使用频率可能较高，建议设计得可爱但不刺眼。

## 系统状态动画

| 文件名 | 用途 | 动作描述 | 设计建议 |
| --- | --- | --- | --- |
| `loading.mjpeg` | 启动、加载中 | 宠物等待系统启动，可以轻微眨眼、呼吸、左右张望 | 不要太激烈，避免启动时占资源过高 |
| `idle.mjpeg` | 默认待机 | 宠物安静陪伴，轻微呼吸、眨眼、尾巴或耳朵小幅摆动 | 最重要的常驻形象，要稳定、耐看 |
| `listen.mjpeg` | 聆听用户说话 | 宠物身体前倾、耳朵竖起、眼神专注 | 表达“我在听”，不要抢戏 |
| `talk.mjpeg` | AI 正在说话 | 宠物嘴巴轻动、身体有节奏点头 | 适合长时间播放，动作要流畅 |
| `bye.mjpeg` | 坐姿提醒、轻度警示 | 宠物挥手、提醒、轻轻皱眉或认真看着用户 | 用于纠正坐姿，不要恐吓儿童 |

## AI 情绪表情

| 表情名 | 文件名 | 含义 | 动作描述 | 适用语境 |
| --- | --- | --- | --- | --- |
| `neutral` | `neutral.mjpeg` | 中性、平静 | 平稳站立或坐着，轻微眨眼 | 无明确情绪、兜底 |
| `happy` | `happy.mjpeg` | 开心 | 微笑、轻快点头、身体轻轻弹动 | 肯定、鼓励、答对问题 |
| `laughing` | `laughing.mjpeg` | 大笑 | 笑得更明显，但不要张牙舞爪；可以小幅前后晃动 | 笑话、轻松互动 |
| `funny` | `funny.mjpeg` | 搞怪 | 歪头、吐舌、夸张眨眼、小幅摆手 | 玩笑、调皮回答 |
| `sad` | `sad.mjpeg` | 难过 | 低头、眼神下垂、动作变慢 | 遗憾、安慰、失败反馈 |
| `angry` | `angry.mjpeg` | 生气 | 眉头收紧、叉腰、轻微跺脚 | 轻度不满，避免吓人 |
| `crying` | `crying.mjpeg` | 哭泣 | 眼泪、低头、抽泣式小动作 | 伤心表达、夸张卖萌 |
| `loving` | `loving.mjpeg` | 喜爱、亲近 | 眼神温柔、抱心、靠近用户 | 表扬、亲密陪伴 |
| `embarrassed` | `embarrassed.mjpeg` | 害羞、尴尬 | 脸红、挠头、眼神躲闪 | 被夸奖、说错话 |
| `surprised` | `surprised.mjpeg` | 惊讶 | 眼睛睁大、身体后仰、耳朵弹起 | 突然发现、惊喜 |
| `shocked` | `shocked.mjpeg` | 震惊 | 比 surprised 更强烈，短暂停顿后夸张反应 | 非常意外的内容 |
| `thinking` | `thinking.mjpeg` | 思考 | 托腮、眼睛上看、慢慢点头 | 推理、等待答案 |
| `winking` | `winking.mjpeg` | 眨眼、俏皮 | 单眼眨眼、轻轻挥手 | 小提示、轻松确认 |
| `cool` | `cool.mjpeg` | 耍酷 | 戴墨镜感、抬下巴、摆 pose | 自信、完成任务 |
| `relaxed` | `relaxed.mjpeg` | 放松 | 舒展身体、慢呼吸、眯眼 | 休息、缓和情绪 |
| `delicious` | `delicious.mjpeg` | 好吃、满足 | 舔嘴、开心晃动、抱着食物也可以 | 美食、奖励场景 |
| `kissy` | `kissy.mjpeg` | 亲亲、撒娇 | 飞吻、靠近镜头、开心眨眼 | 亲密互动，注意克制 |
| `confident` | `confident.mjpeg` | 自信 | 挺胸、点头、坚定眼神 | 鼓励、任务完成 |
| `sleepy` | `sleepy.mjpeg` | 困倦 | 打哈欠、眼皮下垂、身体轻晃 | 夜间、疲劳提醒 |
| `silly` | `silly.mjpeg` | 傻萌 | 歪头、吐舌、转圈或小失误 | 轻松娱乐 |
| `confused` | `confused.mjpeg` | 困惑 | 歪头、挠头、眼神左右看 | 没听懂、需要澄清 |

## 当前 SD 卡实际文件注意事项

当前默认皮肤目录为 `/sdcard/style/jinglingshu`。该目录建议包含：

- `happy.mjpeg`
- `laughing.mjpeg`
- `funny.mjpeg`
- `sad.mjpeg`
- `crying.mjpeg`
- `loving.mjpeg`
- `surprised.mjpeg`
- `shocked.mjpeg`
- `thinking.mjpeg`
- `winking.mjpeg`
- `cool.mjpeg`
- `relaxed.mjpeg`
- `delicious.mjpeg`
- `kissy.mjpeg`
- `confident.mjpeg`
- `sleepy.mjpeg`
- `confuesed.mjpeg`

其中 `confuesed.mjpeg` 是旧拼写。新生成资产时建议使用正确文件名 `confused.mjpeg`。

日志中暂未看到这些 MJPEG 文件，但内置 icon 支持对应表情：

- `neutral`
- `angry`
- `embarrassed`
- `silly`

如果要做到全 MJPEG 宠物形象库，建议补齐：

- `neutral.mjpeg`
- `angry.mjpeg`
- `embarrassed.mjpeg`
- `silly.mjpeg`
- `confused.mjpeg`

## 扩展动作

当前 SD 卡还存在一些非标准但可复用的动作：

| 文件名 | 建议用途 | 动作描述 |
| --- | --- | --- |
| `baojiancao.mjpeg` | 健康操、久坐活动 | 宠物做简单拉伸、转肩、举手 |
| `glass.mjpeg` | 学习、专注 | 宠物戴眼镜、认真阅读 |
| `jingya.mjpeg` | 惊讶别名 | 可映射为 `surprised` 或 `shocked` |
| `xiaoliang.mjpeg` | 品牌默认形象 | 可作为主角介绍或特殊欢迎 |
| `ls.mjpeg` | 聆听别名 | 可映射为 `listen.mjpeg` |

## 给生成模型的统一提示词模板

生成每个动作时可以使用如下描述：

```text
生成一个 480x800 竖屏 MJPEG 动画角色。角色是同一只可爱的宠物精灵，保持一致的外形、颜色、五官和比例。背景干净简洁，主体居中，占画面高度约 70%。动作需要循环自然，首尾帧可无缝衔接，动作幅度适中，不要频闪，不要复杂背景，不要出现文字。

当前表情：{表情名}
动作要求：{动作描述}
情绪强度：中等，适合儿童学习陪伴设备长时间观看。
风格：温暖、聪明、友好、有陪伴感，不要恐怖、不要攻击性、不要过度夸张。
```

## 推荐优先生成顺序

1. `idle.mjpeg`
2. `listen.mjpeg`
3. `talk.mjpeg`
4. `happy.mjpeg`
5. `sad.mjpeg`
6. `surprised.mjpeg`
7. `thinking.mjpeg`
8. `cool.mjpeg`
9. `bye.mjpeg`
10. `confused.mjpeg`

这 10 个覆盖默认陪伴、对话、情绪反馈和坐姿提醒，是最小可用宠物动作库。
