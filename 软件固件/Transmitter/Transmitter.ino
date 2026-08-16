/*
 * Transmitter.ino —— 双摇杆 2.4GHz 遥控器·发射端固件
 *
 * 硬件：Arduino Nano ESP32（U5，ABX00092）+ NRF24L01 + 双摇杆
 * 引脚映射已对照 EasyEDA 原理图核实：
 *   JOY_LX→A1(U5.A11)  JOY_LY→A2(U5.A10)  JOY_RX→A3(U5.A9)  JOY_RY→A4(U5.A8)
 *   NRF: CE→D9(U5.B12) CSN→D10(U5.B13) SPI 走默认 D13/D11/D12
 * 协议与飞控 rc_protocol.h 对齐：13 字节 = 帧头 1B + 6 通道×2B LE，无校验，
 * armed 经 AUX1 通道传输（>1500=解锁）
 * 版本：v0.2（采样+滤波+校准+50Hz 发送+USB 调试；静止降帧率 v0.3 未实现）
 */

#include <SPI.h>
#include <RF24.h>
#include <EEPROM.h>
#include "../rc_protocol.h"   /* 共享协议常量：FRAME_LEN, FRAME_HEAD, CHANNEL_COUNT, RF_ADDR 等 */

/* ---------- 硬件引脚 ---------- */
#define PIN_JOY_LX A1
#define PIN_JOY_LY A2
#define PIN_JOY_RX A3
#define PIN_JOY_RY A4
#define PIN_NRF_CE 9
#define PIN_NRF_CSN 10

/* ---------- 射频与帧格式（来自 rc_protocol.h，此处不再重复定义） ----------
 * 共享常量：FRAME_LEN, FRAME_HEAD, CHANNEL_COUNT, AUX_ARMED/DISARM/MID,
 *          RF_CHANNEL, RF_ADDR, FRAME_PERIOD_MS 等
 * 帧格式注释见 rc_protocol.h 顶部 */

/* ---------- 摇杆→通道索引（AETR） ---------- */
#define IDX_ROLL 0     /* 右摇杆 X */
#define IDX_PITCH 1    /* 右摇杆 Y */
#define IDX_YAW 2      /* 左摇杆 X */
#define IDX_THROTTLE 3 /* 左摇杆 Y */

/* ---------- 杆量解锁手势 ----------
 * 偏航杆推右满 + 油门收到底，保持 2 秒切换解锁状态。
 * 解锁时误触会立即上锁（优先安全）。 */
#define ARM_YAW_MIN 1800
#define ARM_THR_MAX 1100
#define ARM_HOLD_MS 2000

/* ---------- 采样与滤波参数 ---------- */
#define ADC_MAX 1023 /* 10bit */
#define DEAD_BAND 20 /* 中点死区 ±2% ≈ 1023*0.02 */
#define WIN_DEPTH 4  /* 滑动窗口深度 */

/* ---------- 校准数据（EEPROM 存储） ---------- */
#define CAL_MAGIC 0xCA11
struct Calib {
  uint16_t magic;
  uint16_t mn[4];
  uint16_t mid[4];
  uint16_t mx[4];
};
Calib cal;

/* 采样顺序即 AETR 通道序：us[0]=右X(roll) us[1]=右Y(pitch)
 * us[2]=左X(yaw) us[3]=左Y(throttle)，校准数据按此索引存储 */
const uint8_t joyPins[4] = { PIN_JOY_RX, PIN_JOY_RY, PIN_JOY_LX, PIN_JOY_LY };

RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);

uint16_t winBuf[4][WIN_DEPTH];
uint8_t winIdx = 0;
bool calibMode = false;
uint32_t lastFrameMs = 0;
uint32_t lastPrintMs = 0;
uint8_t frameCount = 0;
uint32_t armGestureMs = 0;  /* 手势持续时间累计 */
uint32_t armCooldownMs = 0; /* 手势结束后的冷却，防止杆回中过程中反复触发 */
bool armed = false;

/* ============ 校准数据读写 ============ */

void calibLoadDefaults() {
  cal.magic = CAL_MAGIC;
  for (uint8_t i = 0; i < 4; i++) {
    cal.mn[i] = 0;
    cal.mid[i] = ADC_MAX / 2;
    cal.mx[i] = ADC_MAX;
  }
}

void calibLoad() {
  EEPROM.get(0, cal);
  if (cal.magic != CAL_MAGIC) {
    calibLoadDefaults(); /* 首次使用或数据损坏：用默认值，不影响发送 */
  }
}

void calibSave() {
  EEPROM.put(0, cal);
  EEPROM.commit();
}

/* ============ ADC 采样与滤波 ============ */

