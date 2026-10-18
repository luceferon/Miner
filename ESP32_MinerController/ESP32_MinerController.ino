/*
 * ESP32-C6-N16 Miner Controller v3
 */

#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <ESPmDNS.h>
#include <WiFiClientSecure.h>

/* ======================================================================== */
/*  DEFS                                                                    */
/* ======================================================================== */

#define MAX_MINERS          8
#define DEFAULT_WEB_USER    "admin"
#define DEFAULT_WEB_PASS    "capybara2026"
#define SETUP_AP_SSID       "MinerControl-Setup"
#define SETUP_AP_PASSWORD   "1234567890"
#define SETUP_AP_IP         192,168,4,1
#define DNS_PORT            53

struct MinerConfig;
struct MinerStatus;
struct DigestCtx;

DNSServer dnsServer;
WebServer server(80);

/* ======================================================================== */
/*  ВЕБ-КОНСОЛЬ ЛОГОВ (RING BUFFER 64KB)                                    */
/* ======================================================================== */
static SemaphoreHandle_t stateMutex, scanMutex, logMutex;
class LockGuard {
  SemaphoreHandle_t mutex;
public:
  explicit LockGuard(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(mutex); }
  LockGuard(const LockGuard&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
};
#define LOG_RING_SIZE  (64 * 1024)
static __attribute__((aligned(4))) char s_logRing[LOG_RING_SIZE];
static size_t s_logHead = 0, s_logTail = 0;
static uint64_t s_logDropped = 0;
static int s_logMinerFilter = -1;

static void logPushUnlocked(uint8_t c) {
  size_t next = (s_logHead + 1) % LOG_RING_SIZE;
  if (next == s_logTail) {
    s_logTail = (s_logTail + 1) % LOG_RING_SIZE;
    ++s_logDropped;
  }
  s_logRing[s_logHead] = (char)c;
  s_logHead = next;
}
static void logWriteUnlocked(const uint8_t* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i) logPushUnlocked(bytes[i]);
}
static void logPush(uint8_t c) {
  LockGuard lock(logMutex);
  logPushUnlocked(c);
}
static void logPushN(const uint8_t* buf, size_t n) {
  LockGuard lock(logMutex);
  logWriteUnlocked(buf, n);
}
static void logClear() {
  LockGuard lock(logMutex);
  s_logHead = s_logTail = 0;
  s_logDropped = 0;
}

static void logStreamToClient(bool includeBom) {
  LockGuard lock(logMutex);
  if (includeBom) {
    server.sendContent("\xEF\xBB\xBF", 3);
  }
  char header[240];
  int hlen = snprintf(header, sizeof(header),
      "# Log: 64 KiB; retained=%u bytes; overwritten=%llu bytes; miner_filter=%d (-1=all)\n"
      "# Oldest entry may be partial when overwritten > 0. Times are ESP uptime.\n",
      (unsigned)((s_logHead + LOG_RING_SIZE - s_logTail) % LOG_RING_SIZE),
      (unsigned long long)s_logDropped, s_logMinerFilter);
  if (hlen > 0) {
    server.sendContent(header, (size_t)hlen);
  }

  size_t h = s_logHead, t = s_logTail;
  while (t != h && (((uint8_t)s_logRing[t] & 0xC0) == 0x80)) {
    t = (t + 1) % LOG_RING_SIZE;
  }

  auto sendChunkedBuf = [](const char* ptr, size_t len) {
    const size_t CHUNK = 2048;
    while (len > 0) {
      size_t n = (len > CHUNK) ? CHUNK : len;
      server.sendContent(ptr, n);
      ptr += n;
      len -= n;
    }
  };

  if (h > t) {
    sendChunkedBuf(s_logRing + t, h - t);
  } else if (h < t) {
    sendChunkedBuf(s_logRing + t, LOG_RING_SIZE - t);
    if (h > 0) sendChunkedBuf(s_logRing, h);
  }
}

class LogTee : public Print {
public:
  virtual size_t write(uint8_t c) override { logPush((uint8_t)c); return 1; }
  virtual size_t write(const uint8_t* buffer, size_t size) override {
    logPushN(buffer, size); return size;
  }
  virtual void flush() override {}
};
static LogTee Log;

typedef enum : uint8_t {
  MT_ANTMINER_S19 = 1,
  MT_WHATSMINER_BIXBIT = 2,
  MT_ANTMINER_L3 = 3,
  MT_UNKNOWN = 0
} MinerType;

/* ======================================================================== */
/*  BASE64                                                                  */
/* ======================================================================== */

static const char B64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static String base64Encode(const String& input) {
  String out;
  out.reserve(((input.length() + 2) / 3) * 4);
  uint32_t val = 0;
  int vbits = -6;
  for (size_t i = 0; i < input.length(); i++) {
    val = (val << 8) + (uint8_t)input[i];
    vbits += 8;
    while (vbits >= 0) {
      out += B64_TABLE[(val >> vbits) & 0x3F];
      vbits -= 6;
    }
  }
  if (vbits > -6) out += B64_TABLE[((val << 8) >> (vbits + 8)) & 0x3F];
  while (out.length() & 3) out += '=';
  return out;
}
static String basicAuthHeader(const char* u, const char* p) {
  return "Basic " + base64Encode(String(u) + ":" + String(p));
}

/* ======================================================================== */
/*  CONFIG (NVS PERSISTENT)                                                 */
/* ======================================================================== */

Preferences prefs;

struct WiFiConfig {
  bool     configured = false;
  String   ssid = "";
  String   password = "";
  bool     useStatic = false;
  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns1;
  IPAddress dns2;
};

struct MinerConfig {
  bool      inUse = false;
  MinerType type = MT_UNKNOWN;
  String    name = "";
  IPAddress ip;
  uint16_t  apiPort = 0;
  String    user = "";
  String    pass = "";
};

struct AppConfig {
  WiFiConfig wifi;
  String     webUser = DEFAULT_WEB_USER;
  String     webPass = DEFAULT_WEB_PASS;
  MinerConfig miners[MAX_MINERS];
} cfg;

static void cfgSave() {
  prefs.begin("minerctl", false);
  prefs.putBool("w_cfg", cfg.wifi.configured);
  prefs.putString("w_ssid", cfg.wifi.ssid);
  prefs.putString("w_pass", cfg.wifi.password);
  prefs.putBool("w_static", cfg.wifi.useStatic);
  uint32_t buf[5];
  buf[0] = cfg.wifi.ip;      buf[1] = cfg.wifi.gateway;
  buf[2] = cfg.wifi.subnet;  buf[3] = cfg.wifi.dns1;  buf[4] = cfg.wifi.dns2;
  prefs.putBytes("w_ipdata", buf, sizeof(buf));

  prefs.putString("w_user", cfg.webUser);
  prefs.putString("w_pwd",  cfg.webPass);

  prefs.putUChar("mn_count", MAX_MINERS);
  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    const MinerConfig& m = cfg.miners[i];
    String kp = "m" + String(i) + "_";
    String ku = kp + "u";   prefs.putBool(ku.c_str(), m.inUse);
    String kt = kp + "t";   prefs.putUChar(kt.c_str(), (uint8_t)m.type);
    String kn = kp + "n";   prefs.putString(kn.c_str(), m.name);
    uint32_t ip = m.ip;
    String kip = kp + "ip"; prefs.putUInt(kip.c_str(), ip);
    String kpo = kp + "p";  prefs.putUShort(kpo.c_str(), m.apiPort);
    String kusr = kp+"usr"; prefs.putString(kusr.c_str(), m.user);
    String kpwd = kp+"pwd"; prefs.putString(kpwd.c_str(), m.pass);
  }
  prefs.end();
  Log.println("[CFG] Сохранено в NVS.");
}

static void cfgLoad() {
  prefs.begin("minerctl", true);
  cfg.wifi.configured = prefs.getBool("w_cfg", false);
  cfg.wifi.ssid       = prefs.getString("w_ssid", "");
  cfg.wifi.password   = prefs.getString("w_pass", "");
  cfg.wifi.useStatic  = prefs.getBool("w_static", false);
  uint32_t buf[5];
  size_t r = prefs.getBytesLength("w_ipdata");
  if (r == sizeof(buf)) {
    prefs.getBytes("w_ipdata", buf, sizeof(buf));
    cfg.wifi.ip      = buf[0]; cfg.wifi.gateway = buf[1]; cfg.wifi.subnet = buf[2];
    cfg.wifi.dns1    = buf[3]; cfg.wifi.dns2    = buf[4];
  } else {
    cfg.wifi.ip      = IPAddress(192,168,1,200);
    cfg.wifi.gateway = IPAddress(192,168,1,1);
    cfg.wifi.subnet  = IPAddress(255,255,255,0);
    cfg.wifi.dns1    = IPAddress(8,8,8,8);
    cfg.wifi.dns2    = IPAddress(1,1,1,1);
  }
  cfg.webUser = prefs.getString("w_user", DEFAULT_WEB_USER);
  cfg.webPass = prefs.getString("w_pwd",  DEFAULT_WEB_PASS);

  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    MinerConfig& m = cfg.miners[i];
    String kp = "m" + String(i) + "_";
    String ku = kp + "u";   m.inUse   = prefs.getBool(ku.c_str(), false);
    String kt = kp + "t";   m.type    = (MinerType)prefs.getUChar(kt.c_str(), 0);
    String kn = kp + "n";   m.name    = prefs.getString(kn.c_str(), String(""));
    String kip = kp + "ip"; m.ip      = (uint32_t)prefs.getUInt(kip.c_str(), 0);
    String kpo = kp + "p";  m.apiPort = prefs.getUShort(kpo.c_str(), 0);
    String kusr = kp+"usr"; m.user    = prefs.getString(kusr.c_str(), String(""));
    String kpwd = kp+"pwd"; m.pass    = prefs.getString(kpwd.c_str(), String(""));
  }
  prefs.end();
  Log.printf("[CFG] Загружено из NVS. WiFi configured: %d, майнеров в списке: ", cfg.wifi.configured);
  uint8_t c = 0; for (uint8_t i=0;i<MAX_MINERS;i++) if (cfg.miners[i].inUse) c++;
  Log.println(c);
}

static uint8_t minerFreeSlot() {
  for (uint8_t i = 0; i < MAX_MINERS; i++)
    if (!cfg.miners[i].inUse) return i;
  return 0xFF;
}

/* ======================================================================== */
/*  MINER STATUS + RUNTIME STATE                                            */
/* ======================================================================== */

struct ChainInfo {
  uint8_t  id = 0;
  uint16_t acn = 0;
  double   rateGHs = 0;
  uint32_t hw = 0;
  uint16_t freq = 0;
  String   tempChip = "";
  String   tempPcb = "";
  bool     badAsic = false;
};

struct PoolInfo {
  String   url = "";
  String   user = "";
  String   status = "";
  bool     active = false;
  String   diff = "";
  uint32_t accepted = 0;
  uint32_t rejected = 0;
  uint32_t stale = 0;
};

struct MinerStatus {
  bool      online = false;
  uint32_t  lastUpdate = 0;
  String    lastError = "";
  double    hashrateTHs = 0;
  double    hashrate5sTHs = 0;
  double    hashrateAvgTHs = 0;
  double    rate30mTHs = 0;
  double    idealTHs = 0;
  uint16_t  avgChipTempC = 0;
  uint16_t  maxChipTempC = 0;
  uint16_t  avgBoardTempC = 0;
  uint16_t  maxBoardTempC = 0;
  uint32_t  powerW = 0;
  uint32_t  voltage_mV = 0;
  uint32_t  current_mA = 0;
  uint16_t  fanRpm[4] = {0,0,0,0};
  uint8_t   fanCount = 0;
  String    status = "Unknown";
  String    errorMsg = "";
  uint32_t  uptimeSec = 0;
  uint32_t  acceptedShares = 0;
  uint32_t  rejectedShares = 0;
  uint32_t  staleShares = 0;
  uint32_t  hwErrors = 0;
  double    rejectedRatePct = 0;
  String    pool1User = "";
  String    pool2User = "";
  bool      rebootPending = false;
  String    rebootResult = "";
  uint32_t  rebootTime = 0;

  /* ---- Chains & Pools (v3) ---- */
  uint16_t  freqAvg = 0;
  uint16_t  totalAcn = 0;
  uint8_t   chainCount = 0;
  ChainInfo chains[4];
  uint8_t   poolCount = 0;
  PoolInfo  pools[3];

  /* ---- Whatsminer BiXBiT specific ---- */
  bool      isWhatsminer = false;
  String    minerModel = "";
  String    psuName = "";
  uint8_t   memUsage = 0;
  bool      noFeePenalty = false;
  uint8_t   fanPercent = 0;
  uint32_t  psuFanRpm = 0;
  uint32_t  fanInRpm = 0;
  uint32_t  fanOutRpm = 0;
  uint16_t  psuTemp0C = 0, psuTemp1C = 0, psuTemp2C = 0;
  uint16_t  envTempC = 0;
  double    factoryTHs = 0;
  double    psuVout = 0, psuVin0 = 0, psuIout = 0, psuIin0 = 0;
  uint16_t  chipTempTargetC = 0, chipTempProtectC = 0;
};

