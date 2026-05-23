# StackChan AI 开放版

本仓库基于官方 StackChan 项目做了面向 AI / MCP / 私有桥接链路的定制修改，目标是让机器人在多种模式之间切换时，尽量保持联网、可控、可语音交互，并且把私有配置从代码中分离出来，方便私有自用和公开分享。

## 免责声明

**这是一个偏实验性质的 AI 开放版本，不保证没有 bug，也不保证长期稳定性、兼容性、可维护性或生产可用性。**

**任何人下载、修改、编译、烧录、分发或运行本项目，均视为已充分理解并自行承担全部风险。**

- 本项目主要用于个人折腾、研究和二次开发参考。
- 本项目包含大量由 AI 参与生成、修改、整理和补全的代码与文档内容。
- 这意味着其中很可能仍然存在逻辑错误、边界条件遗漏、隐藏 bug、错误假设、说明不完整或实现与文档不一致的问题。
- 我目前只在自己的设备、自己的网络和自己的配置上完成了烧录与功能验证。
- 这不代表它在你的设备、你的网络环境、你的语音服务、你的网关、你的 API 提供商或你的部署方式下也一定稳定可用。
- 仓库已经尽量去掉了私有值，但你仍然需要自己准备并维护服务器、鉴权、模型和运行环境。
- 如果你准备公开 fork、继续分发或长期使用，请自行承担测试、维护和安全审查责任。
- 尤其需要注意：刷写、回刷、修改分区、替换配置、异常断电、错误电压、错误接线、错误固件或错误构建产物，都有可能导致设备无法启动、反复重启、功能异常、数据丢失、异常发热，甚至硬件损坏。
- 如果有人因为使用本项目而导致固件损坏、设备变砖、硬件损坏、网络异常、账号封禁、数据丢失或其他任何直接/间接损失，项目维护者不提供任何保证、赔偿、售后或责任承担。
- 任何涉及烧录和硬件改造的操作，都应该由使用者自行评估、自行备份、自行测试，并在能够接受后果的前提下进行。
- 简单说，这个项目是“能跑就继续折腾”的实验版本，不构成任何质量、稳定性、安全性或适用性承诺。

## 项目背景

- 这个仓库不是出自专业固件团队，也不是成熟产品线的正式发布版本。
- 我一开始主要是不熟悉 `ESP-IDF`、固件编译、烧录和刷机恢复这一套流程，但服务器配置和 API Key 管理本身并不是完全从零开始。
- 这个版本基本是靠这几天反复烧录、反复踩坑、反复修问题，才一点点跑通出来的。
- 所以这份 README 更接近“把我实际跑通的路径、踩过的坑和已知限制写清楚”，而不是“权威教程”或“稳定方案说明书”。
- 如果你愿意参考它，请把它当作个人实验记录和可复现的折腾结果，而不是对任何人都适用的标准答案。

## 项目现状

这个版本的目标不是复刻官方所有能力，而是在官方固件基础上，优先实现下面这些方向：

- 机器人在自定义链路下保持联网和可远程控制
- 原生模式、Launcher、AI Agent、小智兼容链路之间可切换
- 自定义 MCP 工具可控制机器人说话、拍照、动作、表情、切模式
- 运行时“切模式”仅表示在几套预置链路之间切换，不表示可以远程改写整套固件关键配置
- 非 `AI.AGENT` 状态下尽量保持桥接器在线
- 原生模式下远程文字气泡显示正常，短文本立即显示，长文本滚动显示
- 私有地址、密钥、令牌不直接写死在仓库追踪文件里

## 模式说明

这个仓库里提到的几个“模式”容易让人误解，这里单独说明一下：