/* 单轴连续采 8 次，去掉最大最小后取平均 */
uint16_t sampleAxis(uint8_t pin) {
  uint32_t sum = 0;
  uint16_t mn = ADC_MAX, mx = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint16_t v = analogRead(pin);
    sum += v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  return (uint16_t)((sum - mn - mx) / 6);
}

/* 滑动窗口（深度 4）平滑；每帧对 4 通道写入同一列后统一推进 */
uint16_t windowFilter(uint8_t ch, uint16_t v) {
  winBuf[ch][winIdx] = v;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < WIN_DEPTH; i++) sum += winBuf[ch][i];
  return (uint16_t)(sum / WIN_DEPTH);
}

/* 校准映射：min→1000 / mid→1500 / max→2000，含中点死区 */
uint16_t mapToUs(uint8_t ch, uint16_t v) {
  int16_t md = cal.mid[ch];
  if (abs((int16_t)v - md) <= DEAD_BAND) v = (uint16_t)md;

  long out;
  if (v <= md) {
    int16_t span = md - cal.mn[ch];
    out = 1000L + (long)((int16_t)v - cal.mn[ch]) * 500L / (span > 0 ? span : 1);
  } else {
    int16_t span = cal.mx[ch] - md;
    out = 1500L + (long)((int16_t)v - md) * 500L / (span > 0 ? span : 1);
  }
  if (out < 1000) out = 1000;
  if (out > 2000) out = 2000;
  return (uint16_t)out;
}

/* ============ 协议打包 ============ */

void buildFrame(uint8_t *f, const uint16_t us[4], bool isArmed) {
  uint16_t ch[CHANNEL_COUNT];
  ch[0] = us[IDX_ROLL];
  ch[1] = us[IDX_PITCH];
  ch[2] = remapThrottle(us[IDX_THROTTLE]); /* 中位归零安全映射 */
  ch[3] = us[IDX_YAW];
  ch[4] = isArmed ? AUX_ARMED : AUX_DISARM; /* AUX1 = 解锁开关 */
  ch[5] = AUX_MID;                          /* AUX2 预留 */

  f[0] = FRAME_HEAD;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    f[1 + i * 2] = ch[i] & 0xFF; /* 低字节在前 */
    f[1 + i * 2 + 1] = ch[i] >> 8;
  }
}

/* 自回中摇杆安全映射：中位及以下 → 1000（松手=零油门），
 * 上半行程 1500~2000 线性拉伸到 1000~2000，防止解锁即半油门 */
uint16_t remapThrottle(uint16_t us) {
  if (us <= 1500) return 1000;
  long t = 1000L + (long)(us - 1500) * 2L;
  if (t > 2000) t = 2000;
  return (uint16_t)t;
}

/* ============ 杆量解锁手势检测 ============ */

void armPoll(const uint16_t us[4], uint32_t now) {
  bool gesture = (us[IDX_YAW] >= ARM_YAW_MIN) && (us[IDX_THROTTLE] <= ARM_THR_MAX);
  if (gesture) {
    if (armGestureMs == 0) armGestureMs = now;
    if (now - armGestureMs >= ARM_HOLD_MS && now - armCooldownMs >= ARM_HOLD_MS) {
      armed = !armed;
      armCooldownMs = now; /* 状态切换后需松杆再计时 */
      Serial.printf("[TX] %s\n", armed ? "ARMED" : "DISARMED");
    }
  } else {
    armGestureMs = 0;
  }
}

/* ============ 校准模式（USB 串口命令触发） ============
 * 任意时刻发送字符 C 进入校准模式：
 *   M —— 两摇杆静置居中，采集中点
 *   E —— 把摇杆推满某方向并保持，合并当前 min/max（四个方向各做一次）
 *   S —— 保存并重启
 *   X —— 放弃退出
 */
void enterCalib() {
  calibMode = true;
  Serial.println(F("[CAL] calibration mode: M=mid  E=extreme  S=save+reboot  X=exit"));
}