MinerStatus minerState[MAX_MINERS];
static uint32_t minerGeneration[MAX_MINERS] = {};
class MinerSnapshot {
public:
  uint8_t idx;
  uint32_t generation;
  MinerConfig config;
  MinerStatus status;
  bool reboot;
  bool publish;
  explicit MinerSnapshot(uint8_t i, bool rb = false) : idx(i), reboot(rb), publish(!rb) {
    LockGuard lock(stateMutex);
    generation = minerGeneration[i];
    config = cfg.miners[i];
    status = minerState[i];
  }
  ~MinerSnapshot() {
    if (!publish) return;
    LockGuard lock(stateMutex);
    if (generation != minerGeneration[idx]) return;
    if (reboot) {
      minerState[idx].rebootPending = status.rebootPending;
      minerState[idx].rebootResult = status.rebootResult;
      minerState[idx].rebootTime = status.rebootTime;
    } else {
      status.rebootPending = minerState[idx].rebootPending;
      status.rebootResult = minerState[idx].rebootResult;
      status.rebootTime = minerState[idx].rebootTime;
      minerState[idx] = status;
    }
  }
};

static void logMinerResponse(uint8_t idx, const MinerConfig& miner, uint16_t port,
                             const char* command, bool received, const String& body) {
  LockGuard lock(logMutex);
  if (s_logMinerFilter >= 0 && s_logMinerFilter != idx) return;
  String name = miner.name;
  name.replace("\r", " "); name.replace("\n", " ");
  if (name.length() > 96) name = name.substring(0, 96);
  char header[320];
  snprintf(header, sizeof(header),
      "\n===== BEGIN uptime_ms=%lu miner_id=%u name=\"%s\" ip=%s port=%u command=%s received=%s bytes=%u =====\n",
      (unsigned long)millis(), (unsigned)idx, name.c_str(), miner.ip.toString().c_str(),
      (unsigned)port, command, received ? "yes" : "no", (unsigned)body.length());
  logWriteUnlocked((const uint8_t*)header, strlen(header));
  if (body.length()) logWriteUnlocked((const uint8_t*)body.c_str(), body.length());
  else {
    const char* empty = "(empty response)";
    logWriteUnlocked((const uint8_t*)empty, strlen(empty));
  }
  char footer[100];
  snprintf(footer, sizeof(footer), "\n===== END miner_id=%u command=%s =====\n",
           (unsigned)idx, command);
  logWriteUnlocked((const uint8_t*)footer, strlen(footer));
}

/* ======================================================================== */
/*  ANTMINER / CGMINER / BTMINER TCP API 4028                               */
/* ======================================================================== */

static bool antTcpCmd(const IPAddress& ip, uint16_t port, const String& cmd, String& out, uint32_t toMs = 2500) {
  out = "";
  WiFiClient c;
  if (!c.connect(ip, port, toMs)) return false;
  StaticJsonDocument<64> doc;
  doc["command"] = cmd;
  String payload;
  serializeJson(doc, payload);
  c.print(payload);
  c.print('\n');
  c.flush();

  uint32_t t0 = millis();
  uint32_t lastRx = millis();
  String buf = "";
  buf.reserve(2048);
  bool gotData = false;

  while (millis() - t0 < toMs) {
    while (c.available()) {
      int ch = c.read();
      if (ch <= 0) continue;
      if (buf.length() >= 32768) {
        c.stop(); return false;
      }
      buf += (char)ch;
      gotData = true;
      lastRx = millis();
    }
    // Выходим только если сервер закрыл соединение И буфер пуст,
    // либо если после получения полного JSON (заканчивается на '}' или '\n') тишина > 60 мс
    if (gotData) {
      if (!c.connected() && !c.available()) break;
      if ((millis() - lastRx > 60) && (buf.endsWith("}") || buf.endsWith("\n"))) break;
    }
    delay(4);
  }
  c.stop();
  // Небольшая пауза, чтобы btminer на WhatsMiner успел освободить воркер перед следующим TCP-запросом
  delay(25);
  if (buf.length() == 0) return false;
  out = buf;
  return true;
}

static bool extractAntFragment(const String& src, const char* marker, String& out) {
  out = "";
  if (!marker || !marker[0] || src.length() == 0) return false;
  String key(marker);
  if (key.endsWith(":")) key.remove(key.length() - 1);
  int markerPos = src.indexOf(key);
  if (markerPos < 0) return false;

  int stack[32], count = 0;
  bool quoted = false, escaped = false;
  for (int i = 0; i < markerPos; ++i) {
    char ch = src[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
      continue;
    }
    if (ch == '"') quoted = true;
    else if (ch == '{') {
      if (count >= 32) return false;
      stack[count++] = i;
    } else if (ch == '}' && count) --count;
  }
  int begin = count ? stack[count - 1] : -1;
  if (begin < 0) return false;
  int depth = 0;
  quoted = escaped = false;
  for (size_t i = begin; i < src.length(); ++i) {
    char ch = src[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
      continue;
    }
    if (ch == '"') quoted = true;
    else if (ch == '{') ++depth;
    else if (ch == '}' && --depth == 0) {
      out = src.substring(begin, i + 1);
      return true;
    }
  }
  return false;
}

static bool parseAntSummary(const String& json, MinerStatus& o) {
  String body;
  if (!extractAntFragment(json, "\"SUMMARY\":", body) &&
      !extractAntFragment(json, "\"STATUS\":",  body)) {
    body = json;
  }
  JsonDocument d; DeserializationError e = deserializeJson(d, body);
  if (e) return false;
  if (!d["STATUS"][0].isNull()) o.status = d["STATUS"][0]["STATUS"] | "Unknown";
  if (!d["SUMMARY"][0].is<JsonObject>()) return false;
  if (!d["SUMMARY"][0].isNull()) {
    auto s = d["SUMMARY"][0];

    auto tryRate = [&](const char* k) -> double {
      if (s.containsKey(k)) {
        String ks(k);
        double v = s[k].as<double>();
        if (v == 0.0) return 0.0;
        if (ks.startsWith("GH") || ks.startsWith("GHS")) return v / 1000.0;
        if (ks.startsWith("MH") || ks.startsWith("MHS")) return v / 1000000.0;
        if (ks.startsWith("TH") || ks.startsWith("THS")) return v;
        if (v > 500000.0) return v / 1000000.0;
        if (v > 500.0)   return v / 1000.0;
        return v;
      }
      return -1.0;
    };
    double hv;
    hv = tryRate("GH 5s");   if (hv >= 0) o.hashrate5sTHs = hv;  else
    hv = tryRate("GHS 5s");  if (hv >= 0) o.hashrate5sTHs = hv;  else
    hv = tryRate("MHS 5s");  if (hv >= 0) o.hashrate5sTHs = hv;  else
    hv = tryRate("GHSav");   if (hv >= 0) o.hashrate5sTHs = hv;  else
    hv = tryRate("Rate 5s"); if (hv >= 0) o.hashrate5sTHs = hv;  else
    hv = tryRate("Rate 5S"); if (hv >= 0) o.hashrate5sTHs = hv;  else
    hv = tryRate("hashrate"); if (hv >= 0) o.hashrate5sTHs = hv; else
    hv = tryRate("hash_rate"); if (hv >= 0) o.hashrate5sTHs = hv;

    hv = tryRate("GH av");   if (hv >= 0) o.hashrateAvgTHs = hv; else
    hv = tryRate("GHS av");  if (hv >= 0) o.hashrateAvgTHs = hv; else
    hv = tryRate("MHS av");  if (hv >= 0) o.hashrateAvgTHs = hv; else
    hv = tryRate("Rate av"); if (hv >= 0) o.hashrateAvgTHs = hv; else
    hv = tryRate("Rate AV"); if (hv >= 0) o.hashrateAvgTHs = hv; else
    hv = tryRate("average_hashrate"); if (hv >= 0) o.hashrateAvgTHs = hv;

    hv = tryRate("GHS 30m"); if (hv >= 0) o.rate30mTHs     = hv;

    hv = tryRate("GH 1m");   if (hv >= 0) o.hashrateTHs    = hv; else
    hv = tryRate("MHS 1m");  if (hv >= 0) o.hashrateTHs    = hv; else
    hv = tryRate("Rate 1m"); if (hv >= 0) o.hashrateTHs    = hv; else
    hv = tryRate("Rate 15m"); if (hv >= 0) o.hashrateTHs   = hv; else
    hv = tryRate("Rate 24h"); if (hv >= 0) o.hashrateTHs   = hv; else
    o.hashrateTHs = o.hashrateAvgTHs;

    if (s.containsKey("Power"))               o.powerW = s["Power"];
    else if (s.containsKey("Power Input"))    o.powerW = s["Power Input"];
    else if (s.containsKey("Power Consumption")) o.powerW = s["Power Consumption"];
    else if (s.containsKey("Power Scrypt"))   o.powerW = s["Power Scrypt"];
    else if (s.containsKey("Input power"))    o.powerW = s["Input power"];
    else if (s.containsKey("Input Power"))    o.powerW = s["Input Power"];
    else if (s.containsKey("Power consumption")) o.powerW = s["Power consumption"];
    else if (s.containsKey("power"))          o.powerW = s["power"];
    else if (s.containsKey("wattage"))        o.powerW = s["wattage"];
    else o.powerW = 0;

    // >>> Фоллбэк на паспортную мощность из документации, если API не вернул ватты <<<
    if (o.powerW == 0) {
      String mModel = o.minerModel;
      mModel.toUpperCase();
      if (mModel.indexOf("S19I") >= 0) {
        o.powerW = 3100; // Antminer S19i из документации
      } else {
        o.powerW = 3250; // Antminer S19 по умолчанию
      }
    }

    o.uptimeSec      = s["Elapsed"] | s["Elapsed (secs)"] | 0;
    o.acceptedShares = s["Accepted"] | s["accepted"] | 0;
    o.rejectedShares = s["Rejected"] | s["rejected"] | 0;
    o.staleShares    = s["Stale"] | s["stale"] | 0;
    o.hwErrors       = s["Hardware Errors"] | 0;
    double a = o.acceptedShares, r = o.rejectedShares;
    o.rejectedRatePct = (a + r) > 0 ? (r / (a + r)) * 100.0 : 0.0;

    /* ================= Whatsminer BiXBiT / BTMiner specific ================= */
    if (s.containsKey("Miner Type") || s.containsKey("Factory GHS") ||
        s.containsKey("Fan Speed Percent")) {
      o.isWhatsminer = true;

      if (s.containsKey("Miner Type"))    o.minerModel = s["Miner Type"].as<String>();
      if (s.containsKey("PSU Name"))      o.psuName    = s["PSU Name"].as<String>();
      if (s.containsKey("Status"))        o.status     = s["Status"].as<String>();
      if (s.containsKey("Factory GHS"))   o.factoryTHs = s["Factory GHS"].as<double>() / 1000.0;
      if (s.containsKey("Miner Memory Usage")) o.memUsage = (uint8_t)(s["Miner Memory Usage"].as<int>());
      if (s.containsKey("No Fee Pools Penalty Active")) o.noFeePenalty = s["No Fee Pools Penalty Active"].as<bool>();

      if (s.containsKey("Fan Speed Percent")) o.fanPercent = (uint8_t)(s["Fan Speed Percent"].as<int>());
      if (s.containsKey("Fan Speed In"))      o.fanInRpm   = s["Fan Speed In"].as<uint32_t>();
      if (s.containsKey("Fan Speed Out"))     o.fanOutRpm  = s["Fan Speed Out"].as<uint32_t>();
      if (s.containsKey("PSU Fan Speed"))     o.psuFanRpm  = s["PSU Fan Speed"].as<uint32_t>();

      if (s.containsKey("PSU Temp0")) o.psuTemp0C = (uint16_t)(s["PSU Temp0"].as<double>());
      if (s.containsKey("PSU Temp1")) o.psuTemp1C = (uint16_t)(s["PSU Temp1"].as<double>());
      if (s.containsKey("PSU Temp2")) o.psuTemp2C = (uint16_t)(s["PSU Temp2"].as<double>());
      if (s.containsKey("Env Temp"))  o.envTempC  = (uint16_t)(s["Env Temp"].as<double>());

      if (s.containsKey("Chip Temp Target"))  o.chipTempTargetC  = (uint16_t)(s["Chip Temp Target"].as<double>());
      if (s.containsKey("Chip Temp Protect")) o.chipTempProtectC = (uint16_t)(s["Chip Temp Protect"].as<double>());

      if (s.containsKey("PSU Vout")) o.psuVout = s["PSU Vout"].as<double>();
      if (s.containsKey("PSU Vin0")) o.psuVin0 = s["PSU Vin0"].as<double>();
      if (s.containsKey("PSU Iout")) o.psuIout = s["PSU Iout"].as<double>();
      if (s.containsKey("PSU Iin0")) o.psuIin0 = s["PSU Iin0"].as<double>();

      if (s.containsKey("Power Realtime")) o.powerW = s["Power Realtime"].as<uint32_t>();

      if (s.containsKey("MHS av")) {
        double v = s["MHS av"].as<double>();
        if (v > 0) {
          o.hashrateAvgTHs = v;
          o.hashrateTHs    = v;
          o.hashrate5sTHs  = v;
        }
      }

      o.fanCount = 0;
      if (o.fanInRpm  && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)o.fanInRpm;
      if (o.fanOutRpm && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)o.fanOutRpm;
      if (o.psuFanRpm && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)o.psuFanRpm;
    }
  }
  return true;
}

