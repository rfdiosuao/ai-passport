<p align="right">
  <strong>简体中文</strong> · <a href="identifying-a-production-unit-before-flashing.md">English</a>
</p>

# 刷写前先识别量产机及其出厂布局

本文记录在一台零售版 AI Passport（ESP32-C3、8 MB、原生 USB-Serial-JTAG）上，
从 Windows 侧为刷入自定义固件做准备的实测过程。这台机器到手时运行的是出厂
**AppStore-AI-Passport** 固件，OTA 槽里还装着一个已下载的应用。本文说明只读识别
阶段实际查到了什么，以及为什么出厂分区表会让一次 `0x0` 合并刷写对量产机的破坏面
大于布局文档措辞所暗示的范围。

下文全部来自 `esptool` 5.4.0 在这一台设备上的实测。**未执行任何刷写、编译或擦除。**

## `esptool` 5 把子命令全改名了

v5 去掉了下划线写法，网上几乎所有教程里的命令都会直接失败：

| v4（广为流传） | v5（当前） |
| --- | --- |
| `write_flash` | `write-flash` |
| `read_flash` | `read-flash` |
| `erase_flash` | `erase-flash` |
| `flash_id` | `flash-id` |
| `chip_id` | `chip-id` |
| `merge_bin` | `merge-bin` |

`--version` 也不再是选项（会报 `No such option '--version'`），要改用子命令
`esptool version`。装好后先跑一次 `esptool --help`，以那份列表为准。
`skills/passport-device-test/SKILL.md` 已经要求做这项核对，在照抄任何指南里的命令
之前值得先照做。

## 三条只读命令就能判断这台机器能不能刷

`chip-id`、`flash-id`、`get-security-info` 只需要端口。第三条是最容易被跳过、
却真正决定走向的检查：

```sh
esptool --chip esp32c3 --port COM6 get-security-info
```

本机返回 `Secure Boot: Disabled`、`Flash Encryption: Disabled`，六个密钥块全为
`USER/EMPTY`。正是这个组合才让自编译镜像可启动，同时让整片回读成为明文、可还原的
副本。若任一保护为开启状态，自定义镜像会在启动时被拒绝，回读拿到的也只是密文
——这是两条完全不同的工作流，而它们由一个命令决定。

两个看起来像报错、实际正常的地方：

- `chip-id` 会打印 `WARNING: ESP32-C3 has no chip ID. Reading MAC address instead.`
  ESP32-C3 确实没有 chip ID 寄存器，这行是预期输出，不是失败。
- 不需要按 BOOT。esptool 的默认复位通过原生 USB 接口驱动 RTS 即可进入下载模式，
  该复合设备同时暴露一个 CDC 串口和一个 JTAG 接口。

## 出厂布局不是仓库基线布局

最该先读的是设备自己的分区表。用 `read-flash 0x8000 0x1000` 读出后，按 32 字节
条目解析（magic `0x50AA`）。它与仓库基线完全不同：

| 分区 | 出厂机器 | 仓库基线 |
| --- | --- | --- |
| `nvs` | `0x9000`，24 KB | `0x9000`，24 KB |
| `phy_init` | `0xF000`，4 KB | `0xF000`，4 KB |
| `factory`（app） | `0x10000`，3 MB | `0x10000`，`0x7F0000`（占满剩余空间） |
| `otadata` | `0x310000`，8 KB | — |
| `cardid`（data/nvs） | `0x356000`，16 KB | — |
| `ota_0`（app） | `0x360000`，3 MB | — |
| `store`（data/nvs） | `0x660000`，16 KB | — |
| `recovery`（app/test） | `0x700000`，1 MB | — |

基线里 `factory` 条目预留的是 `0x10000`–`0x800000`，但合并文件只到它最后一个镜像为止
——预留范围不等于写入范围。这个差值决定了出厂各分区究竟哪些字节真的会丢，下一节给出实测。

`firmware-layout.md` 说合并刷写“可能重置 NVS 与 PHY 数据区”，这句话是准确的：写入会完整
覆盖 `nvs` 与 `phy_init`。出厂 `nvs` 区约 85% 非空，所以产品存在里面的内容确实会丢。
这句话没提到的是**分区表本身**也会被替换，连同 bootloader 和应用一起。策略文档也明确写了
无法承诺原始固件可还原。

## `image-info` 能告诉你即将擦掉什么

两个应用分区都能正常解析，其中的身份字段值得作为“刷写前”证据留档：

