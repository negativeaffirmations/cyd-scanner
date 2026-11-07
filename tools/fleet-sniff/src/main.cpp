// fleet-sniff: receive-only 802.11 capture for checking the fleet link on air. It never transmits: the STA is
// started only so promiscuous mode works, and it never scans or connects.
//
// Keeps two kinds of management frame, and drops everything else in the callback:
//   A = action frames (ESP-NOW is a vendor action frame)
//   M = any other management frame sent by a MAC on the watch list
// Beacons and probes from other people's devices are never printed.
//
// Serial commands (newline terminated):  c <channel>   w <12 hex MAC>   s (stats)
// Output, one frame per line:  <A|M> <ms> <rssi> <ch> <fc hex> <addr1> <addr2> <len> <up to 128 bytes hex>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

static constexpr int KEEP = 128, MAX_WATCH = 24;
struct Rec { uint32_t ms; int8_t rssi; uint8_t ch; char kind; uint16_t len; uint8_t n; uint8_t d[KEEP]; };

static QueueHandle_t g_q;
static uint8_t  g_watch[MAX_WATCH][6];
static volatile int g_nWatch = 0;
static volatile uint32_t g_seen = 0, g_kept = 0, g_lost = 0;
static uint8_t  g_ch = 1;

static bool watched(const uint8_t* m) {
  for (int i = 0; i < g_nWatch; i++) if (!memcmp(g_watch[i], m, 6)) return true;
  return false;
}

static void IRAM_ATTR rxCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint16_t len = p->rx_ctrl.sig_len;
  if (len < 24) return;
  g_seen = g_seen + 1;
  const uint8_t subtype = (p->payload[0] >> 4) & 0x0F;
  char kind;
  if (subtype == 13) kind = 'A';
  else if (watched(p->payload + 10)) kind = 'M';
  else return;
  Rec r;
  r.ms = millis(); r.rssi = p->rx_ctrl.rssi; r.ch = p->rx_ctrl.channel; r.kind = kind; r.len = len;
  r.n = len < KEEP ? len : KEEP;
  memcpy(r.d, p->payload, r.n);
  if (xQueueSendFromISR(g_q, &r, nullptr) == pdTRUE) g_kept = g_kept + 1; else g_lost = g_lost + 1;
}

static void printRec(const Rec& r) {
  static char line[400];
  int o = snprintf(line, sizeof line, "%c %lu %d %u %02x%02x ", r.kind, (unsigned long)r.ms, r.rssi, r.ch, r.d[1], r.d[0]);
  for (int i = 4; i < 10; i++) o += snprintf(line + o, sizeof line - o, "%02x", r.d[i]);
  line[o++] = ' ';
  for (int i = 10; i < 16; i++) o += snprintf(line + o, sizeof line - o, "%02x", r.d[i]);
  o += snprintf(line + o, sizeof line - o, " %u ", r.len);
  for (int i = 0; i < r.n; i++) o += snprintf(line + o, sizeof line - o, "%02x", r.d[i]);
  Serial.println(line);
}

static void command(char* s) {
  if (s[0] == 'c') {
    int ch = atoi(s + 1);
    if (ch >= 1 && ch <= 13) { g_ch = ch; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }
    Serial.printf("# channel %u\n", g_ch);
  } else if (s[0] == 'w' && g_nWatch < MAX_WATCH) {
    uint8_t m[6]; unsigned v[6];
    if (sscanf(s + 1, " %2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
      for (int i = 0; i < 6; i++) m[i] = v[i];
      memcpy(g_watch[g_nWatch], m, 6);
      g_nWatch = g_nWatch + 1;
      Serial.printf("# watching %d\n", g_nWatch);
    }
  } else if (s[0] == 's') {
    Serial.printf("# stats ch %u mgmt %lu kept %lu lost %lu\n", g_ch, (unsigned long)g_seen, (unsigned long)g_kept, (unsigned long)g_lost);
  }
}

void setup() {
  Serial.setTxBufferSize(8192);
  Serial.begin(115200);
  g_q = xQueueCreate(64, sizeof(Rec));
  WiFi.mode(WIFI_STA);
  wifi_promiscuous_filter_t f = {};
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous_rx_cb(&rxCb);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(g_ch, WIFI_SECOND_CHAN_NONE);
  Serial.println("# fleet-sniff ready");
}

void loop() {
  static char in[40]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { in[n] = 0; if (n) command(in); n = 0; }
    else if (n < (int)sizeof in - 1) in[n++] = c;
  }
  Rec r;
  if (xQueueReceive(g_q, &r, pdMS_TO_TICKS(20)) == pdTRUE) printRec(r);
}