// Вспомогательный парсер дефисных строк вида "30-34-48-59"
static void parseDashTemps(const char* str, uint32_t& sum, uint32_t& cnt, uint32_t& maxVal) {
  if (!str || !*str) return;
  const char* p = str;
  while (*p) {
    while (*p && (*p < '0' || *p > '9')) p++;
    if (!*p) break;
    int v = atoi(p);
    if (v > 0 && v < 180) {
      sum += (uint32_t)v;
      cnt++;
      if ((uint32_t)v > maxVal) maxVal = (uint32_t)v;
    }
    while (*p >= '0' && *p <= '9') p++;
  }
}

static bool parseAntStats(const String& json, MinerStatus& o) {
  String body;
  extractAntFragment(json, "\"STATS\":", body);
  if (body.length() == 0) body = json;
  JsonDocument d; DeserializationError e = deserializeJson(d, body);
  if (e) return false;

  if (d["STATS"][0].is<JsonObject>()) {
    JsonObject hdr = d["STATS"][0].as<JsonObject>();
    if (hdr.containsKey("Type") && o.minerModel.length() == 0) {
      o.minerModel = hdr["Type"].as<String>();
    }
  }

  if (!d["STATS"][1].is<JsonObject>()) return false;
  JsonObject obj = d["STATS"][1].as<JsonObject>();

  if (obj.containsKey("total_rateideal")) o.idealTHs = obj["total_rateideal"].as<double>() / 1000.0;
  if (obj.containsKey("total_freqavg"))   o.freqAvg  = obj["total_freqavg"].as<uint16_t>();
  else if (obj.containsKey("frequency"))  o.freqAvg  = obj["frequency"].as<uint16_t>();
  if (obj.containsKey("total_acn"))       o.totalAcn = obj["total_acn"].as<uint16_t>();
  if (obj.containsKey("no_matching_work") && o.hwErrors == 0) o.hwErrors = obj["no_matching_work"].as<uint32_t>();

  uint32_t chipSum = 0, chipMax = 0, chipCnt = 0;
  uint32_t pcbSum = 0,  pcbMax = 0,  pcbCnt = 0;
  o.fanCount = 0;

  for (JsonPair kv : obj) {
    const char* k = kv.key().c_str();
    // В Antminer S19: temp2_1..3 — это чипы, а temp1..3 — это платы (PCB)!
    if (strncmp(k, "temp2_", 6) == 0) {
      int t = kv.value().as<int>();
      if (t > 0 && t < 180) { chipSum += t; chipCnt++; if ((uint32_t)t > chipMax) chipMax = t; }
    } else if (strncmp(k, "temp_chip", 9) == 0 && kv.value().is<const char*>()) {
      parseDashTemps(kv.value().as<const char*>(), chipSum, chipCnt, chipMax);
    } else if (strncmp(k, "temp", 4) == 0 && isDigit(k[4]) && k[5] == '\0') {
      int t = kv.value().as<int>();
      if (t > 0 && t < 180) { pcbSum += t; pcbCnt++; if ((uint32_t)t > pcbMax) pcbMax = t; }
    } else if (strncmp(k, "temp_pcb", 8) == 0 && kv.value().is<const char*>()) {
      parseDashTemps(kv.value().as<const char*>(), pcbSum, pcbCnt, pcbMax);
    } else if (strncmp(k, "fan", 3) == 0 && isDigit(k[3]) && k[4] == '\0') {
      int rpm = kv.value().as<int>();
      if (rpm > 0 && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)rpm;
    }
  }

  if (chipCnt > 0) { o.avgChipTempC = chipSum / chipCnt; o.maxChipTempC = chipMax; }
  if (pcbCnt > 0)  { o.avgBoardTempC = pcbSum / pcbCnt;  o.maxBoardTempC = pcbMax; }

  // Извлечение телеметрии по каждой из 4 хеш-плат
  o.chainCount = 0;
  for (int c = 1; c <= 4; c++) {
    String kAcn  = "chain_acn"  + String(c);
    String kRate = "chain_rate" + String(c);
    String kHw   = "chain_hw"   + String(c);
    String kTCh  = "temp_chip"  + String(c);
    String kTPcb = "temp_pcb"   + String(c);
    String kFreq = "freq"       + String(c);
    String kAcs  = "chain_acs"  + String(c);

    uint16_t acn = obj[kAcn] | 0;
    double rate = 0;
    if (obj.containsKey(kRate)) {
      if (obj[kRate].is<const char*>()) rate = String(obj[kRate].as<const char*>()).toDouble();
      else rate = obj[kRate].as<double>();
    }
    if (acn == 0 && rate <= 0.0) continue;

    ChainInfo& ci = o.chains[o.chainCount++];
    ci.id       = c;
    ci.acn      = acn;
    ci.rateGHs  = rate;
    ci.hw       = obj[kHw] | 0;
    ci.freq     = obj[kFreq] | o.freqAvg;
    ci.tempChip = obj[kTCh] | "";
    ci.tempPcb  = obj[kTPcb] | "";
    String acs  = obj[kAcs] | "";
    ci.badAsic  = (acs.indexOf('x') >= 0 || acs.indexOf('X') >= 0 || acs.indexOf('-') >= 0);
  }
  return true;
}

// Парсинг ответа "devs" для WhatsMiner (температуры плат/чипов и хешрейт по платам)
static bool parseWhatsDevs(const String& json, MinerStatus& o) {
  JsonDocument d;
  DeserializationError e = deserializeJson(d, json);
  if (e) {
    String body;
    if (!extractAntFragment(json, "\"DEVS\":", body)) return false;
    if (deserializeJson(d, body)) return false;
  }

  JsonArray devs = d["DEVS"];
  if (devs.isNull() && d["Msg"].is<JsonObject>()) {
    devs = d["Msg"]["DEVS"];
  }
  if (devs.isNull() || devs.size() == 0) return false;

  uint32_t chipSum = 0, chipMax = 0, chipCnt = 0;
  uint32_t pcbSum = 0,  pcbMax = 0,  pcbCnt = 0;
  o.chainCount = 0;
  o.totalAcn = 0;

  for (size_t i = 0; i < devs.size() && i < 4; i++) {
    JsonObject dv = devs[i];
    int slot = dv["ASC"] | dv["Slot"] | (int)i;
    uint16_t acn = dv["Effective Chips"] | dv["Chips"] | 0;
    double mhsAv = dv["MHS av"] | dv["GHS av"] | 0.0;

    double rateGHs = 0;
    if (mhsAv > 500000.0)     rateGHs = mhsAv / 1000.0;
    else if (mhsAv > 0 && mhsAv < 500.0) rateGHs = mhsAv * 1000.0;
    else                      rateGHs = mhsAv;

    int tChipCurrent = 0;
    int tChipPeak    = 0;

    // Проверяем поля со строковым диапазоном вида "58°→86°C" или отдельные числовые ключи
    if (dv["Chip Temp"].is<const char*>()) {
      String tStr = dv["Chip Temp"].as<const char*>();
      int arrow = tStr.indexOf('→');
      if (arrow > 0) {
        tChipCurrent = tStr.substring(0, arrow).toInt();
        tChipPeak    = tStr.substring(arrow + 1).toInt();
      } else {
        tChipCurrent = tStr.toInt();
        tChipPeak    = tChipCurrent;
      }
    } else {
      tChipCurrent = dv["Chip Temp Min"] | dv["Chip Temp Avg"] | dv["Chip Temp"] | 0;
      tChipPeak    = dv["Chip Temp Max"] | tChipCurrent;
    }

    int tBoard = (int)(dv["Temperature"] | dv["Temp"] | dv["PCB Temp"] | 0.0);

    // Учитываем для средних/максимальных показателей по майнеру именно ПЕРВОЕ (текущее) число
    if (tChipCurrent > 0) {
      chipSum += tChipCurrent;
      chipCnt++;
      if ((uint32_t)tChipCurrent > chipMax) chipMax = (uint32_t)tChipCurrent;
    }
    if (tBoard > 0) {
      pcbSum += tBoard;
      pcbCnt++;
      if ((uint32_t)tBoard > pcbMax) pcbMax = (uint32_t)tBoard;
    }

    if (acn > 0 || rateGHs > 0) {
      ChainInfo& ci = o.chains[o.chainCount++];
      ci.id       = slot + 1;
      ci.acn      = acn;
      ci.rateGHs  = rateGHs;
      ci.hw       = dv["Hardware Errors"] | 0;
      ci.freq     = dv["Chip Frequency"] | dv["Frequency"] | 0;
      ci.badAsic  = (acn > 0 && acn < 100);

      // Сохраняем строку для отображения в таблице плат как есть
      if (dv["Chip Temp"].is<const char*>()) {
        ci.tempChip = dv["Chip Temp"].as<String>();
      } else if (tChipCurrent > 0 && tChipPeak > 0) {
        ci.tempChip = String(tChipCurrent) + "°→" + String(tChipPeak) + "°C";
      } else if (tBoard > 0) {
        ci.tempChip = String(tBoard) + "°C";
      }
      o.totalAcn += acn;
    }
  }

  if (chipCnt > 0) { o.avgChipTempC = chipSum / chipCnt; o.maxChipTempC = chipMax; }
  if (pcbCnt > 0)  { o.avgBoardTempC = pcbSum / pcbCnt;  o.maxBoardTempC = pcbMax; }
  return o.chainCount > 0;
}

static bool parseAntPools(const String& json, MinerStatus& o) {
  String body;
  extractAntFragment(json, "\"POOLS\":", body);
  if (body.length() == 0) body = json;
  JsonDocument d; DeserializationError e = deserializeJson(d, body);
  if (e) return false;
  JsonArray pools = d["POOLS"];
  if (pools.isNull()) return false;
  if (pools.size() > 0) o.pool1User = pools[0]["User"] | "";
  if (pools.size() > 1) o.pool2User = pools[1]["User"] | "";

  o.poolCount = 0;
  for (size_t i = 0; i < pools.size() && o.poolCount < 3; i++) {
    JsonObject p = pools[i];
    PoolInfo& pi = o.pools[o.poolCount++];
    pi.url      = p["URL"] | "";
    pi.user     = p["User"] | "";
    pi.status   = p["Status"] | "Unknown";
    pi.active   = p["Stratum Active"] | false;
    pi.accepted = p["Accepted"] | 0;
    pi.rejected = p["Rejected"] | 0;
    pi.stale    = p["Stale"] | 0;
    if (p.containsKey("Diff")) {
      pi.diff = p["Diff"].as<String>();
    } else if (p.containsKey("Stratum Difficulty")) {
      double sd = p["Stratum Difficulty"].as<double>();
      if (sd >= 1000.0) pi.diff = String(sd / 1000.0, 1) + "K";
      else if (sd > 0)  pi.diff = String((int)sd);
    }
  }
  return true;
}

/* ===================== Reboot API helpers ===================== */
static bool tcp4028Cmd(const IPAddress& ip, const String& command,
                       const String* paramsArr=nullptr, int paramsArrLen=0,
                       uint32_t toMs=6000, String* outResp=nullptr) {
  WiFiClient c;
  if (!c.connect(ip, 4028, (uint16_t)(toMs/2))) {
    Log.println("connect fail");
    return false;
  }
  StaticJsonDocument<384> doc;
  doc["command"] = command;
  doc["id"] = 1;
  if (paramsArrLen > 0 && paramsArr) {
    JsonArray a = doc.createNestedArray("parameter");
    for (int i=0;i<paramsArrLen;i++) a.add(paramsArr[i]);
  }
  String payload;
  serializeJson(doc, payload);
  c.print(payload);
  c.print('\n');
  c.flush();
  delay(6);

  String resp;
  uint32_t t0 = millis();
  while (millis() - t0 < toMs) {
    while (c.available()) {
      if (resp.length() >= 8192 || millis() - t0 >= toMs) { c.stop(); return false; }
      int ch = c.read();
      if (ch == 0) { c.stop(); break; }
      resp += (char)ch;
    }
    if (!c.connected() && c.available() == 0) break;
    delay(4);
  }
  c.flush();
  c.stop();
  if (outResp) *outResp = resp;
  Log.print("payload=[" + payload + "]  resp=");
  Log.println(resp.length() ? resp : "<empty>");
  if (!resp.length()) return false;
  if (resp.indexOf("\"STATUS\":\"S\"") >= 0 || resp.indexOf("\"STATUS\": \"S\"") >= 0) return true;
  if (resp.indexOf("success\":true")  >= 0) return true;
  if (resp.indexOf("\"STATUS\":\"E\"") >= 0) return false;
  return false;
}