- `factory`：工程 `AppStore-AI-Passport`，版本 1，ESP-IDF v5.5.3，checksum 与
  validation hash 均有效。
- `ota_0`：工程 `ai-passport-pinball`，版本 `6291c82`，ESP-IDF v5.5.5，checksum 与
  validation hash 均有效。

可见这台机器不是空白板：它带着产品应用出厂，OTA 槽里还有一个已下载的应用。
两者都会被从 `0x0` 的合并刷写抹掉，而它们都不在本仓库里。

## 先量一下合并文件的长度，别假设它有多大

本仓库构建出的产物是 **1,589,824 字节（`0x184240`）**，因此 `write-flash 0x0`
只覆盖 `0x0`–`0x184240`，占 8 MB 的 18%。该偏移之后的所有出厂分区，
字节都保留原样。

| 出厂分区 | 范围 | 字节是否被写 | 新分区表中是否还在 |
| --- | --- | --- | --- |
| `nvs` | `0x9000`–`0xF000` | 全部 | 在 |
| `phy_init` | `0xF000`–`0x10000` | 全部 | 在 |
| `factory` | `0x10000`–`0x310000` | 仅开头一段 | 在 |
| `otadata` | `0x310000`–`0x312000` | 未写 | 不在 |
| `cardid` | `0x356000`–`0x35A000` | 未写 | 不在 |
| `ota_0` | `0x360000`–`0x660000` | 未写 | 不在 |
| `store` | `0x660000`–`0x664000` | 未写 | 不在 |
| `recovery` | `0x700000`–`0x800000` | 未写 | 不在 |

“字节被覆盖”与“布局被替换”是两件独立的事，混为一谈会得出错误的回滚结论：

- **字节层面**：只有 `nvs`、`phy_init` 和应用区开头被重写。`ota_0` 里那个已下载应用、
  OTA 状态以及两个额外数据分区的内容都还在 flash 上。
- **布局层面**：这次写入同时装上了本仓库的三条目分区表，于是上述五个分区全部变得
  **不可寻址**。设备会以空白板的方式启动基线固件——没有 OTA 槽、没有产品数据，
  尽管那些字节仍在 flash 里。

对回滚而言这个区别就是全部意义所在：正因为字节还在，用 8 MB 回读副本
`write-flash 0x0 <backup>.bin` 就能把机器恢复出厂状态。如果合并镜像真把预留范围写满，
这条就不成立了。

## 读 8 MB 之前先确认输出路径

`read-flash` 会先把整段区间读完，传输结束后才打开输出文件。主机 shell 里一次
`mkdir` 失败，就足以让已经完成的 52 秒读取在最后一步全部作废：

```text
FileNotFoundError: [Errno 2] No such file or directory: 'backup/passport-8MB.bin'
```

先建好并确认目录存在，尽量用绝对输出路径，并加 `--no-progress` 让日志可读。
重跑在 921600 波特率下花了 43 秒，所以这次失误的代价大约是一分钟和一段令人困惑的
堆栈——不是数据。

## 整片回读不是前置条件，但很划算

刷写策略明确不要求备份固件，这在给空白板做初始化时是成立的。但对量产机而言，
它永远是唯一的回滚手段；而且在本板上既不慢也不贵：921600 波特率下经
USB-Serial-JTAG 读 8 MB 只要 43–53 秒，因为未开启 Flash 加密，dump 即明文。

不要盲信传输结果，做一次交叉校验：单独读 `0x8000`–`0x9000`，再与整片 dump 同偏移
的字节比对，一秒即可确认这次读取是忠实的。

## 一个值得向上游确认的分区

`recovery` 在 `0x700000`（1 MB）声明为 `app/test`，但不含有效镜像头：前 64 KB 是
重复的 `fe 02` 图案，其中任何位置都找不到 `0xE9` 应用魔法。`firmware-layout.md`
只描述默认布局，因此该分区在量产机上的用途未能确定。在上游确认之前，应视其为已占用。

## 刷完之后怎么看串口

打开这个 CDC 端口并不会复位本板，用 `pyserial` 单独脉冲 RTS 也不会。所以只"打开端口"
的监视器什么都不打印，看起来跟端口选错或设备死了完全一样。真正有效的是**让 `esptool`
去复位、然后立刻打开端口**，同一个脚本里、中间不要 sleep：

```sh
esptool --chip esp32c3 --port COM6 --after hard-reset flash-id   # 紧接着立刻打开 COM6
```

