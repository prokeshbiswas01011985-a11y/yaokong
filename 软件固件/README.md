# 固件代码说明

本目录包含遥控器系统的两份固件源码（Arduino IDE 工程），协议规范见[固件设计规范](固件设计规范.md)。

| 目录 | 固件 | 硬件平台 | 功能 |
|---|---|---|---|
| `Transmitter/` | 发射端 v0.2 | Arduino Nano ESP32（板载 U5） | 双摇杆采样 → 滤波 → 校准映射 → 杆量解锁手势 → 50Hz NRF 发送 |
| `Receiver/` | 首版已废弃 ⚠️ | — | 架构理解有误（接收端即飞控本身），待按飞控 rc.c 接口重写为 NRF 接收模块 |

## 1. 开发环境搭建（Arduino IDE）

1. Arduino IDE 2.x → Boards Manager 安装两个核心：
   - **Arduino ESP32 Boards**（含 `Arduino Nano ESP32`，发射端用）
   - **STM32 MCU based boards**（含 `Generic STM32F1 series`，接收端用）
2. Libraries 安装 **RF24**（nRF24/RF24，作者 TMRh20）。
3. 发射端调试直接用 USB-C 虚拟串口（115200 8N1）；接收端调试接 USART1（PA9=TX、PA10=RX）。

## 2. 发射端烧录

- 开发板选择 **Arduino Nano ESP32**，USB 连接后直接上传。
- 引脚已按网表核实，无需改动：摇杆 A1~A4，NRF 的 CE=D9、CSN=D10，SPI 走默认 D13/D11/D12。
- ⚠️ 本板 NRF 插座为 **Pin1=3.3V、Pin2=GND**，与市售常见模块相反，插模块前用万用表核对（见[装配指南](../装配指南.md)）。

### 摇杆校准流程（[整机校准](../整机校准.md) 的固件实现）

USB 串口助手发送以下字符（任意时刻可进入）：

| 命令 | 操作 |
|---|---|
| `C` | 进入校准模式（暂停发送，回显原始 ADC） |
| `M` | 两摇杆静置居中后发送，采集中点 |
| `E` | 把某个摇杆推满一个方向并保持，发送以合并 min/max；四个方向各做一次 |
| `S` | 保存校准数据到 EEPROM 并重启 |
| `X` | 放弃退出 |

未校准时使用默认映射（0/512/1023），仍可正常发送。

## 3. 接收端（即飞控，无需单独烧录）

接收端就是飞控本身（STM32F103C8T6），NRF 接收模块在飞控工程内实现（`bsp_nrf24l01.c` + `rc.c`），接线已在飞控板上固化：

| NRF24L01 引脚 | 接飞控 | 说明 |
|---|---|---|
| SCK / MISO / MOSI | PB13 / PB14 / PB15 | SPI2，3MHz |
| CE / CSN | PA15 / PB12 | GPIO，JTAG 已释放 |
| IRQ | 不接 | 飞控固件为轮询方式 |
| VCC / GND | 3.3V / GND | ⚠️ 勿接 5V |

（早期设想的独立 NRF转PWM 接收板方案已废弃，`Receiver/` 目录仅存档。）

## 4. 射频参数（收发两端必须一致）

| 项目 | 值 |
|---|---|
| 地址 | `E7 E7 E7 E7 E7` |
| 信道 | 100 |
| 数据率 | 1Mbps |
| 功率 | 0dBm（RF24_PA_MAX，裸模块） |
| 帧长 | 13 字节（帧头 `AA` 1B + Roll/Pitch/Throttle/Yaw/AUX1/AUX2 六通道 uint16 小端，无校验和，armed 走 AUX1>1500） |
| CRC | NRF24L01 硬件 2 字节 CRC（发射端 `RF24_CRC_16`，飞控 CONFIG CRCO=1，不一致会静默丢包） |

修改任一端参数时，另一端必须同步修改（发射端在 `Transmitter.ino` 顶部常量区，飞控在 `rc_protocol.h` / `bsp_nrf24l01.c`）。接收端即飞控本身（STM32F103C8T6 + NRF24L01，SPI2：PB12=CSN/PB13/14/15，PA15=CE），无独立接收板；失控保护在飞控 `rc.c` 实现（500ms 无帧 → signal_ok=0，自动停电机）。

## 5. 失控保护行为（飞控 `rc.c` 实现）

- 上电默认 `signal_ok=0`，电机不动；
- 连续 500ms 无合法帧 → `signal_ok=0` + `armed=0`，飞控内环自动 `pid_reset_all()` + `motor_stop_all()`；
- 恢复收帧立即切回实时数据。

## 6. 版本与未实现项

- 已实现：固件设计规范 v0.1/v0.2 全部内容；发射端协议已切换为飞控 `rc_protocol.h` 新格式（13 字节 1 帧头+6 通道，CRC16，油门中位归零重映射）；
- ⚠️ 协议切换后摇杆校准语义变化，需重新走 C→M→E→S 校准流程；
- 未实现（v0.3）：静止检测降帧率（10Hz）；发射端状态指示受硬件限制（无 LED/蜂鸣器）无法实现。