static bool readHttpLine(Client& client, String& line, uint32_t started, uint32_t timeoutMs) {
  line = "";
  while ((uint32_t)(millis() - started) < timeoutMs) {
    if (client.available()) {
      int ch = client.read();
      if (ch < 0) continue;
      if (ch == '\n') return true;
      if (line.length() >= 2048) return false;
      line += (char)ch;
    } else if (!client.connected()) return false;
    else delay(2);
  }
  return false;
}

static bool httpRecon(const IPAddress& ip, const char* u, const char* p, const char* path) {
  WiFiClient c;
  if (!c.connect(ip, 80, 3000)) { Log.println("   recon:80 connect fail"); return false; }
  String req = "GET "; req += path; req += " HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  if (u && *u) req += "Authorization: " + basicAuthHeader(u, p) + "\r\n";
  req += "Accept: */*\r\n";
  req += "User-Agent: MinerCtl/2.2\r\nConnection: close\r\n\r\n";
  c.print(req);
  String head, body; int code=0; uint32_t t0=millis();
  String authHeader;
  while (millis()-t0 < 4500) {
    if (c.available()) {
      if (head.length() == 0 || head.indexOf("\r\n\r\n") < 0) {
        while (c.available() && head.length() < 2048) {
          char ch = c.read(); head += ch;
          if (ch == '\n') {
            int nl = head.lastIndexOf('\n', head.length()-2);
            String line = head.substring(nl < 0 ? 0 : nl+1);
            line.trim();
            if (line.startsWith("HTTP/1.")) {
              int sp = line.indexOf(' ');
              if (sp>0) code = line.substring(sp+1, line.indexOf(' ', sp+1)).toInt();
            }
            if (line.startsWith("WWW-Authenticate:") || line.startsWith("Www-Authenticate:")) {
              int col = line.indexOf(':');
              if (col>0) authHeader = line.substring(col+1); authHeader.trim();
            }
          }
          if (ch == '\n' && head.endsWith("\r\n\r\n")) break;
        }
      } else {
        while (c.available() && body.length()<900) body += (char)c.read();
      }
    } else if (!c.connected()) break;
    if (head.length() >= 2048 || body.length() >= 900) break;
    delay(3);
  }
  c.flush(); c.stop();
  Log.printf("   recon %s -> HTTP %d  head=%d  body=%d  auth=%s\r\n",
                path, code, head.length(), body.length(),
                authHeader.length() ? authHeader.c_str() : "<none>");
  return (code >= 200 && code < 300);
}

/* ================================================================ */
/*  MD5 + HTTP Digest Access Authentication (RFC 2617, qop=auth)   */
/* ================================================================ */
static uint32_t md5F(uint32_t x,uint32_t y,uint32_t z){return (x&y)|(~x&z);}
static uint32_t md5G(uint32_t x,uint32_t y,uint32_t z){return (x&z)|(y&~z);}
static uint32_t md5H(uint32_t x,uint32_t y,uint32_t z){return x^y^z;}
static uint32_t md5I(uint32_t x,uint32_t y,uint32_t z){return y^(x|~z);}
static uint32_t rotl32(uint32_t v,int s){return (v<<s)|(v>>(32-s));}
static void md5Transform(uint32_t state[4], const uint8_t block[64]) {
  const uint32_t s[64]={
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  const uint32_t K[64]={
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
  uint32_t M[16];
  for (int i=0;i<16;i++){
    M[i]=(uint32_t)block[i*4]|((uint32_t)block[i*4+1]<<8)|((uint32_t)block[i*4+2]<<16)|((uint32_t)block[i*4+3]<<24);
  }
  uint32_t A=state[0],B=state[1],C=state[2],D=state[3];
  for (int i=0;i<64;i++){
    uint32_t F,g;
    if (i<16){F=md5F(B,C,D);g=i;}
    else if(i<32){F=md5G(B,C,D);g=(5*i+1)%16;}
    else if(i<48){F=md5H(B,C,D);g=(3*i+5)%16;}
    else        {F=md5I(B,C,D);g=(7*i)%16;}
    F = F + A + K[i] + M[g];
    A = D; D = C; C = B; B += rotl32(F,s[i]);
  }
  state[0]+=A; state[1]+=B; state[2]+=C; state[3]+=D;
}
static void md5(const uint8_t* in, size_t len, uint8_t out[16]) {
  uint32_t state[4] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476};
  size_t off = 0;
  while (len - off >= 64) {
    md5Transform(state, in + off);
    off += 64;
  }
  __attribute__((aligned(4))) uint8_t sbuf[128];
  size_t rem = len - off;
  memcpy(sbuf, in + off, rem);
  sbuf[rem] = 0x80;
  size_t padLen = (rem < 56) ? (56 - rem) : (120 - rem);
  memset(sbuf + rem + 1, 0, padLen - 1);
  uint64_t bitLen = (uint64_t)len * 8;
  size_t bitOff = rem + padLen;
  for (int i=0;i<8;i++) sbuf[bitOff+i] = (uint8_t)((bitLen >> (8*i)) & 0xff);
  md5Transform(state, sbuf);
  if (rem >= 56) {
    md5Transform(state, sbuf + 64);
  }
  uint8_t* r[4]={(uint8_t*)&state[0],(uint8_t*)&state[1],(uint8_t*)&state[2],(uint8_t*)&state[3]};
  for (int i=0;i<4;i++){
    for (int j=0;j<4;j++) out[i*4+j]=r[i][j];
  }
}
static void md5HexBuf(const uint8_t h[16], char out33[33]) {
  static const char hex[]="0123456789abcdef";
  for (int i=0;i<16;i++){
    out33[i*2]   = hex[(h[i]>>4)&0xf];
    out33[i*2+1] = hex[h[i]&0xf];
  }
  out33[32] = '\0';
}
static void md5HexStr(const char* s, int slen, char out33[33]) {
  uint8_t h[16];
  md5((const uint8_t*)s, slen, h);
  md5HexBuf(h, out33);
}
struct DigestCtx { String realm,nonce,qop,opaque; };

static bool parseDigestChallenge(const String& headerVal,
                                 String& realm, String& nonce, String& qop,
                                 String& opaque) {
  realm=""; nonce=""; qop=""; opaque="";
  String s = headerVal;
  int idx = s.indexOf("Digest ");
  if (idx < 0) return false;
  s = s.substring(idx+7);
  int pos = 0;
  while (pos < (int)s.length()) {
    while (pos < (int)s.length() && (s[pos]==' ' || s[pos]==',')) pos++;
    if (pos >= (int)s.length()) break;
    int eq = s.indexOf('=', pos);
    if (eq < 0) break;
    String key = s.substring(pos, eq); key.trim();
    pos = eq+1;
    while (pos < (int)s.length() && s[pos] == ' ') ++pos;
    String val;
    if (pos < (int)s.length() && s[pos] == '"') {
      pos++;
      int end = s.indexOf('"', pos);
      if (end < 0) end = s.length();
      val = s.substring(pos, end);
      pos = end+1;
    } else {
      int end = s.indexOf(',', pos);
      if (end<0) end = s.length();
      val = s.substring(pos, end); val.trim();
      pos = end;
    }
    if (key == "realm")  realm  = val;
    if (key == "nonce")  nonce  = val;
    if (key == "qop")    qop    = val;
    if (key == "opaque") opaque = val;
    if (key == "algorithm" && val != "MD5" && val != "md5") return false;
  }
  if (qop.length()) {
    bool supportsAuth = false;
    int start = 0;
    while (start < (int)qop.length()) {
      int comma = qop.indexOf(',', start);
      if (comma < 0) comma = qop.length();
      String token = qop.substring(start, comma); token.trim();
      if (token == "auth") supportsAuth = true;
      start = comma + 1;
    }
    if (!supportsAuth) return false;
    qop = "auth";
  }
  return realm.length()>0 && nonce.length()>0;
}

static bool fetchDigestChallenge(const IPAddress& ip, DigestCtx& ctx, uint32_t toMs=5000) {
  WiFiClient c;
  if (!c.connect(ip, 80, toMs/2)) return false;
  char ipstr[20]; snprintf(ipstr,sizeof(ipstr),"%d.%d.%d.%d",ip[0],ip[1],ip[2],ip[3]);
  char req[256];
  snprintf(req,sizeof(req),
           "GET / HTTP/1.1\r\n"
           "Host: %s\r\n"
           "User-Agent: MinerCtl/2.2\r\n"
           "Accept: */*\r\n"
           "Connection: close\r\n\r\n",
           ipstr);
  c.print(req);
  String auth; int code = 0; uint32_t t0=millis();
  while (millis()-t0 < toMs) {
    if (c.available()) {
      String line;
      if (!readHttpLine(c, line, t0, toMs)) break;
      line.trim();
      if (line.startsWith("HTTP/1.")) {
        int sp=line.indexOf(' ');
        if (sp>0) code = line.substring(sp+1, line.indexOf(' ',sp+1)).toInt();
      }
      String lowerLine = line; lowerLine.toLowerCase();
      if (lowerLine.startsWith("www-authenticate:")) {
        int col = line.indexOf(':');
        auth = line.substring(col+1); auth.trim();
      }
      if (line.length()==0 && code != 0) break;
    } else if (!c.connected()) break;
    else delay(4);
  }
  c.flush(); c.stop();
  (void)code;
  return parseDigestChallenge(auth, ctx.realm, ctx.nonce, ctx.qop, ctx.opaque);
}

static void genCnonce16(char out17[17]) {
  snprintf(out17, 17, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
}

static String digestQuoted(const String& value) {
  String result;
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\r' || c == '\n') continue;
    if (c == '"' || c == '\\') result += '\\';
    result += c;
  }
  return result;
}

static bool buildDigestHeaderRaw(const char* ipstr, const char* method, const char* path,
                                 const DigestCtx& d, const char* u, const char* p,
                                 uint32_t nc, const char cnonce17[17], char out[768]) {
  (void)ipstr;
  String ha1 = String(u) + ":" + d.realm + ":" + p;
  String ha2 = String(method) + ":" + path;
  char ha1h[33], ha2h[33], resh[33], ncs[9];
  md5HexStr(ha1.c_str(), ha1.length(), ha1h);
  md5HexStr(ha2.c_str(), ha2.length(), ha2h);
  snprintf(ncs, sizeof(ncs), "%08lx", (unsigned long)nc);
  String response = String(ha1h) + ":" + d.nonce + ":";
  if (d.qop.length()) response += String(ncs) + ":" + cnonce17 + ":auth:";
  response += ha2h;
  md5HexStr(response.c_str(), response.length(), resh);
  String header = "Digest username=\"" + digestQuoted(u) + "\", realm=\"" +
      digestQuoted(d.realm) + "\", nonce=\"" + digestQuoted(d.nonce) +
      "\", uri=\"" + digestQuoted(path) + "\"";
  if (d.opaque.length()) header += ", opaque=\"" + digestQuoted(d.opaque) + "\"";
  if (d.qop.length()) header += ", qop=auth, nc=" + String(ncs) + ", cnonce=\"" + cnonce17 + "\"";
  header += ", response=\"" + String(resh) + "\"";
  if (header.length() >= 768) { out[0] = 0; return false; }
  header.toCharArray(out, 768);
  return true;
}

static int httpDigestRequestRaw(const IPAddress& ip, const char* method, const char* urlPath,
                                const char* u, const char* p,
                                const char* contentType, const uint8_t* bodyData, size_t bodyLen,
                                String* outAuthErr, uint32_t toMs=5500) {
  DigestCtx d;
  if (!fetchDigestChallenge(ip, d, toMs)) {
    if (outAuthErr) *outAuthErr = "cannot fetch Digest challenge (порт 80 не отвечает?)";
    return 0;
  }
  WiFiClient c;
  if (!c.connect(ip, 80, toMs/2)) {
    if (outAuthErr) *outAuthErr="connect fail to port 80";
    return -1;
  }

  char ipstr[20]; snprintf(ipstr,sizeof(ipstr),"%d.%d.%d.%d",ip[0],ip[1],ip[2],ip[3]);
  uint32_t nc = 1;
  char cnonce17[17]; genCnonce16(cnonce17);
  char authHdr[768];
  if (!buildDigestHeaderRaw(ipstr, method, urlPath, d, u, p, nc, cnonce17, authHdr)) {
    if (outAuthErr) *outAuthErr = "Digest header too long";
    c.stop(); return -1;
  }

  String request = String(method) + " " + urlPath + " HTTP/1.1\r\nHost: " + ipstr +
      "\r\nAuthorization: " + authHdr + "\r\nUser-Agent: MinerCtl/2.2\r\nAccept: */*\r\n";
  if (bodyLen > 0) {
    if (!contentType || !bodyData) { c.stop(); return -1; }
    request += "Content-Type: " + String(contentType) + "\r\nContent-Length: " + String(bodyLen) + "\r\n";
  }
  request += "Connection: close\r\n\r\n";
  c.print(request);
  if (bodyLen) c.write(bodyData, bodyLen);

  uint32_t t0 = millis();
  int code = 0;
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String line;
      if (!readHttpLine(c, line, t0, toMs)) break;
      line.trim();
      if (line.startsWith("HTTP/1.")) {
        int sp = line.indexOf(' ');
        if (sp>0) code = line.substring(sp+1, line.indexOf(' ', sp+1)).toInt();
      }
      if (line.length() == 0 && code != 0) break;
    } else if (!c.connected()) break;
    else delay(4);
  }
  c.flush(); c.stop();
  if (outAuthErr && code == 401) *outAuthErr = "Digest 401 — creds user/pass неверные";
  if (outAuthErr && code == 0)   *outAuthErr = "no response";
  return code;
}