- **小智模式**：这里指的是和官方小智链路对应的兼容模式。
- 小智模式更接近官方侧的对话入口和协议语义，对应仓库里的 `/xiaozhi/ws` 兼容入口。
- **原生模式**：这个名字在当前项目里是历史沿用，不代表“官方原版云端服务”。
- 当前这个仓库里说的“原生模式”，实际更接近**我们自己的自定义桥接 / 自定义网关链路**。
- 也就是说，原生模式下，机器人主要连接的是你自己配置的桥接地址、语音链路和自定义 LLM 网关，而不是官方默认服务。
- 如果只看名字，容易把“小智模式”和“原生模式”理解反了，所以这里特别说明：**小智模式更偏官方对应链路，原生模式反而是本项目的自定义链路。**
- 当前运行时保留的是“模式切换”能力，而不是“远程修改桥接地址、token、网关后立即生效”的完整远程配置能力。

## 已实现功能概览

当前仓库里，已经实际接入并验证过的主要能力包括：

- 自定义语音 WebSocket 主链路
- 机器人控制通道 `/robot-wss`
- 小智兼容语音入口 `/xiaozhi/ws`
- StackChan App / Avatar 代理入口 `/stackChan/ws`
- MCP 控制桥接，可通过 SSE 暴露给外部客户端
- 远程 TTS 播放和屏幕文字气泡显示
- 机器人动作控制
  - 转头
  - 点头
  - 摇头
  - 跳舞
  - 回正 / 朝向控制
  - 重启
- 表情控制
- 拍照与图像解释
- 自定义 LLM 网关对话
- 基于 Groq Whisper API 的语音识别
- 基于 Edge TTS 的语音合成

## 近期实机验证

- 最近一轮回归里，`MCP` 控制链路可正常使用，动作、说话、模式切换都能正常闭环。
- 原生模式与小智兼容模式都已做过实际对话验证，基本对话流程可用。
- 语音处理链路已补过一轮稳定性修复，重点处理了 `listening` 切换后 `AfeAudioProcessor` 未真正启动、`AFE(FEED) is full`、以及 speaking 阶段唤醒词检测导致的额外负担问题。
- 当前 speaking 阶段默认关闭唤醒词检测，目的不是砍掉待机唤醒能力，而是降低播放期间的 CPU/AFE 压力，优先换稳定性和播报顺滑度。
- 不接外部电源线的纯电池场景也做过一轮实测，当前版本相对之前明显稳定一些，但这仍然只代表我手头设备和当前配置下的结果，不构成对所有硬件批次或所有供电状态的保证。

## 大致架构

当前链路可以简单理解为下面这几层：

1. **固件层**
   - 运行在 StackChan / CoreS3 上
   - 负责麦克风采集、扬声器播放、舵机动作、屏幕显示、模式切换、桥接注册
   - 固件内的桥接地址、网关地址、鉴权令牌等通过本地 `private_config.h` 编译进固件
   - 控制通道可以触发有限的模式切换，但关键桥接参数仍以编译期配置为准

