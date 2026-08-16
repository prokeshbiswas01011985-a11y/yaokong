/*
 * rc_protocol.h —— 遥控器协议共享头文件
 *
 * 发射端（Transmitter.ino）与接收端（飞控内 NRF 接收模块）共同引用此文件，
 * 确保帧格式、通道定义、射频参数在所有固件中保持一致。
 *
 * 注意：此文件不包含平台相关代码，可被 Arduino/STM32duino 等不同平台引用。
 */

#ifndef RC_PROTOCOL_H
#define RC_PROTOCOL_H

#include <stdint.h>

/* ========== 帧格式（13 字节，与飞控 rc_protocol.h 逐字节对齐） ==========
 * [0]  = 帧头 0xAA
 * [1..2]  = Roll   (右摇杆 X)  uint16 LE, 1000~2000
 * [3..4]  = Pitch  (右摇杆 Y)  uint16 LE, 1000~2000
 * [5..6]  = Throttle (左摇杆 Y, 已安全重映射)  uint16 LE, 1000~2000
 * [7..8]  = Yaw    (左摇杆 X)  uint16 LE, 1000~2000
 * [9..10] = AUX1   (解锁开关: >1500=armed)  uint16 LE
 * [11..12]= AUX2   (预留, 固定 1500)  uint16 LE
 * 协议无校验和，抗错靠 NRF24L01 硬件 CRC（2 字节）+ 帧头粗筛
 */

#define FRAME_LEN       13
#define FRAME_HEAD      0xAA
#define CHANNEL_COUNT   6

/* ---------- 通道索引（帧内顺序，与飞控 rc.c 直接映射） ---------- */
#define CH_ROLL         0   /* 右摇杆 X → roll（横滚） */
#define CH_PITCH        1   /* 右摇杆 Y → pitch（俯仰） */
#define CH_THROTTLE     2   /* 左摇杆 Y → throttle（油门） */
#define CH_YAW          3   /* 左摇杆 X → yaw（偏航） */
#define CH_AUX1         4   /* 解锁开关 */
#define CH_AUX2         5   /* 预留 */

/* ---------- 通道值范围（微秒约定） ---------- */
#define CH_MIN          1000
#define CH_MID          1500
#define CH_MAX          2000

/* ---------- AUX 通道值 ---------- */
#define AUX_ARMED       2000
#define AUX_DISARM      1000
#define AUX_MID         1500

/* ---------- 射频参数（收发两端必须一致） ---------- */
static const uint8_t RF_ADDR[5] = { 0xE7, 0xE7, 0xE7, 0xE7, 0xE7 };
#define RF_CHANNEL          100   /* 避开 WiFi 常用信道 */
#define RF_DATA_RATE        1     /* 1Mbps, RF24_1MBPS */
#define RF_PA_LEVEL         3     /* 0dBm, RF24_PA_MAX */
#define RF_CRC_LENGTH       2     /* 2 字节 CRC, RF24_CRC_16 */
#define RF_PAYLOAD_SIZE     FRAME_LEN

/* ---------- 发送周期 ---------- */
#define FRAME_PERIOD_MS     20    /* 50Hz */

/* ---------- 失控保护 ---------- */
#define FAILSAFE_TIMEOUT_MS 500   /* 25 帧无合法帧 → 保护态 */

/* ---------- 通道值合法性校验 ---------- */
#define CH_VALID_MIN        950   /* 容差 ±50，用于接收端脏帧防护 */
#define CH_VALID_MAX        2050

#endif /* RC_PROTOCOL_H */