static int httpDigestGet(const IPAddress& ip, const char* path, const char* u, const char* p, String* err=nullptr) {
  return httpDigestRequestRaw(ip, "GET", path, u, p, nullptr, nullptr, 0, err);
}
static int httpDigestPost(const IPAddress& ip, const char* path, const char* u, const char* p,
                          const char* ct, const uint8_t* bodyData, size_t bodyLen, String* err=nullptr) {
  return httpDigestRequestRaw(ip, "POST", path, u, p, ct, bodyData, bodyLen, err);
}

/* ===================== WhatsMiner LuCI restart (HTTPS) ===================== */
static int httpsRequestGet(const IPAddress& ip, uint16_t port, const String& path,
                           const char* user, const char* pass, const char* cookie,
                           String* outHead = nullptr, uint32_t toMs = 6000) {
  WiFiClientSecure c;
  c.setInsecure();
  if (!c.connect(ip, port, toMs / 2)) return -1;

  String req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  if (cookie && *cookie) {
    req += "Cookie: " + String(cookie) + "\r\n";
  } else if (user && *user) {
    req += "Authorization: " + basicAuthHeader(user, pass ? pass : "") + "\r\n";
  }
  req += "User-Agent: MinerCtl/2.2\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: close\r\n\r\n";
  c.print(req);

  String head; int code = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String line;
      if (!readHttpLine(c, line, t0, toMs)) break;
      line.trim();
      if (line.startsWith("HTTP/1.")) {
        int sp = line.indexOf(' ');
        if (sp > 0) code = line.substring(sp + 1, line.indexOf(' ', sp + 1)).toInt();
      }
      if (head.length() + line.length() > 8192) { c.stop(); return -1; }
      head += line; head += "\n";
      if (line.length() == 0 && code != 0) break;
    } else if (!c.connected()) break;
    else delay(5);
  }
  c.stop();
  if (outHead) *outHead = head;
  return code;
}

static String formEncode(const String& value) {
  static const char hex[] = "0123456789ABCDEF";
  String encoded;
  for (size_t i = 0; i < value.length(); ++i) {
    uint8_t c = (uint8_t)value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += (char)c;
    else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
  }
  return encoded;
}

static String httpsLuciLogin(const IPAddress& ip, uint16_t port,
                             const char* user, const char* pass, uint32_t toMs = 6000) {
  WiFiClientSecure c;
  c.setInsecure();
  if (!c.connect(ip, port, toMs / 2)) return "";

  String body = "luci_username=" + formEncode(user) + "&luci_password=" + formEncode(pass);
  String req = "POST /cgi-bin/luci HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  req += "Content-Type: application/x-www-form-urlencoded\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "User-Agent: MinerCtl/2.2\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;
  c.print(req);

  String cookie;
  uint32_t t0 = millis();
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String line;
      if (!readHttpLine(c, line, t0, toMs)) break;
      line.trim();
      String lowerLine = line; lowerLine.toLowerCase();
      if (lowerLine.startsWith("set-cookie:")) {
        int col = line.indexOf(':');
        String cv = line.substring(col + 1); cv.trim();
        for (const char* key : { "sysauth_https=", "sysauth=" }) {
          int s = cv.indexOf(key);
          if (s >= 0) {
            int e = cv.indexOf(';', s);
            if (e < 0) e = cv.length();
            cookie = cv.substring(s, e);
            break;
          }
        }
        if (cookie.length()) break;
      }
      if (line.length() == 0) break;
    } else if (!c.connected()) break;
    else delay(5);
  }
  c.stop();
  return cookie;
}

static bool whatsminerLuCIRestart(const IPAddress& ip, const char* user, const char* pass) {
  const char* restartPath = "/cgi-bin/luci/admin/status/btminerstatus/restart";
  const uint16_t httpsPort = 443;

  Log.print(F("[RBT-WM] LuCI HTTPS restart: "));
  Log.print(ip.toString());
  Log.print(F(" path=")); Log.println(restartPath);

  {
    String head;
    int code = httpsRequestGet(ip, httpsPort, restartPath, user, pass, nullptr, &head, 6000);
    Log.print(F("[RBT-WM] 1) Basic-HTTPS code=")); Log.println(code);
    if (code >= 200 && code < 300) return true;
  }

  {
    String cookie = httpsLuciLogin(ip, httpsPort, user, pass, 6000);
    if (cookie.length()) {
      String head;
      int code = httpsRequestGet(ip, httpsPort, restartPath, nullptr, nullptr,
                                 cookie.c_str(), &head, 6000);
      Log.print(F("[RBT-WM] 2) Cookie-HTTPS code=")); Log.println(code);
      if (code >= 200 && code < 300) return true;
    }
  }
  return false;
}

static bool antHttpReboot(const IPAddress& ip, const char* u, const char* p) {
  struct { const char* u; const char* p; } creds[4];
  uint8_t ncred = 0;
  if (u && *u) {
    creds[ncred++] = {u, p ? p : ""};
  } else {
    creds[ncred++] = {"root",  "root"};
    creds[ncred++] = {"admin", "root"};
    creds[ncred++] = {"root",  "admin"};
    creds[ncred++] = {"admin", "admin"};
  }

  httpRecon(ip, (u && *u) ? u : "root", (u && *u) ? p : "root", "/");

  bool ok = false;
  {
    String pt[] = {"true"};
    if (tcp4028Cmd(ip, "reboot", pt, 1, 4000)) ok = true;
  }

  if (!ok) {
    const char* pathsGet[] = {
      "/cgi-bin/reboot.cgi",
      "/cgi-bin/reboot_system.cgi",
      "/cgi-bin/reboot_system",
      "/cgi-bin/luci/admin/system/reboot",
      "/cgi-bin/reboot.cgi?action=go",
      "/reboot",
      "/api/reboot"
    };
    const uint8_t np = sizeof(pathsGet) / sizeof(pathsGet[0]);
    for (uint8_t ci = 0; ci < ncred && !ok; ci++) {
      String err;
      for (uint8_t pi = 0; pi < np && !ok; pi++) {
        int code = httpDigestGet(ip, pathsGet[pi], creds[ci].u, creds[ci].p, &err);
        if (code >= 200 && code < 300) ok = true;
        delay(250);
      }
      if (!ok) {
        const char* bodyJ = "{\"reboot\":true}";
        int code = httpDigestPost(ip, "/api/system/reboot", creds[ci].u, creds[ci].p,
                                  "application/json",
                                  (const uint8_t*)bodyJ, strlen(bodyJ), &err);
        if (code >= 200 && code < 300) ok = true;
      }
    }
  }
  return ok;
}

/* ======================================================================== */
/*  WHATSMINER BIXBIT HTTP API                                              */
/* ======================================================================== */

static bool whatsApiGet(const IPAddress& ip, uint16_t port, const char* u, const char* p,
                        String path, String& body, int& codeOut, uint32_t toMs = 4000) {
  HTTPClient h; WiFiClient c;
  String url = "http://" + ip.toString() + ":" + String(port) + path;
  if (!h.begin(c, url)) return false;
  h.addHeader("Authorization", basicAuthHeader(u, p));
  h.addHeader("Accept", "application/json");
  h.setTimeout(toMs);
  codeOut = h.GET();
  if (codeOut > 0) body = h.getString();
  h.end();
  return (codeOut >= 200 && codeOut < 300);
}

static bool parseWhatsStatus(const String& json, MinerStatus& o) {
  JsonDocument d; DeserializationError e = deserializeJson(d, json);
  if (e || !d.is<JsonObject>()) return false;
  JsonObject source = d["data"].is<JsonObject>() ? d["data"].as<JsonObject>() : d.as<JsonObject>();
  if (source["hashrate_th"].isNull() && source["hashrate"].isNull() &&
      source["power_w"].isNull() && source["power"].isNull() && source["fans"].isNull()) return false;
  o.isWhatsminer = true;
  if (!d["data"].isNull()) {
    auto x = d["data"];
    o.status = x["status"] | "Unknown";
    o.errorMsg = x["error"] | "";
    o.hashrateTHs = x["hashrate_th"] | x["hashrate"] | 0;
    o.hashrate5sTHs = x["hashrate_5s_th"] | o.hashrateTHs;
    o.hashrateAvgTHs = x["hashrate_avg_th"] | o.hashrateTHs;
    o.powerW = x["power_w"] | x["power"] | 0;
    o.voltage_mV = x["voltage_mv"] | 0;
    o.current_mA = x["current_ma"] | 0;
    o.uptimeSec = x["uptime_sec"] | 0;
    o.acceptedShares = x["accepted"] | x["accepted_shares"] | 0;
    o.rejectedShares = x["rejected"] | x["rejected_shares"] | 0;
    double a = o.acceptedShares, r = o.rejectedShares;
    o.rejectedRatePct = (a + r) > 0 ? (r / (a + r)) * 100.0 : 0.0;
    if (!x["chip_temp_avg"].isNull()) o.avgChipTempC = x["chip_temp_avg"];
    if (!x["chip_temp_max"].isNull()) o.maxChipTempC = x["chip_temp_max"];
    if (!x["temp_avg"].isNull()) o.avgChipTempC = x["temp_avg"];
    if (!x["temp_max"].isNull()) o.maxChipTempC = x["temp_max"];
    if (!x["board_temp"].isNull()) o.avgBoardTempC = x["board_temp"];
    o.fanCount = 0;
    if (!x["fans"].isNull()) {
      JsonArray fans = x["fans"];
      for (size_t i = 0; i < fans.size() && i < 4; i++)
        o.fanRpm[o.fanCount++] = fans[i] | 0;
    }
    if (!x["pools"].isNull()) {
      JsonArray pools = x["pools"];
      if (pools.size() > 0) o.pool1User = pools[0]["user"] | pools[0]["Worker"] | "";
      if (pools.size() > 1) o.pool2User = pools[1]["user"] | pools[1]["Worker"] | "";
    }
  } else {
    o.hashrateTHs = d["hashrate_th"] | d["hashrate"] | 0;
    o.hashrate5sTHs = d["hashrate_5s_th"] | o.hashrateTHs;
    o.hashrateAvgTHs = d["hashrate_avg_th"] | o.hashrateTHs;
    o.powerW = d["power"] | 0;
    o.status = d["status"] | "Unknown";
  }
  return true;
}

static bool parseWhatsPools(const String& json, MinerStatus& o) {
  JsonDocument d; DeserializationError e = deserializeJson(d, json);
  if (e) return false;
  JsonArray pools;
  if (!d["pools"].isNull()) pools = d["pools"];
  else if (!d["data"].isNull() && !d["data"]["pools"].isNull()) pools = d["data"]["pools"];
  if (pools.isNull()) return false;
  if (pools.size() > 0) o.pool1User = pools[0]["user"] | pools[0]["Worker"] | pools[0]["User"] | "";
  if (pools.size() > 1) o.pool2User = pools[1]["user"] | pools[1]["Worker"] | pools[1]["User"] | "";
  return true;
}

/* ======================================================================== */
/*  MINER POLLING + REBOOT DISPATCHER                                       */
/* ======================================================================== */

static const uint32_t STATUS_INTERVAL_MS = 5000;
static const uint32_t WIFI_RECONNECT_MS = 3000;
static const uint32_t REBOOT_MSG_TTL_MS = 180000;
volatile uint32_t lastStatusTick = 0;
bool isPortalMode = false;
static bool firstPollDone = false;
static uint32_t bootReadyAtMs = 0;

volatile bool s_pollBusy = false;
volatile bool s_pollTaskStarted = false;

static void pollMiner(uint8_t idx);
static void rebootMiner(uint8_t idx);

