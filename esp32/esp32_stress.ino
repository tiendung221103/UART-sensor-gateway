// ESP32 stress sender (giai đoạn 3): gửi frame qua UART2 tới OPi, điều khiển bằng lệnh qua USB.
// Mở Serial Monitor (115200, kết thúc dòng "Newline") hoặc terminal tới cổng USB và gõ lệnh:
//
//   period <ms>        chu kỳ gửi theo mili giây (mặc định 1000)
//   period_us <us>     chu kỳ gửi theo micro giây (để thử tốc độ cao)
//   corrupt <pm>       xác suất (phần nghìn) làm sai 1 bit trong payload -> CRC sai
//   drop <pm>          xác suất làm MẤT 1 byte ngẫu nhiên của frame -> lệch ranh giới
//   noise <pm>         xác suất chèn 1 byte rác trước frame
//   pause / resume     tạm dừng / tiếp tục gửi
//   reset              đưa SEQ và bộ đếm về 0
//   stats              in bộ đếm: frame gửi, byte gửi, số lỗi đã chèn
//
// Khuôn dạng frame giống esp32_sender.ino:
//   AA | LEN | TYPE | SEQ | PAYLOAD | CRC16_L | CRC16_H   (CRC16-CCITT-FALSE trên LEN..PAYLOAD)
// Chân UART2: TX2 = GPIO17, RX2 = GPIO16 (đổi nếu module của bạn dùng chân này cho PSRAM).

#include <Arduino.h>

#define LINK Serial2
constexpr int PIN_TX2 = 17;
constexpr int PIN_RX2 = 16;
constexpr uint32_t LINK_BAUD = 115200;

constexpr uint8_t SOF = 0xAA;
constexpr uint8_t TYPE_SENSOR = 0x01;
constexpr size_t PAYLOAD_LEN = 8;
constexpr size_t FRAME_LEN = 4 + PAYLOAD_LEN + 2;

static uint32_t period_us = 1000000UL;
static uint32_t corrupt_pm = 0, drop_pm = 0, noise_pm = 0;
static bool paused = false;

static uint8_t seq = 0;
static uint32_t counter = 0;
static uint32_t last_us = 0;

static uint32_t sent_frames = 0, sent_bytes = 0;
static uint32_t inj_corrupt = 0, inj_drop = 0, inj_noise = 0;

static uint16_t crc16_update(uint16_t crc, const uint8_t *d, size_t n)
{
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static bool hit(uint32_t permille)
{
  return permille && (uint32_t)random(1000) < permille;
}

static void send_sensor()
{
  uint8_t buf[FRAME_LEN];
  int16_t temp = 250 + (int16_t)(counter % 50);
  uint16_t hum = 600 + (uint16_t)(counter % 100);
  uint32_t up = millis();

  buf[0] = SOF;
  buf[1] = PAYLOAD_LEN;
  buf[2] = TYPE_SENSOR;
  buf[3] = seq++;
  memcpy(&buf[4], &temp, 2);
  memcpy(&buf[6], &hum, 2);
  memcpy(&buf[8], &up, 4);
  uint16_t crc = crc16_update(0xFFFF, &buf[1], 3 + PAYLOAD_LEN);
  buf[4 + PAYLOAD_LEN] = crc & 0xFF;
  buf[5 + PAYLOAD_LEN] = crc >> 8;
  size_t n = FRAME_LEN;

  if (hit(corrupt_pm)) {                 // làm sai 1 bit payload sau khi đã tính CRC
    buf[4 + random(PAYLOAD_LEN)] ^= (uint8_t)(1U << random(8));
    inj_corrupt++;
  }
  if (hit(drop_pm)) {                    // mất 1 byte ở vị trí ngẫu nhiên
    size_t k = random(n);
    memmove(&buf[k], &buf[k + 1], n - k - 1);
    n--;
    inj_drop++;
  }
  if (hit(noise_pm)) {                   // chèn 1 byte rác trước frame
    uint8_t g = (uint8_t)random(256);
    LINK.write(&g, 1);
    sent_bytes++;
    inj_noise++;
  }

  LINK.write(buf, n);                    // chặn khi bộ đệm TX đầy: tốc độ bị giới hạn bởi baud
  sent_bytes += n;
  sent_frames++;
  counter++;
}

static void handle_cmd(const char *line)
{
  unsigned long v;

  if (sscanf(line, "period_us %lu", &v) == 1 && v > 0) {
    period_us = v;
  } else if (sscanf(line, "period %lu", &v) == 1 && v > 0) {
    period_us = v * 1000UL;
  } else if (sscanf(line, "corrupt %lu", &v) == 1) {
    corrupt_pm = v;
  } else if (sscanf(line, "drop %lu", &v) == 1) {
    drop_pm = v;
  } else if (sscanf(line, "noise %lu", &v) == 1) {
    noise_pm = v;
  } else if (strncmp(line, "pause", 5) == 0) {
    paused = true;
  } else if (strncmp(line, "resume", 6) == 0) {
    paused = false;
    last_us = micros();
  } else if (strncmp(line, "reset", 5) == 0) {
    seq = 0; counter = 0;
    sent_frames = sent_bytes = inj_corrupt = inj_drop = inj_noise = 0;
  } else if (strncmp(line, "stats", 5) == 0) {
    // in bên dưới
  } else {
    Serial.println("lenh khong hop le");
    return;
  }
  Serial.printf("period_us=%lu corrupt=%lu drop=%lu noise=%lu paused=%d | "
                "sent_frames=%lu sent_bytes=%lu inj_corrupt=%lu inj_drop=%lu inj_noise=%lu\n",
                (unsigned long)period_us, (unsigned long)corrupt_pm, (unsigned long)drop_pm,
                (unsigned long)noise_pm, paused ? 1 : 0,
                (unsigned long)sent_frames, (unsigned long)sent_bytes,
                (unsigned long)inj_corrupt, (unsigned long)inj_drop, (unsigned long)inj_noise);
}

static void poll_usb()
{
  static char line[64];
  static size_t n = 0;

  while (Serial.available()) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (n) {
        line[n] = '\0';
        handle_cmd(line);
        n = 0;
      }
    } else if (n < sizeof(line) - 1) {
      line[n++] = (char)c;
    }
  }
}

void setup()
{
  Serial.begin(115200);                  // USB: lệnh điều khiển và in thống kê
  LINK.begin(LINK_BAUD, SERIAL_8N1, PIN_RX2, PIN_TX2);
  last_us = micros();
  Serial.println("esp32_stress san sang. Go 'stats' de xem bo dem.");
}

void loop()
{
  poll_usb();
  if (paused)
    return;

  uint32_t now = micros();
  if ((uint32_t)(now - last_us) >= period_us) {
    last_us += period_us;                                // giữ nhịp, tránh trôi
    if ((uint32_t)(now - last_us) > 10UL * period_us)    // bị trễ quá nhiều: đồng bộ lại
      last_us = now;
    send_sensor();
  }
}