2. **Python 桥接层**
   - 文件： [vps_bridge.py](file:///E:/STACKCHAN/StackChan/vps_bridge.py)
   - 提供以下主要入口：
     - `/ws`：主语音 WebSocket
     - `/xiaozhi/ws`：小智兼容 WebSocket
     - `/stackChan/ws`：Avatar 代理
     - `/robot-wss`：机器人控制通道
     - `/sse` 和 `/messages`：MCP SSE 通道
    - `/vision/explain`：图片解释接口
    - `/capture`：机器人拍照直传保存接口
   - 负责串联 STT、LLM、TTS、MCP 工具和机器人控制

3. **模型 / 服务层**
   - STT：默认走 Groq 的 `whisper-large-v3`
   - 视觉：默认走 Groq 的视觉聊天接口
   - 主对话：走你自己的自定义网关
   - TTS：默认走 Edge TTS

4. **外部客户端层**
   - 你自己的 MCP 客户端
   - 兼容的小智链路
   - 可能接入的其他控制端

## 文字版流程图

下面用文字把主要链路串起来，方便快速理解：

### 1. 语音对话主链路

```text
原生模式 / 自定义链路：
用户说话
-> StackChan 麦克风采集音频
-> 固件通过 /ws 把音频发给 Python 桥接器
-> Python 桥接器调用 STT 服务转文字
-> Python 桥接器把文字和上下文发给自定义 LLM 网关
-> LLM 返回回答文本
-> Python 桥接器调用 Edge TTS 合成语音
-> 合成后的音频回传给机器人播放
-> 同时把文本消息发给机器人显示气泡

小智模式 / 官方对应链路：
用户说话
-> StackChan 麦克风采集音频
-> 固件通过小智对应链路与服务端交互
-> 默认语义上更接近官方小智服务，不应简单理解为“也一定发给当前 Python 桥接器”
```

### 2. MCP 控制链路

```text
MCP 客户端
-> 通过 /sse 和 /messages 接入 Python 桥接器
-> 调用拍照 / 动作 / 表情 / 说话 / 模式切换等工具
-> Python 桥接器把控制指令发到 /robot-wss
-> 机器人执行动作、发声、拍照或切换模式
-> 结果再返回给 MCP 客户端
```

### 3. 拍照与视觉理解链路

```text
MCP 或桥接器发起拍照请求
-> 机器人采集图片
-> 机器人通过 /capture 直接把 JPEG 上传到 Python 桥接器
-> Python 桥接器保存图片并通过 /robot-wss 仅回传保存路径 / 状态
-> 如需视觉理解，机器人再把同一张图上传到 /vision/explain 获取结果
-> 返回图片路径或分析结果给 MCP 工具调用方
-> 当前默认不是“拍照后机器人自动自己语音回复一遍”
```

### 3.1 电池模式下的拍照保护

- 当前版本已经针对“插线能拍、纯电池拍照容易重启”的场景加了一层保守保护。
- 这些保护主要只在**没有外部供电**时启用，目标是降低拍照瞬间的电流峰值和资源峰值，而不是追求所有场景都强行拍成功。
- 目前的保护策略包括：
  - 拍照前先停止当前远控 TTS 播放
  - 电池模式下跳过快门音
  - 电池模式下临时降低屏幕背光
  - 电池模式下拍照窗口临时静音扬声器，结束后恢复
  - 电池模式下按电量自动降低 JPEG 质量
  - 电池模式下连续拍照会增加一个短暂冷却时间
  - 电量过低时会直接拒绝拍照，而不是硬顶着去触发更高的掉压风险
- 这意味着在纯电池模式下，`capture_photo` / 视觉解读相比外部供电场景，可能表现为：
  - 响应略慢一点
  - 图片质量略低一点
  - 连拍时会被限一下
  - 低电量时直接返回失败提示
- 这属于**有意行为变化**，目的是把“直接 brownout 重启”尽量改成“可预期地失败或降级”。

### 4. 配置生效链路

```text
你修改 private_config.h
-> 重新编译固件
-> 重新烧录设备
-> 新的桥接地址 / token / 网关配置才会进入机器人

你修改 .env
-> 重启 Python 桥接器
-> 新的桥接器运行参数才会生效
```

## 目录说明

- [firmware](file:///E:/STACKCHAN/StackChan/firmware)：主固件工程，使用 ESP-IDF 构建
- [vps_bridge.py](file:///E:/STACKCHAN/StackChan/vps_bridge.py)：Python 桥接器，核心 AI / MCP / 语音中转逻辑
- [requirements-vps-bridge.txt](file:///E:/STACKCHAN/StackChan/requirements-vps-bridge.txt)：桥接器 Python 依赖
- [.env.example](file:///E:/STACKCHAN/StackChan/.env.example)：桥接器环境变量模板
- [private_config.example.h](file:///E:/STACKCHAN/StackChan/firmware/main/hal/private_config.example.h)：固件私有配置模板
- [flash.ps1](file:///E:/STACKCHAN/StackChan/firmware/flash.ps1)：Windows 下编译并烧录脚本
- [monitor.ps1](file:///E:/STACKCHAN/StackChan/firmware/monitor.ps1)：Windows 下串口监控脚本
- [build-native.ps1](file:///E:/STACKCHAN/StackChan/firmware/build-native.ps1)：Windows 下本地构建脚本
- 说明：这几个脚本会优先读取你本机的 `ESP-IDF` 相关环境变量，并按常见安装路径自动探测；如果你的 Windows 环境和默认结构不同，仍然需要按自己的机器调整

## 运行前提

要跑起来这个版本，你至少需要准备下面这些东西：

- 一台可正常烧录的 StackChan / CoreS3 设备
- Windows 本地 ESP-IDF 构建环境
- Python 3 环境
- `ffmpeg`
- 一个可用的自定义 LLM 网关
  - 当前桥接器默认向 `STACKCHAN_GATEWAY_URL` 发 OpenAI 兼容的 `chat/completions` 风格请求
  - 需要提供 `Bearer` 鉴权，也就是 `STACKCHAN_GATEWAY_KEY`
- 一个可用的 STT 服务
  - 当前默认是 Groq 的 `https://api.groq.com/openai/v1/audio/transcriptions`
  - 默认模型写死为 `whisper-large-v3`
- 一个可用的视觉模型接口
  - 当前默认是 Groq 的 `chat/completions`
  - 默认视觉模型为 `meta-llama/llama-4-scout-17b-16e-instruct`
- 一个机器人与桥接器之间能互相访问的网络环境

## 本地电脑能跑吗

**能跑，但有前提。**

## ⚠️ 历史说明：回滚后的硬件优化排查记录

**这一段最初是一次 Git 回滚后的自用排查备忘，不应简单理解为“当前仓库这 4 个修复都还缺着”。**

当时主要是担心下面几类关键修复在回滚中被一起带掉，所以先把怀疑点记在了 `README` 里，方便下次部署前复查：

1.  **PSRAM 模式相关设置**
    -   早先曾怀疑 `firmware/sdkconfig.defaults` 里缺少显式的 `CONFIG_SPIRAM_MODE_QUAD=y`，可能导致某些环境下出现 WiFi 相关内存分配失败。
    -   当前仓库里已经保留了 `CONFIG_SPIRAM=y`、`CONFIG_SPIRAM_SPEED_80M=y` 等配置，但是否还需要额外显式写死 `QUAD`，仍应按你当下的 `ESP-IDF` 版本和实际构建结果确认。
2.  **WiFi Modem Sleep 延迟修复**
    -   当前代码里已经把 `LOW_POWER` 模式收敛到 `BALANCED`，避免语音链路在调制解调器省电模式下更容易出现丢包和延迟。
3.  **MCP 语音回环修复**
    -   当前 `vps_bridge.py` 已保留音频缓冲清理逻辑，`run_tts_response` 的相关路径里会调用 `clear_session_audio_buffer(session)`，避免把刚播放过的音频残留继续送去识别。
4.  **I2C 兼容性修复 (TryReadRegs / 触摸读取容错)**
    -   当前版本应保留“读取失败时跳过并记录日志”的行为，不能因为单次触摸/I2C 读取失败直接触发崩溃或重启。

更准确地说，这一节现在应该被当作：

- 一次回滚后的历史排查记录
- 以后排查“为什么突然又不稳了”时可参考的怀疑点清单
- 而不是当前仓库的实时缺陷列表


- 如果你只是想在本地电脑上运行 Python 桥接器，这个是可以的。
- [vps_bridge.py](file:///E:/STACKCHAN/StackChan/vps_bridge.py) 默认会监听本机端口，对局域网开放。
- 但机器人必须能够访问到你配置进固件的地址。
- 如果你的机器人和电脑在同一局域网，并且你把固件里的地址改成电脑可访问的 IP / 域名，那么理论上可以本地跑。
- 如果你希望跨公网使用、使用 `wss` / `https`、或者希望稳定一些，仍然更建议放在 VPS、带 TLS 的反向代理后面运行。
- 当前仓库默认思路仍然更偏向“有一台长期在线服务器”的用法，而不是纯本机临时试玩。

## 仓库分支与稳定版本约定

为了避免后续继续开发时把“当前能用的版本”弄丢，当前仓库建议按下面的方式理解和使用：

- `private-working`：当前主要开发分支，后续继续改代码时优先在这里提交
- `stable-2026-05-23`：当前确认可构建、可回到的稳定分支，用来保留一条稳定线
- `stable-audio-ok-2026-05-23`：当前稳定点的标签，用来固定这一次已经确认可用的快照
- `main`：公开仓库原有主线，后续是否整理合并回去可以单独决定，不必每次开发都直接碰它

这三个名字现在都可能暂时指向同一个提交，但它们的用途不同：

- 分支是会随着新提交继续往前移动的“工作线”
- 标签是贴在某个提交上的固定名字，默认不跟着后续提交移动
- “快照”不是单独的 Git 类型，而是“把某一刻代码状态固定下来”的意思，通常用标签或稳定分支来表达

如果你只是想继续开发：

- 进入 `private-working`

如果你只是想回到一版已经确认稳定的代码：

- 优先看 `stable-2026-05-23`
- 如果想精确回到这次确认稳定的提交点，直接使用标签 `stable-audio-ok-2026-05-23`

如果以后再次确认出新的稳定版本，建议重复同样做法：

- 保留一个清楚命名的稳定分支
- 再打一个能说明状态的标签

## 下次从零继续时先检查的 5 项

如果隔了一段时间再回来，不想重新回忆整套流程，建议先只检查下面这 5 项：

1. **先确认你在对的仓库目录里**
   - 当前主仓库目录是 `StackChan`
   - 后续继续开发优先使用 `private-working`
   - 如果只是想回到稳定版本，优先看 `stable-2026-05-23` 或标签 `stable-audio-ok-2026-05-23`

2. **先确认两个本地私有文件还在**
   - `firmware/main/hal/private_config.h`
   - `.env`
   - 这两个文件丢了，当前版本大概率无法直接构建或运行

3. **先确认本机构建环境还可用**
   - Windows 下的 `ESP-IDF`
   - Python 环境
   - `ffmpeg`
   - 之前能跑通的 `export.ps1 + idf.py build` 或 `firmware/build-native.ps1`

4. **先看这几个 README 位置**
   - `仓库分支与稳定版本约定`
   - `配置文件说明`
   - `快速开始`
   - `非常重要：这个版本不能“直接烧录后再改”`

5. **先确认这次改动属于哪一类**
   - 改 `private_config.h`：需要重新编译并重新烧录
   - 改 `.env`：只需要重启 Python 桥接器
   - 只是想验证当前稳定版还能不能构建：优先先跑 `.\firmware\build-native.ps1`

## 配置文件说明

这个版本把配置拆成了两部分：

### 1. 固件编译期配置

模板文件： [private_config.example.h](file:///E:/STACKCHAN/StackChan/firmware/main/hal/private_config.example.h)

你需要复制一份：

```text
firmware/main/hal/private_config.example.h
-> firmware/main/hal/private_config.h
```

你至少需要填写这些值：

- `PRIVATE_BRIDGE_URL`
  - 机器人控制桥接地址，例如 `wss://your-bridge.example.com/robot-wss`
- `PRIVATE_BRIDGE_TOKEN`
  - 机器人注册和控制通道使用的令牌
- `PRIVATE_XIAOZHI_WS_URL`
  - 小智兼容链路入口，例如 `wss://your-bridge.example.com/ws`
- `PRIVATE_CUSTOM_URL`
  - 自定义网关地址
- `PRIVATE_CUSTOM_KEY`
  - 自定义网关鉴权 key
- `PRIVATE_HANDSHAKE_TOKEN`
  - 固件侧握手 / 设备标识相关令牌
- `PRIVATE_DOMAIN_HINT`
  - 用于识别自定义桥接域名的提示值

### 2. Python 桥接运行期配置

模板文件： [.env.example](file:///E:/STACKCHAN/StackChan/.env.example)

你需要复制一份：

```text
.env.example
-> .env
```

桥接器至少要保证这些变量可用：

- `STACKCHAN_GATEWAY_URL`
- `STACKCHAN_GATEWAY_KEY`
- `STACKCHAN_GROQ_API_KEY`
- `STACKCHAN_MODEL_NAME`
- `STACKCHAN_VOICE_WS_TOKEN`
- `STACKCHAN_CONTROL_WS_TOKEN`
  - 机器人控制通道和 `/capture` 的鉴权 token
  - 留空时默认复用 `STACKCHAN_VOICE_WS_TOKEN`

常用补充变量包括：

- `STACKCHAN_DEVICE_ID`
  - 设备标识
  - 如果你本地想沿用官方默认标识，可以在自己的 `.env` 中填 `hi-stack-chan`
- `STACKCHAN_VOICE_NAME`
  - Edge TTS 音色，默认 `zh-CN-YunxiNeural`
- `STACKCHAN_VISION_MODEL_NAME`
  - 视觉模型名
- `STACKCHAN_STT_API_URL`
  - STT 接口地址
- `STACKCHAN_VISION_API_URL`
  - 视觉接口地址
- `STACKCHAN_FFMPEG_PATH`
  - `ffmpeg` 路径
- `STACKCHAN_TTS_FRAME_MS`
  - 控制通道流式 TTS 的 Opus 帧长，默认 `60`
- `STACKCHAN_TTS_OPUS_BITRATE`
  - 控制通道流式 TTS 的 Opus 码率，默认 `48k`
- `STACKCHAN_PHOTO_DIR`
  - `/capture` 保存照片的目录，默认 `photos`

## 非常重要：这个版本不能“直接烧录后再改”

这是当前版本最需要写清楚的一点：

- **固件侧地址、网关、令牌等关键变量，需要先配置好，再编译，再烧录。**
- 也就是说，不能指望“先烧一个通用固件，之后在设备上直接改完就能用”。
- 至少当前仓库默认工作流不是那样设计的。
- 如果你改了 `private_config.h` 里的值，必须重新编译并重新烧录固件。
- 如果你改的是 `.env` 里的桥接器变量，只需要重启 Python 桥接器，不需要重新烧录。

### 关于 `update_config`

- 旧版本里曾经有过类似 `mcp__stackchan_update_config` 的远程配置能力。
- 当前仓库已经将这类“烧录后通过 MCP 远程更新整套配置”的入口下线，不再作为默认工作流保留。
- 原因不是运行时完全不能改任何东西，而是关键桥接地址、token、网关等配置现在依赖 `private_config.h` 和固件内预置值，动态更新既不完整也不稳定，而且很多场景本来就需要重启后才会生效。
- 当前运行时仍然保留的是有限的模式切换能力，例如通过控制通道切换官方 / 自定义模式；但这不等同于“远程下发并更新整套固件配置”。
- 如果你的目标是修改桥接地址、令牌、网关等关键参数，请按“修改 `private_config.h` -> 重新编译 -> 重新烧录”的方式处理。

## 快速开始

### 1. 准备固件私有配置

复制模板：

```powershell
Copy-Item firmware/main/hal/private_config.example.h firmware/main/hal/private_config.h
```

然后编辑本地的 `private_config.h`，填入你的桥接地址、token、网关和握手配置。

### 2. 准备桥接器环境变量

复制模板：

```powershell
Copy-Item .env.example .env
```

然后编辑本地 `.env`，填入你的：

- `STACKCHAN_GATEWAY_URL`
- `STACKCHAN_GATEWAY_KEY`
- `STACKCHAN_GROQ_API_KEY`
- `STACKCHAN_MODEL_NAME`
- `STACKCHAN_VOICE_WS_TOKEN`
- `STACKCHAN_CONTROL_WS_TOKEN`
- 其他你需要的可选变量

### 3. 安装桥接器依赖

```bash
pip install -r requirements-vps-bridge.txt
```

如果系统里没有 `ffmpeg`，还需要额外安装并保证命令行能找到它。

### 4. 启动桥接器

```bash
python vps_bridge.py
```

默认情况下：

- 主 HTTP 服务端口：`3000`
- 机器人控制 WebSocket 端口：`8765`

### 5. 编译 / 烧录固件

Windows 下可以直接使用脚本：

- 这些脚本不是对所有机器都完全通用的“一键安装器”
- 如果你的 `ESP-IDF`、Python 环境或工具链安装位置不同，请先确认本机环境变量或按需修改脚本

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\firmware\flash.ps1
```

如果只想本地构建：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\firmware\build-native.ps1
```

查看串口日志：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\firmware\monitor.ps1
```

## 功能说明

从目前实现来看，这个版本更适合下面几类使用方式：

- 让机器人连接自定义桥接器，而不是只依赖官方链路
- 使用 MCP 从外部控制机器人动作、表情、发声、拍照
- 在原生模式和小智兼容模式之间切换
- 用自定义 LLM 网关驱动语音对话
- 把拍照结果回传，再做视觉解释

## 已知问题

这个版本目前已知至少有下面这些问题，请务必知晓：

### 1. MCP / 控制通道 TTS 回声问题已做防护，但仍建议继续实机观察

当前代码已经做了两层收敛：

- 播报前清空当前语音会话缓冲
- 控制通道 fallback 播放时增加一段 listening guard，避免刚播完立刻把回放重新送去 STT

所以它已经不再是“明知未处理的老问题”。但这仍然依赖具体设备收音、扬声器音量、环境回声和 VAD 行为，是否彻底消失仍然要以你的实机回归为准。

### 2. 稳定性仍然不保证

- 虽然这个版本已经在我的设备上完成过烧录和基本功能验证
- 但网络切换、外部服务异常、TLS、语音链路、第三方 API 限速、长时间运行等场景下，仍然可能出现不可预期问题

### 3. 原生模式下语音识别仍然偏弱

- 当前原生模式下，语音识别整体还能用，但表现不算很稳
- 偶尔会出现幻听，也就是没有清楚说出内容时被误识别出文字
- 也偶尔会出现漏听，或者明明说了话但没有正确识别出来
- 初步怀疑和 VAD / 端点检测一类环节有关，但目前没有继续深挖和修正
- 这个问题目前不算特别严重，不影响基本体验和主要功能，所以暂时只作为已知问题记录

## 安全与开源说明

- 仓库中只保留了配置模板，不保留我的本地私有值
- 真实值应该只保存在你自己的本地：
  - `firmware/main/hal/private_config.h`
  - `.env`
- 这两个文件默认不应提交到 Git

## 不适合谁

如果你属于下面这些情况，我更不建议你直接刷这个版本：

- 想要“下载后直接烧录，立刻稳定使用”的成品体验
- 不接受刷机失败、设备变砖、回刷恢复、反复排错这类风险
- 不想自己准备网关、API Key、桥接器、局域网或服务器环境
- 不打算自己看日志、改配置、重新编译或重启服务
- 希望作者对你的环境问题、第三方服务问题或硬件问题提供一对一兜底支持

## 适合谁使用

如果你属于下面这些情况，这个仓库可能适合你：

- 已经能自己搭 ESP-IDF 构建环境
- 能自己维护 Python 服务
- 知道怎么准备域名、端口、TLS、反向代理或局域网访问
- 能自己处理第三方 API key、限流和异常
- 想把 StackChan 当作一个可改造的 AI 硬件实验平台

如果你只是想要一个“下载后零配置、一步烧录、长期稳定”的方案，那这个仓库目前**不适合**。

## 致谢

本项目基于官方 StackChan 开源项目继续修改而来，感谢 M5Stack 与社区原始贡献者提供的基础工程和生态。

- 官方仓库：[m5stack/StackChan](https://github.com/m5stack/StackChan)
- 官方文档：[StackChan Docs](https://docs.m5stack.com/zh_CN/StackChan)