static void pollMinerTask(void* pvParam) {
  (void)pvParam;
  Log.println("[POLL] Background worker started.");
  for (;;) {
    if (isPortalMode || WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(1000)); continue;
    }
    uint32_t now = millis();
    if ((int32_t)(now - bootReadyAtMs) < 0 && !firstPollDone) {
      vTaskDelay(pdMS_TO_TICKS(100)); continue;
    }
    for (uint8_t i = 0; i < MAX_MINERS; ++i) rebootMiner(i);
    if (!firstPollDone || (uint32_t)(now - lastStatusTick) >= STATUS_INTERVAL_MS) {
      firstPollDone = true;
      s_pollBusy = true;
      for (uint8_t i = 0; i < MAX_MINERS; ++i) {
        rebootMiner(i);
        pollMiner(i);
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      s_pollBusy = false;
      lastStatusTick = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

static void startPollTask() {
  if (s_pollTaskStarted) return;
  BaseType_t ok = xTaskCreate(pollMinerTask, "pollMiner", 8192, NULL, 2, NULL);
  s_pollTaskStarted = (ok == pdPASS);
}

static void pollMiner(uint8_t idx) {
  MinerSnapshot snapshot(idx);
  MinerConfig& m = snapshot.config;
  MinerStatus& s = snapshot.status;
  if (!m.inUse) return;

  // Сохраняем данные плат с прошлого успешного опроса на случай пропуска пакета devs/stats
  uint32_t previousUpdate   = s.lastUpdate;
  uint8_t  prevChainCount   = s.chainCount;
  uint16_t prevTotalAcn     = s.totalAcn;
  uint16_t prevAvgChipTemp  = s.avgChipTempC;
  uint16_t prevMaxChipTemp  = s.maxChipTempC;
  uint16_t prevAvgBoardTemp = s.avgBoardTempC;
  uint16_t prevMaxBoardTemp = s.maxBoardTempC;
  ChainInfo prevChains[4];
  for (uint8_t k = 0; k < 4; k++) prevChains[k] = s.chains[k];

  s = MinerStatus();
  s.lastUpdate = previousUpdate;

  IPAddress zero(0, 0, 0, 0);
  if (!isPortalMode && (WiFi.status() != WL_CONNECTED || WiFi.localIP() == zero)) {
    s.online = false; s.status = "Нет Wi-Fi";
    s.lastError = "ESP не подключен к сети.";
    return;
  }
  if (m.ip == zero) {
    s.online = false; s.status = "IP не задан";
    s.lastError = "IP адрес майнера не настроен — сохраните заново в Настройках.";
    s.lastUpdate = millis();
    return;
  }

  if (m.type == MT_ANTMINER_S19 || m.type == MT_ANTMINER_L3) {
    uint16_t port = m.apiPort ? m.apiPort : 4028;
    String sum, stats, pools;
    bool okS  = antTcpCmd(m.ip, port, "summary", sum);
    logMinerResponse(idx, m, port, "summary", okS, sum);

    bool parsed = false;
    if (okS && parseAntSummary(sum, s)) parsed = true;

    // Если это Antminer, но в summary определился как Whatsminer, запрашиваем devs вместо stats
    const char* secondCmd = s.isWhatsminer ? "devs" : "stats";
    bool okSt = antTcpCmd(m.ip, port, secondCmd, stats);
    logMinerResponse(idx, m, port, secondCmd, okSt, stats);

    bool okP  = antTcpCmd(m.ip, port, "pools", pools);
    logMinerResponse(idx, m, port, "pools", okP, pools);

    if (!okS && !okSt) {
      s.online = false; s.status = "OFFLINE";
      s.lastError = "Нет связи с TCP API " + m.ip.toString() + ":" + String(port);
      return;
    }
    s.online = true; s.lastUpdate = millis();
    if (okSt) {
      if (s.isWhatsminer) { if (parseWhatsDevs(stats, s)) parsed = true; }
      else                { if (parseAntStats(stats, s))  parsed = true; }
    }
    if (okP) parseAntPools(pools, s);

    // Восстанавливаем кэш плат из предыдущего цикла, если текущий опрос плат не вернул данные
    if (s.online && s.chainCount == 0 && prevChainCount > 0) {
      s.chainCount    = prevChainCount;
      s.totalAcn      = prevTotalAcn;
      s.avgChipTempC  = prevAvgChipTemp;
      s.maxChipTempC  = prevMaxChipTemp;
      s.avgBoardTempC = prevAvgBoardTemp;
      s.maxBoardTempC = prevMaxBoardTemp;
      for (uint8_t k = 0; k < 4; k++) s.chains[k] = prevChains[k];
    }

    if (!parsed) {
      s.lastError = "Ответ от майнера получен, но поля данных не распознаны.";
      s.status = "Данные не распознаны";
    }
    return;
  }

  if (m.type == MT_WHATSMINER_BIXBIT) {
    uint16_t tcpPort = (m.apiPort && m.apiPort != 80) ? m.apiPort : 4028;
    String sum, devs, pools;
    bool okS  = antTcpCmd(m.ip, tcpPort, "summary", sum);
    logMinerResponse(idx, m, tcpPort, "summary", okS, sum);
    bool okDv = antTcpCmd(m.ip, tcpPort, "devs", devs);
    logMinerResponse(idx, m, tcpPort, "devs", okDv, devs);
    bool okP  = antTcpCmd(m.ip, tcpPort, "pools", pools);
    logMinerResponse(idx, m, tcpPort, "pools", okP, pools);

    if (okS || okDv) {
      s.online = true; s.lastUpdate = millis();
      bool parsed = false;
      if (okS)  if (parseAntSummary(sum, s)) parsed = true;
      if (okDv) if (parseWhatsDevs(devs, s)) parsed = true;
      if (okP)  parseAntPools(pools, s);

      // Восстанавливаем кэш плат из предыдущего цикла, если devs пропустил пакет
      if (s.online && s.chainCount == 0 && prevChainCount > 0) {
        s.chainCount    = prevChainCount;
        s.totalAcn      = prevTotalAcn;
        s.avgChipTempC  = prevAvgChipTemp;
        s.maxChipTempC  = prevMaxChipTemp;
        s.avgBoardTempC = prevAvgBoardTemp;
        s.maxBoardTempC = prevMaxBoardTemp;
        for (uint8_t k = 0; k < 4; k++) s.chains[k] = prevChains[k];
      }

      if (!parsed) {
        s.lastError = "TCP 4028 ответ получен, но поля не распознаны.";
        s.status = "Данные не распознаны";
      }
      return;
    }

    // Fallback на HTTP API BiXBiT, если TCP 4028 недоступен
    s.lastError = "";
    String body; int code = 0;
    uint16_t httpPort = (m.apiPort && m.apiPort != 4028) ? m.apiPort : 80;
    const char* httpUser = m.user.length() ? m.user.c_str() : "admin";
    const char* httpPass = m.pass.length() ? m.pass.c_str() : "123456789";
    bool ok = whatsApiGet(m.ip, httpPort, httpUser, httpPass, "/api/system/status", body, code, 5000);
    logMinerResponse(idx, m, httpPort, "/api/system/status", ok, body);
    if (!ok) {
      body = "";
      ok = whatsApiGet(m.ip, httpPort, httpUser, httpPass, "/api/status", body, code, 5000);
      logMinerResponse(idx, m, httpPort, "/api/status", ok, body);
    }
    if (!ok) {
      s.online = false; s.status = "OFFLINE";
      s.lastError = "Нет ответа TCP:" + String(tcpPort) + "; HTTP:" + String(httpPort) + " код " + String(code);
      return;
    }
    s.online = true; s.lastUpdate = millis();
    if (!parseWhatsStatus(body, s)) {
      s.status = "Данные не распознаны";
      s.lastError = "Ответ BiXBiT HTTP получен, но разбор JSON не удался.";
    }
    String bodyP; int codeP = 0;
    bool okPHttp = whatsApiGet(m.ip, httpPort, httpUser, httpPass, "/api/pools", bodyP, codeP, 4000);
    logMinerResponse(idx, m, httpPort, "/api/pools", okPHttp, bodyP);
    if (okPHttp) parseWhatsPools(bodyP, s);

    // Восстанавливаем кэш плат для HTTP режима, если нужно
    if (s.online && s.chainCount == 0 && prevChainCount > 0) {
      s.chainCount    = prevChainCount;
      s.totalAcn      = prevTotalAcn;
      s.avgChipTempC  = prevAvgChipTemp;
      s.maxChipTempC  = prevMaxChipTemp;
      s.avgBoardTempC = prevAvgBoardTemp;
      s.maxBoardTempC = prevMaxBoardTemp;
      for (uint8_t k = 0; k < 4; k++) s.chains[k] = prevChains[k];
    }
    return;
  }
}

/* ===================== Reboot майнера ===================== */
static void rebootMiner(uint8_t idx) {
  MinerSnapshot snapshot(idx, true);
  MinerConfig& m = snapshot.config;
  MinerStatus& s = snapshot.status;
  if (!m.inUse || !s.rebootPending) return;
  snapshot.publish = true;
  bool ok = false;

  if ((m.type == MT_ANTMINER_S19 || m.type == MT_ANTMINER_L3) && !s.isWhatsminer) {
    ok = antHttpReboot(m.ip,
                       m.user.length() ? m.user.c_str() : "root",
                       m.pass.length() ? m.pass.c_str() : "root");
  } else {
    const char* user = m.user.length() ? m.user.c_str() : "admin";
    const char* pass = m.pass.length() ? m.pass.c_str() : "123456789";
    ok = whatsminerLuCIRestart(m.ip, user, pass);
  }

  s.rebootResult = ok
    ? "✅ Перезагрузка инициирована. Майнер вернётся онлайн через 1-2 минуты."
    : "❌ Ошибка перезагрузки — нет ответа";
  s.rebootTime = millis();
  s.rebootPending = false;
}

/* ======================================================================== */
/*  WIFI + SETUP CAPTIVE PORTAL                                             */
/* ======================================================================== */

static bool checkAuthOrBypass() {
  if (isPortalMode) return true;
  if (!server.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str())) {
    server.requestAuthentication(DIGEST_AUTH, "MinerControl", "Login required");
    return false;
  }
  return true;
}

#include "portal_html.h"

static bool connectWiFiSTA(uint32_t timeoutMs = 25000) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  if (cfg.wifi.useStatic) {
    WiFi.config(cfg.wifi.ip, cfg.wifi.gateway, cfg.wifi.subnet, cfg.wifi.dns1, cfg.wifi.dns2);
  }
  WiFi.begin(cfg.wifi.ssid.c_str(), cfg.wifi.password.c_str());
  Log.print("[WiFi] Подключение к ");
  Log.print(cfg.wifi.ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300); Log.print(".");
  }
  Log.println();
  if (WiFi.status() == WL_CONNECTED) {
    Log.print("[WiFi] OK. IP: "); Log.println(WiFi.localIP());
    Log.print("[WiFi] RSSI: "); Log.println(WiFi.RSSI());
    if (MDNS.begin("miner-control")) Log.println("[mDNS] http://miner-control.local");
    return true;
  }
  Log.println("[WiFi] Подключение не удалось.");
  WiFi.disconnect(true, true);
  return false;
}