刷写之后端口与复合设备身份都不变（`VID_303A&PID_1001&MI_00`），不需要重新识别。

**同时拉高 DTR 与 RTS 不是复位，而是进下载模式**，而且会一直停在那里：

```text
rst:0x15 (USB_UART_CHIP_RESET),boot:0x6 (DOWNLOAD(USB/UART0))
waiting for download
```

所以某个终端或监视器如果两条线都驱动，就会把板子挡在应用之外，症状同样是"没有日志"。
用 `esptool` 做一次正常复位即可恢复，不要继续来回拨线。

复位之后跑一次 `verify-flash` 是很便宜的持久性检查，不依赖日志：

```sh
esptool --chip esp32c3 --port COM6 --after hard-reset verify-flash 0x0 <merged.bin>
# Verification successful (digest matched).
```

板子跑起来之后，它自己的控制台输出就是端口上最有用的验收证据，因为基线会把每个初始化的
外设都点名。本机日志最终到达 `就绪:Display=1 Button=1 Audio=1 Battery=1`——ES8311 在
I2C `0x18`、CW2017 在 `0x63`、240×320 面板与按键 ADC 分压均已就绪。这些信息，编译结果
和一个刷写哈希都告诉不了你。

## 要点

- 先看本机安装的 `esptool` 自己的帮助；v5 把每个子命令都改了名。
- 动手规划之前先跑 `get-security-info`。Secure Boot 与 Flash Encryption 会把
  “刷自定义固件”变成两个完全不同的项目。
- ESP32-C3 上 `chip-id` 的“no chip ID”是正常输出，答案在 MAC 里。
- 假定仓库基线适用之前，先读设备自己的分区表，并量出合并文件的长度：写入范围到最后一个
  镜像为止，而不是到 `factory` 预留范围的末尾。
- 把“字节被覆盖”与“布局被替换”分开看。合并镜像可以完全不碰某个出厂分区的字节，
  却因为它装上的分区表更短而让那个分区不可寻址。
- `Build firmware` 工作流带 `workflow_dispatch` 触发器，所以 fork 上就能产出合并镜像，
  不必本地安装 ESP-IDF；产物名为 `FoloToy-AI-Passport-full.bin`。
- 这颗 SoC 的 CDC 端口打开时不会复位，单条线脉冲大概也不会。要看启动日志，就用烧录工具
  复位后立刻打开端口。
- DTR 与 RTS 同时拉高意味着进下载模式，不是复位。监视器可能悄悄把板子挡在应用之外。
- 从 `0x0` 重刷很快（1.5 MB 在 921600 波特率下约 5.5 秒），所以一个测试周期的成本主要在
  抓日志，不在写入。
- 用 `image-info` 记录出厂应用分区的身份；工程名、版本与编译时间足以识别它。
- 读 8 MB 之前先建好并验证输出目录，因为文件只在传输结束时才落盘。
- 回读在策略上是可选的，但在量产机上仍然值得做；它是回到出厂固件与数据的唯一途径。
- 在插上目标设备**之前**先做一次主机设备枚举基线，这样之后的 diff 才能证明它出现了。

## 已验证与未验证的范围

- 设备识别、Flash 识别、安全状态、分区表、8 MB 整片回读：均在上文所述设备上完成。
- 固件编译：**已执行**，通过 fork 上的 GitHub Actions 运行 `Build firmware`
  （`workflow_dispatch`），产出 1,589,824 字节的合并镜像，
  `SHA-256 2820b28766cf5967f7d1e2e4783ee503a46e5a1df339ca2d5c8a8779ba5a6e94`。
  本地主机未做任何编译。
- 刷写：**已执行**（获得明确授权后）：从 `0x0` 写入合并镜像，随后 `verify-flash`
  （digest matched）并复读分区表，确认只剩基线的三条条目。
- 启动与板载初始化：**已在实机观测**——应用从 `0x10000` 启动，并报告
  Display/Button/Audio/Battery 全部就绪。
- 屏显效果、音频输出、按键行为、射频性能或休眠电流：**未测量**；日志只证明初始化成功，
  不证明质量。

## 相关

- `docs/development/engineering/firmware-layout.md` —— 本文据以比对的布局与刷写策略。
- `docs/reference/y2lin/serial-screenshot-protocol.md` —— 自定义固件跑起来后会遇到的
  USB-serial-JTAG 驱动陷阱。
- `skills/passport-device-test/SKILL.md` —— 约束刷写写入的授权与验证流程。
