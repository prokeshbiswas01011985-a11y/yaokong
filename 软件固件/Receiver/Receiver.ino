/*
 * Receiver.ino —— NRF24L01 接收参考实现（Arduino/STM32duino 风格，RF24 库）
 *
 * ⚠️ 定位说明：本项目飞控（STM32F103C8T6，HAL 工程）实际运行的接收模块是
 *    飞控工程内的 bsp_nrf24l01.c + rc.c（接线 SPI2：PB12=CSN、PB13/14/15，
 *    PA15=CE），不是本文件。本文件是同协议的 Arduino 风格参考实现，
 *    供移植/比对/其他 MCU 复用，与发射端共用 rc_protocol.h。
 * 参考接线（STM32duino Blue Pill，与飞控板不同）：
 *   NRF24L01: SCK→PA5  MISO→PA6  MOSI→PA7（SPI1 默认） CE→PB0  CSN→PA4
 * 协议：13 字节帧，6 通道（Roll/Pitch/Throttle/Yaw + AUX1 + AUX2），
 *       无应用层校验和，抗错靠 NRF24L01 硬件 CRC（2 字节）+ 帧头粗筛 + 脏帧范围校验。
 * 失控保护：500ms 无合法帧 → 油门最低、其余回中。
 * 版本：v0.3（由首版 NRF转舵机PWM 错误架构重构而来）
 */

#include <SPI.h>
#include <RF24.h>
#include "rc_protocol.h"

/* ---------- 硬件引脚 ---------- */
#define PIN_NRF_CE   PB0
#define PIN_NRF_CSN  PA4

RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);

/* ---------- 通道数据（供飞控主循环读取） ---------- */
static uint16_t rcChannels[CHANNEL_COUNT];
static uint32_t lastRxMs = 0;
static bool     failsafe = true;   /* 上电即进入保护态 */

/* ============ 帧解析（13 字节，与 rc_protocol.h 对齐） ============
 * [0]     = 帧头 0xAA
 * [1..2]  = Roll    [3..4]  = Pitch
 * [5..6]  = Throttle [7..8] = Yaw
 * [9..10] = AUX1    [11..12]= AUX2
 * 全部 uint16 LE，无校验和 */

bool parseFrame(const uint8_t *f, uint16_t channels[CHANNEL_COUNT]) {
  if (f[0] != FRAME_HEAD) return false;

  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    uint16_t v = f[1 + i * 2] | ((uint16_t)f[2 + i * 2] << 8);
    if (v < CH_VALID_MIN || v > CH_VALID_MAX) return false;  /* 脏帧防护 */
    channels[i] = v;
  }
  return true;
}

/* ============ 失控保护 ============ */

void applyFailsafe() {
  /* 油门通道（CH_THROTTLE）输出最低值，其余通道回中 */
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    rcChannels[i] = (i == CH_THROTTLE) ? CH_MIN : CH_MID;
  }
}

/* ============ 公开 API（供飞控主循环调用） ============ */

/* 获取最新通道值。调用前建议先检查 rc_isConnected() 确认链路状态。
 * channels 数组长度至少为 CHANNEL_COUNT（6）。
 * 索引：CH_ROLL=0, CH_PITCH=1, CH_THROTTLE=2, CH_YAW=3, CH_AUX1=4, CH_AUX2=5 */
void rc_getChannels(uint16_t channels[CHANNEL_COUNT]) {
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    channels[i] = rcChannels[i];
  }
}

/* 返回 true 表示当前链路正常（500ms 内收到过合法帧） */
bool rc_isConnected() {
  return !failsafe;
}

/* 初始化 NRF 接收模块。飞控 setup() 中调用一次。 */
void rc_setup() {
  Serial.begin(115200);   /* USART1：PA9=TX PA10=RX */
  delay(100);

  applyFailsafe();   /* 先填充安全值 */

  if (!radio.begin()) {
    Serial.println(F("[RX] ERR: NRF24L01 not found, check wiring/power"));
    while (true) delay(1000);   /* 保持安全值，不输出带病数据 */
  }
  radio.setChannel(RF_CHANNEL);
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_MAX);
  radio.setCRCLength(RF24_CRC_16);     /* 必须与发射端一致，否则静默丢包 */
  radio.setAutoAck(false);
  radio.setPayloadSize(FRAME_LEN);
  radio.openReadingPipe(1, RF_ADDR);
  radio.startListening();

  Serial.println(F("[RX] ready, waiting for frames"));
}

/* 飞控 loop() 中每周期调用一次，轮询 NRF 并更新通道值。
 * 非阻塞：无数据时立即返回，不影响飞控主循环时序。 */
void rc_update() {
  uint8_t buf[FRAME_LEN];

  while (radio.available()) {
    radio.read(buf, FRAME_LEN);
    uint16_t channels[CHANNEL_COUNT];
    if (parseFrame(buf, channels)) {
      lastRxMs = millis();
      if (failsafe) {
        failsafe = false;
        Serial.println(F("[RX] link established"));
      }
      /* 写入全局通道数组，供飞控通过 rc_getChannels() 读取 */
      for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
        rcChannels[i] = channels[i];
      }
    }
    /* 脏帧静默丢弃，不更新通道值 */
  }

  /* 失控保护：超时未收到合法帧 → 切保护态 */
  if (!failsafe && (millis() - lastRxMs >= FAILSAFE_TIMEOUT_MS)) {
    failsafe = true;
    applyFailsafe();
    Serial.println(F("[RX] link lost -> failsafe"));
  }
}

/* ============ 飞控集成示例（供参考，实际使用时删除） ============
 *
 * void setup() {
 *   rc_setup();
 *   // 飞控其他初始化 ...
 * }
 *
 * void loop() {
 *   rc_update();                    // 每周期轮询 NRF
 *   uint16_t ch[CHANNEL_COUNT];
 *   rc_getChannels(ch);             // 读取最新通道值
 *
 *   if (rc_isConnected()) {
 *     // 正常模式：按 ch[CH_ROLL] / ch[CH_PITCH] / ch[CH_THROTTLE] / ch[CH_YAW] 控制
 *     // ch[CH_AUX1] > 1500 表示已解锁
 *   } else {
 *     // 失控保护：ch 已自动填充安全值，飞控执行安全策略
 *   }
 * }
 */