static void startAPPortal() {
  isPortalMode = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  IPAddress apip(SETUP_AP_IP);
  WiFi.softAPConfig(apip, apip, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  if (!ok) { Log.println("[AP] Start failed"); delay(1000); ESP.restart(); return; }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apip);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PORTAL_HTML); });
  server.on("/gen_204",   HTTP_GET, []() { server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
  server.on("/generate_204",HTTP_GET,[](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
  server.on("/connecttest.txt",HTTP_GET,[](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
  server.on("/ncsi.txt",HTTP_GET,[](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
  server.on("/hotspot-detect.html",HTTP_GET,[](){ server.send(200,"text/html","<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/captive.apple.com",HTTP_GET,[](){ server.send(200,"text/html","<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>"); });
  server.on("/canonical.html",HTTP_GET,[](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });
  server.on("/fwlink",HTTP_GET,[](){ server.sendHeader("Location","/"); server.send(302,"text/plain",""); });

  server.on("/scan.json", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    if (n < 0) { server.send(503, "text/plain", "Wi-Fi scan failed"); return; }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["ssid"] = WiFi.SSID(i);
      o["rssi"] = WiFi.RSSI(i);
      o["ch"]   = WiFi.channel(i);
      o["enc"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? 1 : 0;
    }
    WiFi.scanDelete();
    String s; serializeJson(doc, s);
    server.send(200, "application/json", s);
  });

  server.on("/savewifi", HTTP_POST, []() {
    String ssid = server.arg("ssid"), password = server.arg("password");
    if (!ssid.length() || ssid.length() > 32 || password.length() > 64) {
      server.send(400, "text/plain", "Invalid SSID/password length"); return;
    }
    if (server.arg("static") == "on") {
      IPAddress ip, gw, mask, dns;
      if (!ip.fromString(server.arg("ip")) || !gw.fromString(server.arg("gw")) ||
          !mask.fromString(server.arg("sn")) || !dns.fromString(server.arg("dns")) ||
          ip == IPAddress(0,0,0,0) || gw == IPAddress(0,0,0,0)) {
        server.send(400, "text/plain", "Invalid static IPv4 configuration"); return;
      }
      uint32_t netmask = 0, addr = 0, gateway = 0;
      for (uint8_t k=0; k<4; ++k) { netmask=(netmask<<8)|mask[k]; addr=(addr<<8)|ip[k]; gateway=(gateway<<8)|gw[k]; }
      uint32_t hosts = ~netmask;
      if (!netmask || hosts < 3 || (hosts & (hosts+1)) ||
          (addr & netmask) != (gateway & netmask) || !(addr & hosts) || (addr & hosts) == hosts) {
        server.send(400, "text/plain", "Invalid subnet or gateway"); return;
      }
    }
    cfg.wifi.ssid     = server.arg("ssid");
    cfg.wifi.password = server.arg("password");
    cfg.wifi.useStatic = server.hasArg("static") && server.arg("static") == "on";
    if (cfg.wifi.useStatic) {
      cfg.wifi.ip.fromString(server.arg("ip"));
      cfg.wifi.gateway.fromString(server.arg("gw"));
      cfg.wifi.subnet.fromString(server.arg("sn"));
      cfg.wifi.dns1.fromString(server.arg("dns"));
      cfg.wifi.dns2 = IPAddress(1,1,1,1);
    }
    cfg.wifi.configured = true;
    cfgSave();
    server.send_P(200, "text/html", PORTAL_OK_HTML);
    delay(3000);
    ESP.restart();
  });

  server.onNotFound([]() {
    String uri = server.uri();
    if (uri != "/" && uri != "/scan.json" && uri != "/savewifi") {
      server.sendHeader("Location","http://192.168.4.1/");
      server.send(302, "text/plain", "");
      return;
    }
    server.send(404, "text/plain", "404");
  });
  server.begin();
}

/* ======================================================================== */
/*  SCAN LOCAL NETWORK FOR MINERS (LAN sweep)                               */
/* ======================================================================== */

struct ScanResult { IPAddress ip; MinerType type; uint16_t port; String name; };
static std::vector<ScanResult> scanBuf;
static volatile bool scanRunning = false;

static bool probeTcp4028(const IPAddress& ip, MinerType& outType, String& outName, uint16_t port = 4028, uint32_t to = 550) {
  WiFiClient c;
  if (!c.connect(ip, port, to)) return false;
  c.print("{\"command\":\"summary\"}\n"); c.flush();
  String b = ""; uint32_t t = millis();
  while (millis() - t < 650) {
    while (c.available()) { b += (char)c.read(); if (b.length() > 1024) break; }
    if (b.indexOf("}") >= 0 && !c.available()) break;
    delay(2);
  }
  c.stop();
  if (b.indexOf("STATUS") < 0 && b.indexOf("SUMMARY") < 0) return false;
  if (b.indexOf("Miner Type") >= 0 || b.indexOf("Factory GHS") >= 0 || b.indexOf("PSU ") >= 0) {
    outType = MT_WHATSMINER_BIXBIT;
    outName = "WhatsMiner (порт 4028)";
  } else {
    outType = MT_ANTMINER_S19;
    outName = "Antminer (порт 4028)";
  }
  return true;
}

static bool probeWhatsminerHttp(const IPAddress& ip, uint16_t port = 80, uint32_t to = 550) {
  WiFiClient c;
  if (!c.connect(ip, port, to)) return false;
  String req = "GET /api/system/status HTTP/1.1\r\nHost: "+ip.toString()+"\r\nAuthorization: Basic YWRtaW46MTIzNDU2Nzg5\r\nConnection: close\r\n\r\n";
  c.print(req); c.flush();
  String b = ""; uint32_t t = millis();
  while (millis() - t < 750) {
    if (c.available()) { b += (char)c.read(); }
    delay(2);
  }
  c.stop();
  return b.indexOf("hashrate") >= 0 || b.indexOf("WhatsMiner") >= 0 || b.indexOf("BiXBiT") >= 0;
}

static void scanNetworkTask(void* param) {
  IPAddress my = WiFi.localIP();
  Log.println("[SCAN] Начало сканирования подсети /24 ...");

  for (uint8_t h = 1; h < 255; h++) {
    if (WiFi.status() != WL_CONNECTED) break;
    if (h == my[3]) continue;
    IPAddress ip(my[0], my[1], my[2], h);
    MinerType detType = MT_ANTMINER_S19;
    String detName;
    if (probeTcp4028(ip, detType, detName, 4028, 500)) {
      ScanResult r; r.ip = ip; r.type = detType; r.port = 4028; r.name = detName;
      { LockGuard lock(scanMutex); if (scanBuf.size() < 64) scanBuf.push_back(r); }
    }
    else if (probeWhatsminerHttp(ip, 80, 500)) {
      ScanResult r; r.ip = ip; r.type = MT_WHATSMINER_BIXBIT; r.port = 80;
      r.name = "Whatsminer BiXBiT (HTTP)";
      { LockGuard lock(scanMutex); if (scanBuf.size() < 64) scanBuf.push_back(r); }
    }
    taskYIELD();
  }
  Log.printf("[SCAN] Завершено. Найдено %d майнеров.\n", (int)scanBuf.size());
  { LockGuard lock(scanMutex); scanRunning = false; }
  vTaskDelete(NULL);
}

/* ======================================================================== */
/*  WEB PAGES: MAIN DASHBOARD (STA Mode)                                    */
/* ======================================================================== */
#include "index_html.h"

void handleRoot() {
  if (!checkAuthOrBypass()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiStatus() {
  if (!checkAuthOrBypass()) return;
  server.sendHeader("Cache-Control", "no-store");
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    MinerConfig m;
    MinerStatus s;
    { LockGuard lock(stateMutex); m = cfg.miners[i]; s = minerState[i]; }
    if (!m.inUse) continue;
    JsonObject o = list.add<JsonObject>();
    o["idx"] = i;
    o["type"] = (uint8_t)m.type;
    o["ip"]   = m.ip.toString();
    o["port"] = m.apiPort;
    o["name"] = m.name.length() ? m.name : String("Miner #") + String(i+1);
    bool connected = WiFi.status() == WL_CONNECTED;
    o["online"] = s.online && connected;
    o["status"] = connected ? s.status : String("Нет Wi-Fi");
    o["error"]  = connected ? s.lastError : String("ESP не подключен к сети.");
    o["errorMsg"] = s.errorMsg;
    o["miner_model"] = s.minerModel;
    o["hashrate_th"] = s.hashrateTHs;
    o["hashrate_5s_th"] = s.hashrate5sTHs;
    o["hashrate_avg_th"] = s.hashrateAvgTHs;
    o["rate_30m_th"] = s.rate30mTHs;
    o["ideal_ths"] = s.idealTHs;
    o["freq_avg"] = s.freqAvg;
    o["total_acn"] = s.totalAcn;
    o["power_w"] = s.powerW;
    o["chip_temp_avg"] = s.avgChipTempC;
    o["chip_temp_max"] = s.maxChipTempC;
    o["board_temp"] = s.avgBoardTempC;
    o["board_temp_max"] = s.maxBoardTempC;
    o["uptime"] = s.uptimeSec;
    o["accepted"] = s.acceptedShares;
    o["rejected"] = s.rejectedShares;
    o["stale_shares"] = s.staleShares;
    o["hw_errors"] = s.hwErrors;
    o["rejected_rate_pct"] = s.rejectedRatePct;
    o["pool1_user"] = s.pool1User;
    o["pool2_user"] = s.pool2User;
    o["last_update_ms"] = s.lastUpdate;
    o["reboot_pending"] = s.rebootPending;
    o["reboot_result"] = s.rebootResult;

    JsonArray fa = o["fans"].to<JsonArray>();
    for (uint8_t k = 0; k < s.fanCount; k++) fa.add(s.fanRpm[k]);

    JsonArray ca = o["chains"].to<JsonArray>();
    for (uint8_t k = 0; k < s.chainCount; k++) {
      JsonObject co = ca.add<JsonObject>();
      co["id"]        = s.chains[k].id;
      co["acn"]       = s.chains[k].acn;
      co["rate_gh"]   = s.chains[k].rateGHs;
      co["hw"]        = s.chains[k].hw;
      co["freq"]      = s.chains[k].freq;
      co["temp_chip"] = s.chains[k].tempChip;
      co["temp_pcb"]  = s.chains[k].tempPcb;
      co["bad_asic"]  = s.chains[k].badAsic;
    }

    JsonArray pa = o["pools"].to<JsonArray>();
    for (uint8_t k = 0; k < s.poolCount; k++) {
      JsonObject po = pa.add<JsonObject>();
      po["url"]      = s.pools[k].url;
      po["user"]     = s.pools[k].user;
      po["status"]   = s.pools[k].status;
      po["active"]   = s.pools[k].active;
      po["diff"]     = s.pools[k].diff;
      po["accepted"] = s.pools[k].accepted;
      po["rejected"] = s.pools[k].rejected;
      po["stale"]    = s.pools[k].stale;
    }

    o["is_whatsminer"] = s.isWhatsminer;
    if (s.isWhatsminer) {
      o["psu_name"]       = s.psuName;
      o["mem_usage"]      = s.memUsage;
      o["no_fee_penalty"] = s.noFeePenalty;
      o["fan_percent"]    = s.fanPercent;
      o["psu_fan_rpm"]    = s.psuFanRpm;
      o["fan_in_rpm"]     = s.fanInRpm;
      o["fan_out_rpm"]    = s.fanOutRpm;
      o["psu_temp0"]      = s.psuTemp0C;
      o["psu_temp1"]      = s.psuTemp1C;
      o["psu_temp2"]      = s.psuTemp2C;
      o["env_temp"]       = s.envTempC;
      o["factory_ths"]    = s.factoryTHs;
      o["psu_vout"]       = s.psuVout;
      o["psu_vin"]        = s.psuVin0;
      o["psu_iout"]       = s.psuIout;
      o["psu_iin"]        = s.psuIin0;
      o["chip_temp_target"]  = s.chipTempTargetC;
      o["chip_temp_protect"] = s.chipTempProtectC;
    }
  }
  doc["esp_ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : (isPortalMode ? WiFi.softAPIP().toString() : "");
  doc["esp_rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["uptime_ms"] = millis();
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleApiConfig() {
  if (!checkAuthOrBypass()) return;
  server.sendHeader("Cache-Control", "no-store");
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    MinerConfig m;
    { LockGuard lock(stateMutex); m = cfg.miners[i]; }
    if (!m.inUse) continue;
    JsonObject o = list.add<JsonObject>();
    o["idx"] = i;
    o["type"] = (uint8_t)m.type;
    o["name"] = m.name;
    o["ip"]   = m.ip.toString();
    o["port"] = m.apiPort;
    o["user"] = m.user;
    o["has_pass"] = m.pass.length() > 0;
  }
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleApiMinerSave() {
  if (!checkAuthOrBypass()) return;
  LockGuard stateLock(stateMutex);
  server.sendHeader("Cache-Control", "no-store");
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  if (server.arg("plain").length() > 2048) { server.send(413, "text/plain", "body too large"); return; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, server.arg("plain"));
  if (e) { server.send(400, "text/plain", "bad json"); return; }

  if (!d.is<JsonObject>() || !d["idx"].is<int>() || !d["type"].is<int>() ||
      !d["port"].is<int>() || !d["ip"].is<const char*>() ||
      !d["name"].is<const char*>() || !d["user"].is<const char*>() ||
      (!d["pass"].isNull() && !d["pass"].is<const char*>())) {
    server.send(400, "text/plain", "invalid fields"); return;
  }
  int idx = d["idx"].as<int>(), type = d["type"].as<int>(), port = d["port"].as<int>();
  IPAddress ip;
  String name = d["name"], user = d["user"], pass = d["pass"] | "";
  if (idx < -1 || idx >= MAX_MINERS || type < 1 || type > 3 || port < 0 || port > 65535 ||
      !ip.fromString(d["ip"].as<String>()) || ip[0] == 0 || ip[0] == 127 || ip[0] >= 224 ||
      name.length() > 96 || user.length() > 128 || pass.length() > 128) {
    server.send(400, "text/plain", "invalid miner configuration"); return;
  }
  if (idx >= 0 && !cfg.miners[idx].inUse) { server.send(404, "text/plain", "miner not found"); return; }
  if (idx == -1) idx = minerFreeSlot();
  if (idx == 0xFF) { server.send(507, "text/plain", "max miners reached"); return; }
  MinerConfig& m = cfg.miners[idx];
  m.inUse = true; m.type = (MinerType)type; m.name = name;
  m.ip = ip; m.apiPort = port; m.user = user;
  if (d["pass"].is<const char*>()) m.pass = pass;
  ++minerGeneration[idx];
  minerState[idx] = MinerStatus();
  cfgSave();
  server.send(200, "application/json", "{\"ok\":true}");
}

static int uriIdx(const String& uri) {
  int slash = uri.lastIndexOf('/');
  if (slash < 0) return -1;
  String tail = uri.substring(slash + 1);
  if (!tail.length() || tail.length() > 2) return -1;
  for (size_t n = 0; n < tail.length(); ++n) if (tail[n] < '0' || tail[n] > '9') return -1;
  int idx = tail.toInt();
  return idx < MAX_MINERS ? idx : -1;
}

void handleApiMinerDelete() {
  if (!checkAuthOrBypass()) return;
  LockGuard stateLock(stateMutex);
  server.sendHeader("Cache-Control", "no-store");
  int i = uriIdx(server.uri());
  if (i < 0 || i >= MAX_MINERS || !cfg.miners[i].inUse) { server.send(400, "text/plain", "bad idx"); return; }
  ++minerGeneration[i];
  { LockGuard lock(logMutex); if (s_logMinerFilter == i) s_logMinerFilter = -1; }
  cfg.miners[i] = MinerConfig();
  minerState[i] = MinerStatus();
  cfgSave();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiRebootMiner() {
  if (!checkAuthOrBypass()) return;
  LockGuard stateLock(stateMutex);
  server.sendHeader("Cache-Control", "no-store");
  int i = uriIdx(server.uri());
  if (i < 0 || i >= MAX_MINERS || !cfg.miners[i].inUse) { server.send(400, "application/json", "{\"ok\":false,\"msg\":\"bad idx\"}"); return; }
  if (!s_pollTaskStarted) { server.send(503, "text/plain", "worker unavailable"); return; }
  if (minerState[i].rebootPending) { server.send(409, "text/plain", "reboot already queued"); return; }
  minerState[i].rebootPending = true;
  minerState[i].rebootResult  = "Отправка команды...";
  minerState[i].rebootTime    = millis();
  server.send(202, "application/json", "{\"ok\":true,\"msg\":\"reboot command queued\"}");
}

void handleApiScanStart() {
  if (!checkAuthOrBypass()) return;
  if (isPortalMode || WiFi.status() != WL_CONNECTED) { server.send(503, "text/plain", "Wi-Fi unavailable"); return; }
  if (WiFi.subnetMask() != IPAddress(255,255,255,0)) {
    server.send(400, "text/plain", "Auto scan supports /24; add miners manually"); return;
  }
  LockGuard lock(scanMutex);
  if (scanRunning) { server.send(200, "application/json", "{\"running\":true}"); return; }
  scanBuf.clear();
  scanRunning = true;
  if (xTaskCreate(scanNetworkTask, "scan", 4096, NULL, 1, NULL) != pdPASS) {
    scanRunning = false; server.send(503, "text/plain", "scan task failed"); return;
  }
  server.send(200, "application/json", "{\"running\":true}");
}

void handleApiScanState() {
  if (!checkAuthOrBypass()) return;
  std::vector<ScanResult> results;
  bool running;
  { LockGuard lock(scanMutex); results = scanBuf; running = scanRunning; }
  server.sendHeader("Cache-Control", "no-store");
  JsonDocument doc;
  doc["running"] = running;
  doc["count"]   = (uint32_t)results.size();
  JsonArray arr = doc["results"].to<JsonArray>();
  for (auto& it : results) {
    JsonObject o = arr.add<JsonObject>();
    o["ip"]   = it.ip.toString();
    o["port"] = it.port;
    o["type"] = (uint8_t)it.type;
    o["name"] = it.name;
  }
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

static uint32_t passwordRestartAtMs = 0;

static bool validNewWebPassword(const String& value) {
  if (value.length() < 8 || value.length() > 64) return false;
  for (size_t i = 0; i < value.length(); ++i)
    if ((uint8_t)value[i] < 32 || (uint8_t)value[i] == 127) return false;
  return true;
}

static int storeNewWebPassword(const String& current, const String& next, const String& confirm) {
  if (current != cfg.webPass) return 403;
  if (!validNewWebPassword(next) || next != confirm || next == current) return 400;
  if (!prefs.begin("minerctl", false)) return 500;
  size_t written = prefs.putString("w_pwd", next);
  prefs.end();
  if (written != next.length()) return 500;
  cfg.webPass = next;
  return 200;
}

void handleApiWebPassword() {
  if (!checkAuthOrBypass()) return;
  server.sendHeader("Cache-Control", "no-store");
  if (isPortalMode) { server.send(403, "text/plain", "Unavailable in setup mode"); return; }
  if (!server.hasArg("plain") || server.arg("plain").length() > 1024) {
    server.send(400, "text/plain", "Invalid request body"); return;
  }
  JsonDocument d;
  if (deserializeJson(d, server.arg("plain")) || !d.is<JsonObject>() ||
      !d["current_password"].is<const char*>() || !d["new_password"].is<const char*>() ||
      !d["confirm_password"].is<const char*>()) {
    server.send(400, "text/plain", "Invalid password fields"); return;
  }
  String current = d["current_password"].as<String>();
  String next = d["new_password"].as<String>();
  String confirm = d["confirm_password"].as<String>();
  int result = storeNewWebPassword(current, next, confirm);
  if (result == 403) { server.send(403, "text/plain", "Неверный текущий пароль"); return; }
  if (result == 400) {
    server.send(400, "text/plain", "Новый пароль: 8–64 байта UTF-8, подтверждение должно совпадать, пароль должен отличаться от текущего"); return;
  }
  if (result != 200) { server.send(500, "text/plain", "Не удалось сохранить пароль"); return; }
  Log.println("[AUTH] Web password changed; controller restart scheduled.");
  server.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
  passwordRestartAtMs = millis() + 1500;
  if (!passwordRestartAtMs) passwordRestartAtMs = 1;
}

void handleApiLogFilter() {
  if (!checkAuthOrBypass()) return;
  server.sendHeader("Cache-Control", "no-store");
  if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain") || server.arg("plain").length() > 128) {
      server.send(400, "text/plain", "Invalid request body"); return;
    }
    JsonDocument d;
    if (deserializeJson(d, server.arg("plain")) || !d["idx"].is<int>()) {
      server.send(400, "text/plain", "Invalid filter"); return;
    }
    int idx = d["idx"].as<int>();
    if (idx < -1 || idx >= MAX_MINERS) { server.send(400, "text/plain", "Invalid miner ID"); return; }
    {
      LockGuard stateLock(stateMutex);
      if (idx >= 0 && !cfg.miners[idx].inUse) { server.send(404, "text/plain", "Miner not found"); return; }
    }
    { LockGuard lock(logMutex); s_logMinerFilter = idx; }
    Log.printf("[LOG] Detailed response filter changed to miner_id=%d (-1=all)\n", idx);
  }
  int filter;
  { LockGuard lock(logMutex); filter = s_logMinerFilter; }
  String result = "{\"idx\":" + String(filter) + ",\"capacity_bytes\":" + String(LOG_RING_SIZE) + "}";
  server.send(200, "application/json", result);
}

void handleApiReset() {
  if (!checkAuthOrBypass()) return;
  prefs.begin("minerctl", false);
  prefs.clear();
  prefs.end();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(1200);
  ESP.restart();
}

/* ======================================================================== */
/*  SETUP + LOOP                                                            */
/* ======================================================================== */

void setup() {
  stateMutex = xSemaphoreCreateMutex();
  scanMutex = xSemaphoreCreateMutex();
  logMutex = xSemaphoreCreateMutex();
  if (!stateMutex || !scanMutex || !logMutex) { delay(1000); ESP.restart(); return; }
  Serial.begin(115200);
  delay(300);
  Log.println();
  Log.println(F("=========================================="));
  Log.println(F(" ESP32-C6-N16  Miner Controller v3"));
  Log.println(F(" Chains + Pools + Smart DOM + Fix S19 Temp"));
  Log.println(F("=========================================="));

  cfgLoad();

  if (!cfg.wifi.configured || cfg.wifi.ssid.length() == 0) {
    Log.println("[BOOT] Режим: AP Captive Portal (первая настройка WiFi).");
    startAPPortal();
    return;
  }

  Log.print("[BOOT] Режим: STA. Пробуем подключиться к ");
  Log.println(cfg.wifi.ssid);
  bool ok = connectWiFiSTA(25000);
  if (!ok) {
    Log.println("[BOOT] STA не удалось — переходим в Setup Portal.");
    startAPPortal();
    return;
  }

  ArduinoOTA.setHostname("miner-control");
  ArduinoOTA.setPassword(cfg.webPass.c_str());
  ArduinoOTA.begin();
  ElegantOTA.begin(&server, cfg.webUser.c_str(), cfg.webPass.c_str());

  server.on("/",               handleRoot);
  server.on("/api/status",     HTTP_GET,  handleApiStatus);
  server.on("/api/config",     HTTP_GET,  handleApiConfig);
  server.on("/api/miner/save", HTTP_POST, handleApiMinerSave);
  server.on("/api/miner/delete", HTTP_POST, handleApiMinerDelete);
  server.on("/api/reboot",     HTTP_POST, handleApiRebootMiner);
  server.on("/api/scan/start", HTTP_POST, handleApiScanStart);
  server.on("/api/scan/state", HTTP_GET,  handleApiScanState);
  server.on("/api/reset",      HTTP_POST, handleApiReset);
  server.on("/api/web/password", HTTP_POST, handleApiWebPassword);
  server.on("/api/logs/filter", HTTP_GET,  handleApiLogFilter);
  server.on("/api/logs/filter", HTTP_POST, handleApiLogFilter);
  server.onNotFound([]() {
    const String& u = server.uri();
    if (u.startsWith("/api/reboot/") && server.method() == HTTP_POST) { handleApiRebootMiner(); return; }
    if (u.startsWith("/api/miner/delete/") && server.method() == HTTP_POST) { handleApiMinerDelete(); return; }
    if (u == "/api/logs" && server.method() == HTTP_GET) {
      if (!checkAuthOrBypass()) return;
      String q = server.arg("download");
      bool asFile = q.length() > 0 && (q == "1" || q == "true" || q == "y" || q == "yes");
      server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
      server.sendHeader("Pragma",        "no-cache");
      if (asFile) {
        uint32_t upS = (uint32_t)(millis() / 1000);
        char fname[96];
        snprintf(fname, sizeof(fname), "esp32-minerctl-uptime%06lus.log", (unsigned long)upS);
        char disp[192];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"; filename*=UTF-8''%s", fname, fname);
        server.sendHeader("Content-Disposition", disp);
      }
      server.setContentLength(CONTENT_LENGTH_UNKNOWN);
      server.send(200, "text/plain; charset=utf-8", "");
      logStreamToClient(asFile);
      server.sendContent("");
      return;
    }
    if (u == "/api/logs/clear" && server.method() == HTTP_POST) {
      if (!checkAuthOrBypass()) return;
      logClear();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
    if (!checkAuthOrBypass()) return;
    server.send(404, "text/plain", "404 Not Found");
  });
  server.begin();
  Log.println("[HTTP] Сервер запущен :80");
  bootReadyAtMs = millis() + 2500;
  firstPollDone = false;
  startPollTask();
}

void loop() {
  if (passwordRestartAtMs) {
    if ((int32_t)(millis() - passwordRestartAtMs) >= 0) ESP.restart();
    delay(1);
    return;
  }
  static uint32_t lastHbtLog = 0;
  uint32_t nowM = millis();
  if ((int32_t)(nowM - lastHbtLog) >= 30000) {
    lastHbtLog = nowM;
    uint32_t upS = nowM / 1000;
    uint32_t dd = upS / 86400, hh = (upS % 86400)/3600, mm = (upS % 3600)/60, ss = upS%60;
    int rssi = 0;
    if (!isPortalMode && WiFi.status() == WL_CONNECTED) rssi = WiFi.RSSI();
    Log.print(F("[HB] Uptime "));
    if (dd) { Log.print(dd); Log.print(F("д ")); }
    if (hh<10) Log.print('0'); Log.print(hh); Log.print(':');
    if (mm<10) Log.print('0'); Log.print(mm); Log.print(':');
    if (ss<10) Log.print('0'); Log.print(ss);
    Log.print(F(" · WiFi RSSI ")); Log.print(rssi); Log.print(F(" dBm · майнеров online: "));
    LockGuard stateLock(stateMutex);
    uint8_t on = 0;
    for (uint8_t i=0; i<MAX_MINERS; i++) if (cfg.miners[i].inUse && minerState[i].online) on++;
    Log.println(on);
  }
  if (isPortalMode) {
    dnsServer.processNextRequest();
  }
  if (!isPortalMode) { ElegantOTA.loop(); ArduinoOTA.handle(); }
  server.handleClient();

  if (!isPortalMode && WiFi.status() != WL_CONNECTED) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > WIFI_RECONNECT_MS) {
      lastTry = millis();
      Log.println("[WiFi] reconnect...");
      WiFi.reconnect();
    }
  }

  if (!isPortalMode && !s_pollTaskStarted) {
    static uint32_t lastRetry = 0;
    if (millis() - lastRetry >= 5000) { lastRetry = millis(); startPollTask(); }
  }
  LockGuard stateLock(stateMutex);
  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    if (minerState[i].rebootPending) continue;
    if (minerState[i].rebootResult.length() > 0 &&
        minerState[i].rebootTime > 0 &&
        (uint32_t)(millis() - minerState[i].rebootTime) > REBOOT_MSG_TTL_MS) {
      minerState[i].rebootResult = "";
      minerState[i].rebootTime   = 0;
    }
    if (minerState[i].rebootResult.length() > 0 &&
        minerState[i].online &&
        minerState[i].rebootTime > 0 &&
        (uint32_t)(millis() - minerState[i].rebootTime) > 30000) {
      minerState[i].rebootResult = "";
      minerState[i].rebootTime   = 0;
    }
  }
}