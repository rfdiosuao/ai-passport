[English](summon-voice.md)

# SUMMON USB 语音应用

本分支使用独立语音界面替换硬件测试菜单，复用 BSP 和中文字库。应用入口是 `main/summon_voice.c`，电脑端配套程序为 [gateway.passport](https://github.com/rfdiosuao/summon-protocol/blob/main/gateway/passport.py)。

USB 连接 Windows 网关。确定开始录音，说话后的静音自动结束，也可再按确定立即提交；最长录音八秒。双击确定取消或返回。上下键以十为步长调音量，长按上键打开手机配网页，地址显示在设备上。USB 模式可以只保存 Agent 铭牌，不填 Wi-Fi；网页也有音量滑块。配置保存到 NVS；升级时不要用完整镜像覆盖需要保留的 NVS。

媒体使用有界 `SUMMON1 ` JSONL 帧，与 Hub 控制协议分离。音频为 16 kHz、16 bit、单声道，每块最多 1024 字节并进行 Base64 编码。录音含 turn 和 seq，电脑端拒绝缺块或乱序。播放队列容量十六块，预缓冲十二块；`play.ack` 仅确认入队，`play.done` 在音频任务结束后返回字节数、耗时和断粮次数。取消会丢弃待播放数据。电脑端批量读 USB，避免逐字节读取开销。

按键回调只入队，音频运行于独立任务，LVGL 更新持 BSP 锁。固件不包含 API key。Windows 桥将音频上传到配置的语音服务，远端 Agent 返回经授权的电脑动作与结果。目前验收使用服务器测试 Agent，并非用户原来的本地 Codex；两台电脑路由和 Codex 集成属于后续工作。

验证包括 CI 固件与主机检查、分区表一致时只刷应用区、实体麦克风转写、电脑真实命令及云端回执、缓冲播放测试。一段 3735 ms 的音频报告驱动耗时 3653 ms、断粮零次。驱动数据不能代替听感，人工试听与手机配网分别验收。