void calibHandle(char c) {
  switch (c) {
    case 'M':
      for (uint8_t i = 0; i < 4; i++) {
        uint32_t sum = 0;
        for (uint8_t k = 0; k < 8; k++) sum += sampleAxis(joyPins[i]);
        cal.mid[i] = sum / 8;
      }
      Serial.printf("[CAL] mid: %u %u %u %u\n", cal.mid[0], cal.mid[1], cal.mid[2], cal.mid[3]);
      break;
    case 'E':
      for (uint8_t i = 0; i < 4; i++) {
        uint16_t v = sampleAxis(joyPins[i]);
        if (v < cal.mn[i]) cal.mn[i] = v;
        if (v > cal.mx[i]) cal.mx[i] = v;
      }
      Serial.printf("[CAL] min: %u %u %u %u\n", cal.mn[0], cal.mn[1], cal.mn[2], cal.mn[3]);
      Serial.printf("[CAL] max: %u %u %u %u\n", cal.mx[0], cal.mx[1], cal.mx[2], cal.mx[3]);
      break;
    case 'S':
      cal.magic = CAL_MAGIC;
      calibSave();
      Serial.println(F("[CAL] saved, rebooting..."));
      Serial.flush();
      ESP.restart();
      break;
    case 'X':
      calibMode = false;
      calibLoad(); /* 丢弃未保存改动 */
      Serial.println(F("[CAL] exit"));
      break;
  }
}

/* ============ 串口命令处理 ============ */

/* 每帧最多处理 8 个串口字符，防止串口数据洪泛阻塞主循环导致丢帧 */
#define SERIAL_POLL_MAX 8

void serialPoll() {
  uint8_t count = 0;
  while (Serial.available() && count < SERIAL_POLL_MAX) {
    char c = Serial.read();
    count++;
    if (c == '\n' || c == '\r') continue;
    if (calibMode) {
      calibHandle(toupper(c));
    } else if (toupper(c) == 'C') {
      enterCalib();
    }
  }
}

/* ============ setup / loop ============ */

void setup() {
  Serial.begin(115200);
  delay(200); /* 等待 USB CDC 枚举 */

  analogReadResolution(10);

  EEPROM.begin(64);
  calibLoad();

  if (!radio.begin()) {
    Serial.println(F("[ERR] NRF24L01 not found, check wiring/power"));
    while (true) delay(1000); /* 停在这里，避免带病发送 */
  }
  radio.setChannel(RF_CHANNEL);
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_MAX);   /* 裸模块 MAX=0dBm，符合规范 */
  radio.setCRCLength(RF24_CRC_16); /* ⚠️ 必须与飞控 CONFIG(CRCO=1) 的 2 字节 CRC 一致，否则静默丢包 */
  radio.setAutoAck(false);         /* 单向广播：关 ACK，发送不阻塞重试 */
  radio.setRetries(0, 0);
  radio.setPayloadSize(FRAME_LEN);
  radio.openWritingPipe(RF_ADDR);
  radio.stopListening();

  /* 窗口缓冲先填当前值，避免上电回中过程被平滑拖尾 */
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t v = sampleAxis(joyPins[i]);
    for (uint8_t k = 0; k < WIN_DEPTH; k++) winBuf[i][k] = v;
  }

  Serial.println(F("[TX] ready, 50Hz, send C to calibrate"));
  lastFrameMs = millis();
}

void loop() {
  serialPoll();

  if (calibMode) {
    /* 校准模式不发送，只回显原始 ADC，便于观察摇杆行程 */
    if (millis() - lastPrintMs >= 200) {
      lastPrintMs = millis();
      Serial.printf("[ADC] %u %u %u %u\n",
                    sampleAxis(joyPins[0]), sampleAxis(joyPins[1]),
                    sampleAxis(joyPins[2]), sampleAxis(joyPins[3]));
    }
    return;
  }

  if (millis() - lastFrameMs < FRAME_PERIOD_MS) return;
  /* 落后过多（如刚从校准模式退出）重新同步，
   * 避免爆发式补发积压的几千帧 */
  if (millis() - lastFrameMs > FRAME_PERIOD_MS * 4) {
    lastFrameMs = millis();
    return;
  }
  lastFrameMs += FRAME_PERIOD_MS; /* 固定步进无累积漂移，严格 50Hz；
   * 回绕由上方无符号减法天然安全，大落后由重同步守卫兜底 */

  /* 采样 → 滤波 → 校准映射 */
  uint16_t us[4];
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t raw = sampleAxis(joyPins[i]);
    uint16_t sm = windowFilter(i, raw);
    us[i] = mapToUs(i, sm);
  }
  winIdx = (winIdx + 1) % WIN_DEPTH;

  uint8_t frame[FRAME_LEN];
  buildFrame(frame, us, armed);
  armPoll(us, millis());
  radio.write(frame, FRAME_LEN);

  /* 每 25 帧（0.5s）打印一次，避免刷屏 */
  if (++frameCount >= 25) {
    frameCount = 0;
    Serial.printf("[TX] roll=%u pitch=%u yaw=%u thr=%u(%u) %s\n",
                  us[IDX_ROLL], us[IDX_PITCH], us[IDX_YAW],
                  us[IDX_THROTTLE], remapThrottle(us[IDX_THROTTLE]),
                  armed ? "ARMED" : "disarmed");
  }
}
