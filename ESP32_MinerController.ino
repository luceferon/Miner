/*
 * ESP32-C6-N16 Miner Controller v2.1
 * Управление N майнерами (Antminer S19 stock / Whatsminer M50 BiXBiT / др.)
 *
 * v2.1 (Whatsminer BiXBiT):
 *  - корректный парсинг Whatsminer TCP 4028 (MHS av в TH/s, PSU/Env temp,
 *    Fan In/Out, Fan%, PSU V/I, Factory GHS, Miner Type)
 *  - расширенный MinerStatus + вывод в /api/status
 *  - в UI отдельные метки и блок «Модель/PSU» для Whatsminer
 *  - игнорирование ответа STATS "Invalid command"
 *
 * ТРЕБОВАНИЯ:
 *  - Плата:      esp32 by Espressif Systems >= v3.0.5  (Board Manager)
 *                Select Board: ESP32C6 Dev Module
 *                Flash Size: 16MB (опция N16)
 *                Partition Scheme: OTA 8MB (рекомендовано)
 *
 *  - Библиотеки: (Arduino Library Manager)
 *     ArduinoJson   by Benoit BLANCHON  >= v7.2.0
 *     ElegantOTA    by Ayush Sharma     >= v3.1.1
 *     DNSServer     (встроенная в esp32 core)
 *     Preferences   (встроенная в esp32 core)
 */

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

/* ======================================================================== */
/*  ВЕБ-КОНСОЛЬ ЛОГОВ (RING BUFFER 8KB)                                    */
/* ======================================================================== */
#define LOG_RING_SIZE  8192
static __attribute__((aligned(4))) char s_logRing[LOG_RING_SIZE];
static volatile size_t s_logHead = 0;
static volatile size_t s_logTail = 0;

static void logPush(uint8_t c) {
  size_t next = (s_logHead + 1) % LOG_RING_SIZE;
  if (next == s_logTail) s_logTail = (s_logTail + 1) % LOG_RING_SIZE;
  s_logRing[s_logHead] = (char)c;
  s_logHead = next;
}
static void logPushN(const uint8_t* buf, size_t n) {
  for (size_t i=0; i<n; i++) logPush(buf[i]);
}
static void logClear() { s_logHead = s_logTail = 0; }

static void logDump(Print& out) {
  size_t h = s_logHead, t = s_logTail;
  if (h == t) return;
  if (h > t) {
    for (size_t i = t; i < h; i++) out.print(s_logRing[i]);
  } else {
    for (size_t i = t; i < LOG_RING_SIZE; i++) out.print(s_logRing[i]);
    for (size_t i = 0; i < h; i++) out.print(s_logRing[i]);
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
  int val = 0, vbits = -6;
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
  prefs.clear();

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
static uint8_t minerCount() {
  uint8_t c = 0;
  for (uint8_t i = 0; i < MAX_MINERS; i++) if (cfg.miners[i].inUse) c++;
  return c;
}

/* ======================================================================== */
/*  MINER STATUS + RUNTIME STATE                                            */
/* ======================================================================== */

struct MinerStatus {
  bool      online = false;
  uint32_t  lastUpdate = 0;
  String    lastError = "";
  double    hashrateTHs = 0;
  double    hashrate5sTHs = 0;
  double    hashrateAvgTHs = 0;
  uint16_t  avgChipTempC = 0;
  uint16_t  maxChipTempC = 0;
  uint16_t  avgBoardTempC = 0;
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
  double    rejectedRatePct = 0;
  String    pool1User = "";
  String    pool2User = "";
  bool      rebootPending = false;
  String    rebootResult = "";
  uint32_t  rebootTime = 0;

  /* ---- Whatsminer BiXBiT specific (v2.1) ---- */
  bool      isWhatsminer = false;
  String    minerModel = "";
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

/* ======================================================================== */
/*  ANTMINER TCP API 4028                                                   */
/* ======================================================================== */

static bool antTcpCmd(const IPAddress& ip, uint16_t port, const String& cmd, String& out, uint32_t toMs = 2000) {
  WiFiClient c;
  if (!c.connect(ip, port, toMs)) return false;
  StaticJsonDocument<64> doc;
  doc["command"] = cmd;
  String payload;
  serializeJson(doc, payload);
  c.print(payload);
  c.print('\n');
  c.flush();
  delay(6);
  uint32_t t0 = millis();
  String buf = "";
  bool gotData = false;
  while (millis() - t0 < toMs) {
    while (c.available()) {
      buf += (char)c.read();
      gotData = true;
    }
    if (gotData && !c.connected() && !c.available()) break;
    delay(2);
  }
  c.flush();
  c.stop();
  if (buf.length() == 0) return false;
  out = buf;
  return true;
}

static bool extractAntFragment(const String& src, const char* marker, String& out) {
  if (!marker || !marker[0] || src.length() == 0) return false;
  int markerPos = src.indexOf(marker);
  if (markerPos < 0) return false;

  int open = -1, depth = 0;
  for (int i = markerPos - 1; i >= 0; i--) {
    char c = src.charAt(i);
    if (c == '}') depth++;
    else if (c == '{') {
      if (depth == 0) { open = i; break; }
      depth--;
    }
  }
  if (open < 0 || open >= (int)src.length()) return false;

  int close = -1; depth = 0;
  for (int i = open; i < (int)src.length(); i++) {
    char c = src.charAt(i);
    if (c == '{') depth++;
    else if (c == '}') {
      depth--;
      if (depth == 0) { close = i; break; }
    }
  }
  if (close < 0 || close <= open) return false;
  out = src.substring(open, close + 1);
  return true;
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

    o.uptimeSec      = s["Elapsed"] | s["Elapsed (secs)"] | 0;
    o.acceptedShares = s["Accepted"] | s["accepted"] | 0;
    o.rejectedShares = s["Rejected"] | s["rejected"] | 0;
    double a = o.acceptedShares, r = o.rejectedShares;
    o.rejectedRatePct = (a + r) > 0 ? (r / (a + r)) * 100.0 : 0.0;

    /* ================= Whatsminer BiXBiT specific =================
       Признак: есть ключ "Miner Type" / "Factory GHS" / "Fan Speed Percent".
       Значения "MHS av" у Whatsminer УЖЕ в TH/s (напр. 125.74 = 125.74 TH/s),
       в отличие от Antminer, где "MHS av" в MH/s.
    */
    if (s.containsKey("Miner Type") || s.containsKey("Factory GHS") ||
        s.containsKey("Fan Speed Percent")) {
      o.isWhatsminer = true;

      if (s.containsKey("Miner Type"))    o.minerModel = s["Miner Type"].as<String>();
      if (s.containsKey("Status"))        o.status     = s["Status"].as<String>();
      if (s.containsKey("Factory GHS"))   o.factoryTHs = s["Factory GHS"].as<double>() / 1000.0;

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

      /* "MHS av" у Whatsminer — это TH/s, а не MH/s.*/
      if (s.containsKey("MHS av")) {
        double v = s["MHS av"].as<double>();
        if (v > 0) {
          o.hashrateAvgTHs = v;
          o.hashrateTHs    = v;
          o.hashrate5sTHs  = v;
        }
      }

      /* Соберём вентиляторы Whatsminer: In, Out, PSU */
      o.fanCount = 0;
      if (o.fanInRpm  && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)o.fanInRpm;
      if (o.fanOutRpm && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)o.fanOutRpm;
      if (o.psuFanRpm && o.fanCount < 4) o.fanRpm[o.fanCount++] = (uint16_t)o.psuFanRpm;
    }
  }
  return true;
}

static bool parseAntStats(const String& json, MinerStatus& o) {
  String body;
  extractAntFragment(json, "\"STATS\":", body);
  if (body.length() == 0) body = json;
  JsonDocument d; DeserializationError e = deserializeJson(d, body);
  if (e) return false;
  if (!d["STATS"][1].isNull()) {
    auto s = d["STATS"][1];
    JsonObject obj = s.as<JsonObject>();
    uint32_t tSum = 0, tMax = 0, tCnt = 0, bSum = 0, bCnt = 0;
    o.fanCount = 0;
    for (JsonPair kv : obj) {
      const char* k = kv.key().c_str();
      bool isChipTemp = false;
      bool isBoardTemp = false;
      if (strncmp(k, "temp", 4) == 0 && strlen(k) > 4 && !strstr(k, "in") && !strstr(k, "out") && !strstr(k, "_")) {
        isChipTemp = true;
      } else if (strstr(k, "temp2_") || strstr(k, "temp_in") || strstr(k, "temp_out")) {
        isBoardTemp = true;
      }
      else if (strstr(k, "chip_temp") || strstr(k, "temp_chip") || strstr(k, "ChipTemp") || strstr(k, "chiptemp")) isChipTemp = true;
      else if (strstr(k, "board_temp") || strstr(k, "temp_board") || strstr(k, "BoardTemp") || strstr(k, "boardtemp")) isBoardTemp = true;
      else if (strstr(k, "pcb_temp") || strstr(k, "temp_pcb") || strstr(k, "pcbtemp")) isBoardTemp = true;
      else if (strstr(k, "avg_temp") && strstr(k, "chip")) isChipTemp = true;
      else if (strstr(k, "max_temp") && strstr(k, "chip")) isChipTemp = true;

      if (isChipTemp) {
        int t = kv.value().as<int>();
        if (t > 0) { tSum += t; if ((uint32_t)t > tMax) tMax = t; tCnt++; }
      } else if (isBoardTemp) {
        int t = kv.value().as<int>();
        if (t > 0) { bSum += t; bCnt++; }
      } else if (
        (strncmp(k, "fan", 3) == 0 && isDigit(k[3])) ||
        strstr(k, "fan_speed") || strstr(k, "fan_rpm") || strstr(k, "FanSpeed") ||
        strstr(k, "fan1_speed") || strstr(k, "fan2_speed") || strstr(k, "fan3_speed") || strstr(k, "fan4_speed") ||
        strstr(k, "Fan1") || strstr(k, "Fan2") || strstr(k, "Fan3") || strstr(k, "Fan4")
      ) {
        int rpm = kv.value().as<int>();
        if (rpm > 0 && o.fanCount < 4) o.fanRpm[o.fanCount++] = rpm;
      }
    }
    o.avgChipTempC   = tCnt > 0 ? tSum / tCnt : 0;
    o.maxChipTempC   = tMax;
    o.avgBoardTempC  = bCnt > 0 ? bSum / bCnt : 0;
  }
  return true;
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
    while (c.available()) resp += (char)c.read();
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

static bool httpRecon(const IPAddress& ip, const char* u, const char* p, const char* path) {
  WiFiClient c;
  if (!c.connect(ip, 80, 3000)) { Log.println("   recon:80 connect fail"); return false; }
  String req = "GET "; req += path; req += " HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  if (u && *u) req += "Authorization: " + basicAuthHeader(u, p) + "\r\n";
  req += "Accept: */*\r\n";
  req += "User-Agent: MinerCtl/2.0\r\nConnection: close\r\n\r\n";
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
    else delay(3);
  }
  c.flush(); c.stop();
  Log.printf("   recon %s -> HTTP %d  head=%d  body=%d  auth=%s\r\n",
                path, code, head.length(), body.length(),
                authHeader.length() ? authHeader.c_str() : "<none>");
  if (body.length()) {
    String s = body; s.replace("\r","\\r"); s.replace("\n","\\n");
    if (s.length() > 520) s = s.substring(0, 520);
    Log.print("   body: "); Log.println(s);
  }
  return (code >= 200 && code < 400);
}

static bool httpRawGet(const IPAddress& ip, const String& urlPath, const char* u, const char* p, uint32_t toMs=6000) {
  WiFiClient c;
  if (!c.connect(ip, 80, (uint16_t)(toMs/2))) return false;
  String req = "GET " + urlPath + " HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  req += "Authorization: " + basicAuthHeader(u, p) + "\r\n";
  req += "Referer: http://" + ip.toString() + "/\r\n";
  req += "User-Agent: MinerCtl/2.0\r\n";
  req += "Connection: close\r\n\r\n";
  c.print(req);
  uint32_t t0 = millis();
  int code = 0;
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String s = c.readStringUntil('\n');
      s.trim();
      if (s.startsWith("HTTP/1.")) {
        int sp = s.indexOf(' ');
        if (sp > 0) code = s.substring(sp+1, s.indexOf(' ', sp+1)).toInt();
      }
    } else if (!c.connected()) {
      break;
    } else {
      delay(5);
    }
  }
  c.stop();
  return (code >= 200 && code < 400);
}
static bool httpRawPost(const IPAddress& ip, const String& urlPath, const char* u, const char* p,
                      const String& contentType, const String& body, uint32_t toMs=7000) {
  WiFiClient c;
  if (!c.connect(ip, 80, (uint16_t)(toMs/2))) return false;
  String req = "POST " + urlPath + " HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  req += "Authorization: " + basicAuthHeader(u, p) + "\r\n";
  req += "Referer: http://" + ip.toString() + "/\r\n";
  req += "Content-Type: " + contentType + "\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "User-Agent: MinerCtl/2.0\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;
  c.print(req);
  uint32_t t0 = millis();
  int code = 0;
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String s = c.readStringUntil('\n');
      s.trim();
      if (s.startsWith("HTTP/1.")) {
        int sp = s.indexOf(' ');
        if (sp > 0) code = s.substring(sp+1, s.indexOf(' ', sp+1)).toInt();
      }
    } else if (!c.connected()) {
      break;
    } else {
      delay(5);
    }
  }
  c.stop();
  return (code >= 200 && code < 400);
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
  __attribute__((aligned(4))) static uint8_t sbuf[128];
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
    memset(sbuf, 0, 56);
    for (int i=0;i<8;i++) sbuf[56+i] = (uint8_t)((bitLen >> (8*i)) & 0xff);
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
           "User-Agent: MinerCtl/2.0\r\n"
           "Accept: */*\r\n"
           "Connection: close\r\n\r\n",
           ipstr);
  c.print(req);
  String auth; int code = 0; uint32_t t0=millis();
  while (millis()-t0 < toMs) {
    if (c.available()) {
      String line = c.readStringUntil('\n');
      line.trim();
      if (line.startsWith("HTTP/1.")) {
        int sp=line.indexOf(' ');
        if (sp>0) code = line.substring(sp+1, line.indexOf(' ',sp+1)).toInt();
      }
      if (line.startsWith("WWW-Authenticate:")) {
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
  static uint32_t s_cnt;
  uint64_t mac = ESP.getEfuseMac();
  uint32_t seed = (uint32_t)((mac ^ (mac>>32)) + (s_cnt++ * 2654435761U));
  static const char hex[]="0123456789abcdef";
  for (int i=0;i<8;i++){
    out17[i*2]   = hex[(seed>>(28-4*i))&0xf];
    out17[i*2+1] = hex[(seed>>(28-4*i-4))&0xf];
  }
  out17[16] = '\0';
}

static void buildDigestHeaderRaw(const char* ipstr, const char* method, const char* path,
                                 const DigestCtx& d, const char* u, const char* p,
                                 uint32_t nc, const char cnonce17[17],
                                 char out[768]) {
  char realmB[128];  strncpy(realmB,  d.realm.c_str(),  sizeof(realmB)-1);  realmB[sizeof(realmB)-1]='\0';
  char nonceB[128];  strncpy(nonceB,  d.nonce.c_str(),  sizeof(nonceB)-1);  nonceB[sizeof(nonceB)-1]='\0';
  char qopB[32];     strncpy(qopB,    d.qop.c_str(),    sizeof(qopB)-1);    qopB[sizeof(qopB)-1]='\0';
  char opaqueB[128]; strncpy(opaqueB, d.opaque.c_str(), sizeof(opaqueB)-1); opaqueB[sizeof(opaqueB)-1]='\0';

  char ha1[64]; snprintf(ha1,sizeof(ha1),"%s:%s:%s",u,realmB,p);
  char ha1h[33]; md5HexStr(ha1, (int)strlen(ha1), ha1h);

  char ha2[64]; snprintf(ha2,sizeof(ha2),"%s:%s",method,path);
  char ha2h[33]; md5HexStr(ha2, (int)strlen(ha2), ha2h);

  char ncs[16]; snprintf(ncs,sizeof(ncs),"%08x",(unsigned)nc);
  char resp[256];
  if (strlen(qopB) > 0) {
    snprintf(resp,sizeof(resp),"%s:%s:%s:%s:%s:%s",ha1h,nonceB,ncs,cnonce17,qopB,ha2h);
  } else {
    snprintf(resp,sizeof(resp),"%s:%s:%s",ha1h,nonceB,ha2h);
  }
  char resh[33]; md5HexStr(resp, (int)strlen(resp), resh);

  int w = 0;
  w += snprintf(out+w, 768-w, "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", ",
                u, realmB, nonceB, path);
  if (strlen(opaqueB) > 0)
    w += snprintf(out+w, 768-w, "opaque=\"%s\", ", opaqueB);
  if (strlen(qopB) > 0)
    w += snprintf(out+w, 768-w, "qop=%s, nc=%s, cnonce=\"%s\", ", qopB, ncs, cnonce17);
  snprintf(out+w, 768-w, "response=\"%s\"", resh);
  (void)ipstr;
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
  Log.print(F("   [digest] realm=")); Log.print(d.realm);
  Log.print(F(" nonce=")); Log.print(d.nonce);
  Log.print(F(" qop="));   Log.println(d.qop);

  WiFiClient c;
  if (!c.connect(ip, 80, toMs/2)) {
    if (outAuthErr) *outAuthErr="connect fail to port 80";
    return -1;
  }

  char ipstr[20]; snprintf(ipstr,sizeof(ipstr),"%d.%d.%d.%d",ip[0],ip[1],ip[2],ip[3]);
  uint32_t nc = 1;
  char cnonce17[17]; genCnonce16(cnonce17);
  char authHdr[768];
  buildDigestHeaderRaw(ipstr, method, urlPath, d, u, p, nc, cnonce17, authHdr);

  __attribute__((aligned(4))) static char buf[2048];
  int w = 0;
  w += snprintf(buf+w, sizeof(buf)-w, "%s %s HTTP/1.1\r\n", method, urlPath);
  w += snprintf(buf+w, sizeof(buf)-w, "Host: %s\r\n", ipstr);
  w += snprintf(buf+w, sizeof(buf)-w, "Authorization: %s\r\n", authHdr);
  w += snprintf(buf+w, sizeof(buf)-w, "Referer: http://%s/\r\n", ipstr);
  w += snprintf(buf+w, sizeof(buf)-w, "User-Agent: MinerCtl/2.0\r\n");
  w += snprintf(buf+w, sizeof(buf)-w, "Accept: */*\r\n");
  if (bodyLen > 0 && contentType) {
    w += snprintf(buf+w, sizeof(buf)-w, "Content-Type: %s\r\n", contentType);
    w += snprintf(buf+w, sizeof(buf)-w, "Content-Length: %u\r\n", (unsigned)bodyLen);
  }
  w += snprintf(buf+w, sizeof(buf)-w, "Connection: close\r\n\r\n");
  if (bodyLen > 0 && bodyData && (w + (int)bodyLen) < (int)sizeof(buf)) {
    memcpy(buf+w, bodyData, bodyLen);
    w += (int)bodyLen;
  }
  c.write((const uint8_t*)buf, (size_t)w);

  uint32_t t0 = millis();
  int code = 0;
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String line = c.readStringUntil('\n');
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

// Универсальный HTTPS GET (Basic Auth ИЛИ Cookie). Возвращает HTTP-код или -1.
static int httpsRequestGet(const IPAddress& ip, uint16_t port, const String& path,
                           const char* user, const char* pass, const char* cookie,
                           String* outHead = nullptr, uint32_t toMs = 6000) {
  WiFiClientSecure c;
  c.setInsecure();                      // самоподписанный сертификат майнера
  if (!c.connect(ip, port, toMs / 2)) return -1;

  String req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  if (cookie && *cookie) {
    req += "Cookie: " + String(cookie) + "\r\n";
  } else if (user && *user) {
    req += "Authorization: " + basicAuthHeader(user, pass ? pass : "") + "\r\n";
  }
  req += "User-Agent: MinerCtl/2.0\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: close\r\n\r\n";
  c.print(req);

  String head; int code = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String line = c.readStringUntil('\n');
      line.trim();
      if (line.startsWith("HTTP/1.")) {
        int sp = line.indexOf(' ');
        if (sp > 0) code = line.substring(sp + 1, line.indexOf(' ', sp + 1)).toInt();
      }
      head += line; head += "\n";
      if (line.length() == 0 && code != 0) break;
    } else if (!c.connected()) break;
    else delay(5);
  }
  c.stop();
  if (outHead) *outHead = head;
  return code;
}

// LuCI-логин: POST /cgi-bin/luci с luci_username/luci_password → cookie sysauth_https
static String httpsLuciLogin(const IPAddress& ip, uint16_t port,
                             const char* user, const char* pass, uint32_t toMs = 6000) {
  WiFiClientSecure c;
  c.setInsecure();
  if (!c.connect(ip, port, toMs / 2)) return "";

  String body = "luci_username=" + String(user) + "&luci_password=" + String(pass);
  String req = "POST /cgi-bin/luci HTTP/1.1\r\n";
  req += "Host: " + ip.toString() + "\r\n";
  req += "Content-Type: application/x-www-form-urlencoded\r\n";
  req += "Content-Length: " + String(body.length()) + "\r\n";
  req += "User-Agent: MinerCtl/2.0\r\n";
  req += "Connection: close\r\n\r\n";
  req += body;
  c.print(req);

  String cookie;
  uint32_t t0 = millis();
  while (millis() - t0 < toMs) {
    if (c.available()) {
      String line = c.readStringUntil('\n');
      line.trim();
      if (line.startsWith("Set-Cookie:") || line.startsWith("Set-Cookie")) {
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

  // --- Шаг 1: Basic Auth по HTTPS ---
  {
    String head;
    int code = httpsRequestGet(ip, httpsPort, restartPath, user, pass, nullptr, &head, 6000);
    Log.print(F("[RBT-WM] 1) Basic-HTTPS code=")); Log.println(code);
    if (code >= 200 && code < 400) {
      Log.println(F("[RBT-WM] LuCI restart OK (Basic)"));
      return true;
    }
  }

  // --- Шаг 2: LuCI session cookie (POST /cgi-bin/luci → sysauth_https) ---
  {
    String cookie = httpsLuciLogin(ip, httpsPort, user, pass, 6000);
    if (cookie.length()) {
      Log.print(F("[RBT-WM] 2) got cookie: ")); Log.println(cookie);
      String head;
      int code = httpsRequestGet(ip, httpsPort, restartPath, nullptr, nullptr,
                                 cookie.c_str(), &head, 6000);
      Log.print(F("[RBT-WM] 2) Cookie-HTTPS code=")); Log.println(code);
      if (code >= 200 && code < 400) {
        Log.println(F("[RBT-WM] LuCI restart OK (Cookie)"));
        return true;
      }
    } else {
      Log.println(F("[RBT-WM] 2) LuCI login failed (no Set-Cookie in response)"));
    }
  }

  Log.println(F("[RBT-WM] LuCI restart FAIL — see log above for HTTP codes"));
  return false;
}

static bool antHttpReboot(const IPAddress& ip, const char* u, const char* p) {
  Log.print(F("[RBT] ")); Log.print(ip.toString());
  Log.print(F(" user='")); Log.print(u && *u ? u : "<empty>");
  Log.println('\'');

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

  if (u && *u) {
    Log.println(F("[RBT] === recon (с пользовательскими кредами) ==="));
    httpRecon(ip, u, p, "/");
    httpRecon(ip, u, p, "/cgi-bin/reboot.cgi");
  } else {
    Log.println(F("[RBT] === recon (cred root:root, креды не заданы) ==="));
    httpRecon(ip, "root", "root", "/");
    httpRecon(ip, "root", "root", "/cgi-bin/reboot.cgi");
  }
  Log.println(F("[RBT] === end recon ==="));

  bool ok = false;
  {
    Log.print(F("[RBT] > TCP 4028 command:reboot param:[true] ... "));
    String pt[] = {"true"};
    if (tcp4028Cmd(ip, "reboot", pt, 1, 4000)) { ok = true; Log.println(F("OK")); }
  }

  if (!ok) {
    const char* pathsGet[9]; uint8_t np = 0;
    pathsGet[np++] = "/cgi-bin/reboot.cgi";
    pathsGet[np++] = "/cgi-bin/reboot_system.cgi";
    pathsGet[np++] = "/cgi-bin/reboot_system";
    pathsGet[np++] = "/cgi-bin/luci/admin/system/reboot";
    pathsGet[np++] = "/cgi-bin/reboot.cgi?action=go";
    pathsGet[np++] = "/cgi-bin/reboot.cgi?action=reboot";
    pathsGet[np++] = "/reboot";
    pathsGet[np++] = "/api/reboot";
    pathsGet[np++] = "/admin/system/reboot";
    for (uint8_t ci = 0; ci < ncred && !ok; ci++) {
      String err;
      for (uint8_t pi = 0; pi < np && !ok; pi++) {
        Log.print(F("[RBT] > Digest GET ")); Log.print(pathsGet[pi]);
        Log.print(F("  cred=")); Log.print(creds[ci].u); Log.print(':');
        Log.print(creds[ci].p); Log.print(F(" ... "));
        int code = httpDigestGet(ip, pathsGet[pi], creds[ci].u, creds[ci].p, &err);
        bool loc = (code >= 200 && code < 400);
        Log.print(F("code=")); Log.print(code); Log.print(' ');
        if (loc) Log.println(F("OK"));
        else if (err.length()) Log.println(err);
        else Log.println(F("fail"));
        if (loc) ok = true;
        delay(300);
      }
      if (!ok) {
        const char* bodyF = "cbi.submit=1&system.reboot=1&apply=Reboot";
        Log.print(F("[RBT] > Digest POST /cgi-bin/luci/admin/system/reboot cred="));
        Log.print(creds[ci].u); Log.print(':'); Log.print(creds[ci].p);
        Log.print(F(" ... "));
        int code = httpDigestPost(ip, "/cgi-bin/luci/admin/system/reboot", creds[ci].u, creds[ci].p,
                                  "application/x-www-form-urlencoded",
                                  (const uint8_t*)bodyF, strlen(bodyF), &err);
        bool loc = (code >= 200 && code < 400);
        Log.print(F("code=")); Log.print(code); Log.print(' ');
        if (loc) Log.println(F("OK"));
        else if (err.length()) Log.println(err);
        else Log.println(F("fail"));
        if (loc) ok = true;
        delay(300);
      }
      if (!ok) {
        const char* bodyJ = "{\"reboot\":true}";
        Log.print(F("[RBT] > Digest POST /api/system/reboot cred="));
        Log.print(creds[ci].u); Log.print(':'); Log.print(creds[ci].p);
        Log.print(F(" ... "));
        int code = httpDigestPost(ip, "/api/system/reboot", creds[ci].u, creds[ci].p,
                                  "application/json",
                                  (const uint8_t*)bodyJ, strlen(bodyJ), &err);
        bool loc = (code >= 200 && code < 400);
        Log.print(F("code=")); Log.print(code); Log.print(' ');
        if (loc) Log.println(F("OK"));
        else if (err.length()) Log.println(err);
        else Log.println(F("fail"));
        if (loc) ok = true;
        delay(300);
      }
    }
  }

  if (ok) Log.println(F("[RBT] === RESULT: REBOOT CMD SENT OK ==="));
  else    Log.println(F("[RBT] === RESULT: ALL METHODS FAILED ==="));
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
  return (codeOut >= 200 && codeOut < 400);
}
static bool parseWhatsStatus(const String& json, MinerStatus& o) {
  JsonDocument d; DeserializationError e = deserializeJson(d, json);
  if (e) return false;
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
    if (!x["fans"].isNull()) {
      JsonArray fans = x["fans"];
      o.fanCount = 0;
      for (size_t i = 0; i < fans.size() && i < 4; i++)
        o.fanRpm[o.fanCount++] = fans[i] | 0;
    } else {
      for (int i = 1; i <= 4; i++) {
        String k = "fan" + String(i);
        if (!x[k].isNull() && o.fanCount < 4)
          o.fanRpm[o.fanCount++] = x[k] | 0;
      }
    }
    if (!x["pools"].isNull()) {
      JsonArray pools = x["pools"];
      if (pools.size() > 0) o.pool1User = pools[0]["user"] | pools[0]["Worker"] | "";
      if (pools.size() > 1) o.pool2User = pools[1]["user"] | pools[1]["Worker"] | "";
    }
  } else {
    o.hashrateTHs = d["hashrate_th"] | 0;
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
static bool whatsApiReboot(const IPAddress& ip, uint16_t port, const char* u, const char* p) {
  HTTPClient h; WiFiClient c;
  String url = "http://" + ip.toString() + ":" + String(port) + "/api/system/reboot";
  if (!h.begin(c, url)) return false;
  h.addHeader("Authorization", basicAuthHeader(u, p));
  h.addHeader("Accept", "application/json");
  h.addHeader("Content-Type", "application/json");
  h.setTimeout(8000);
  int code = h.POST("{\"reboot\":true}");
  h.end();
  return (code >= 200 && code < 400);
}

/* ======================================================================== */
/*  MINER POLLING + REBOOT DISPATCHER                                       */
/* ======================================================================== */

static const uint32_t STATUS_INTERVAL_MS = 5000;
static const uint32_t WIFI_RECONNECT_MS = 3000;
static const uint32_t REBOOT_MSG_TTL_MS = 180000;   // 3 минуты
volatile uint32_t lastStatusTick = 0;
bool isPortalMode = false;
static bool firstPollDone = false;
static uint32_t bootReadyAtMs = 0;

volatile bool s_pollBusy = false;
volatile bool s_pollTaskStarted = false;

static void pollMiner(uint8_t idx);

static void pollMinerTask(void* pvParam) {
  (void)pvParam;
  Log.println("[POLL] Фоновая задача опроса майнеров запущена (core 1).");
  for (;;) {
    if (isPortalMode) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
    uint32_t now = millis();
    if (!firstPollDone) {
      if ((int32_t)(now - bootReadyAtMs) < 0) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
      firstPollDone = true;
      Log.println("[BOOT] Первый опрос майнеров...");
      s_pollBusy = true;
      for (uint8_t i = 0; i < MAX_MINERS; i++) {
        if (cfg.miners[i].inUse) {
          pollMiner(i);
          taskYIELD();
          vTaskDelay(pdMS_TO_TICKS(250));
        }
      }
      s_pollBusy = false;
      lastStatusTick = now;
      continue;
    }
    if (now - lastStatusTick >= STATUS_INTERVAL_MS) {
      lastStatusTick = now;
      s_pollBusy = true;
      for (uint8_t i = 0; i < MAX_MINERS; i++) {
        if (cfg.miners[i].inUse) {
          pollMiner(i);
          taskYIELD();
          vTaskDelay(pdMS_TO_TICKS(50));
        }
      }
      s_pollBusy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static void startPollTask() {
  if (s_pollTaskStarted) return;
  s_pollTaskStarted = true;
  BaseType_t ok = xTaskCreate(pollMinerTask, "pollMiner", 8192, NULL, 2, NULL);
  if (ok != pdPASS) Log.println("[POLL] ERROR: xTaskCreate FAILED — опрос останется в loop()");
}

static void pollMiner(uint8_t idx) {
  MinerConfig& m = cfg.miners[idx];
  MinerStatus&  s = minerState[idx];
  if (!m.inUse) return;

  s.lastError = "";

  IPAddress zero(0, 0, 0, 0);
  if (!isPortalMode && (WiFi.status() != WL_CONNECTED || WiFi.localIP() == zero)) {
    s.online = false; s.status = "Нет Wi-Fi";
    s.lastError = "ESP не подключен к сети.";
    return;
  }
  if (m.ip == zero) {
    s.online = false; s.status = "IP не задан";
    s.lastError = "IP адрес майнера не настроен — сохраните заново в Настройки.";
    s.lastUpdate = millis();
    return;
  }

  if (m.type == MT_ANTMINER_S19 || m.type == MT_ANTMINER_L3) {
    String sum, stats, pools;
    bool okS = false, okSt = false, okP = false;
    okS  = antTcpCmd(m.ip, m.apiPort ? m.apiPort : 4028, "summary", sum);
    okSt = antTcpCmd(m.ip, m.apiPort ? m.apiPort : 4028, "stats",   stats);
    okP  = antTcpCmd(m.ip, m.apiPort ? m.apiPort : 4028, "pools",   pools);
    Log.print(F("===== [TCP4028 SUMMARY RAW miner#")); Log.print(idx); Log.print(F("] len=")); Log.print((int)sum.length()); Log.println(F(" ====="));
    if (sum.length()) { Log.println(sum); }
    Log.println(F("----- end summary -----"));
    Log.print(F("===== [TCP4028 STATS RAW miner#")); Log.print(idx); Log.print(F("] len=")); Log.print((int)stats.length()); Log.println(F(" ====="));
    if (stats.length()) { Log.println(stats); }
    Log.println(F("----- end stats -----"));
    Log.print(F("===== [TCP4028 POOLS RAW miner#")); Log.print(idx); Log.print(F("] len=")); Log.print((int)pools.length()); Log.println(F(" ====="));
    if (pools.length()) { Log.println(pools); }
    Log.println(F("----- end pools -----"));
    if (!okS && !okSt) {
      s.online = false; s.status = "OFFLINE";
      s.lastError = "Нет связи с TCP API " + m.ip.toString() + ":" + String(m.apiPort ? m.apiPort : 4028);
      return;
    }
    s.online = true; s.lastUpdate = millis();
    bool parsed = false;
    if (okS)  if (parseAntSummary(sum, s)) parsed = true;
    if (okSt) if (parseAntStats  (stats, s)) parsed = true;
    if (okP)  parseAntPools(pools, s);
    if (!parsed) {
      s.lastError = "Ответ от майнера получен, но поля данных не распознаны (Antminer API fragment).";
      s.status = "Данные не распознаны";
    }
    return;
  }

  if (m.type == MT_WHATSMINER_BIXBIT) {
    uint16_t tcpPort = m.apiPort ? m.apiPort : 4028;
    String sum, stats, pools;
    bool okS = false, okSt = false, okP = false;
    okS  = antTcpCmd(m.ip, tcpPort, "summary", sum);
    okSt = antTcpCmd(m.ip, tcpPort, "stats",   stats);
    okP  = antTcpCmd(m.ip, tcpPort, "pools",   pools);
    Log.print(F("===== [TCP4028 SUMMARY RAW miner#")); Log.print(idx); Log.print(F("] len=")); Log.print((int)sum.length()); Log.println(F(" ====="));
    if (sum.length()) { Log.println(sum); }
    Log.println(F("----- end summary -----"));
    Log.print(F("===== [TCP4028 STATS RAW miner#")); Log.print(idx); Log.print(F("] len=")); Log.print((int)stats.length()); Log.println(F(" ====="));
    if (stats.length()) { Log.println(stats); }
    Log.println(F("----- end stats -----"));
    Log.print(F("===== [TCP4028 POOLS RAW miner#")); Log.print(idx); Log.print(F("] len=")); Log.print((int)pools.length()); Log.println(F(" ====="));
    if (pools.length()) { Log.println(pools); }
    Log.println(F("----- end pools -----"));
    if (okS || okSt) {
      s.online = true; s.lastUpdate = millis();
      bool parsed = false;
      if (okS)  if (parseAntSummary(sum, s)) parsed = true;
      if (okSt) if (parseAntStats  (stats, s)) parsed = true;
      if (okP)  parseAntPools(pools, s);
      if (!parsed) {
        s.lastError = "TCP 4028 ответ получен, но поля не распознаны (WhatsMiner cgminer format).";
        s.status = "Данные не распознаны";
      }
      return;
    }
    String body; int code = 0;
    uint16_t httpPort = m.apiPort ? m.apiPort : 80;
    bool ok = whatsApiGet(m.ip, httpPort, m.user.c_str(), m.pass.c_str(), "/api/system/status", body, code, 5000);
    if (!ok) ok = whatsApiGet(m.ip, httpPort, m.user.c_str(), m.pass.c_str(), "/api/status", body, code, 5000);
    if (!ok) {
      s.online = false; s.status = "OFFLINE";
      s.lastError = "Нет связи с WhatsMiner (TCP 4028 + HTTP BiXBiT " + String(code) + ", IP " + m.ip.toString() + ")";
      return;
    }
    s.online = true; s.lastUpdate = millis();
    if (!parseWhatsStatus(body, s)) {
      s.lastError = "Ответ BiXBiT HTTP получен, но разбор JSON не удался (проверьте прошивку).";
    }
    String bodyP; int codeP = 0;
    if (whatsApiGet(m.ip, httpPort, m.user.c_str(), m.pass.c_str(), "/api/pools", bodyP, codeP, 4000)) {
      parseWhatsPools(bodyP, s);
    }
    return;
  }
}

/* ===================== Reboot майнера ===================== */
static void rebootMiner(uint8_t idx) {
  MinerConfig& m = cfg.miners[idx];
  MinerStatus&  s = minerState[idx];
  bool ok = false;

  if (m.type == MT_ANTMINER_S19 || m.type == MT_ANTMINER_L3) {
    // Antminer:
    ok = antHttpReboot(m.ip,
                       m.user.length() ? m.user.c_str() : "root",
                       m.pass.length() ? m.pass.c_str() : "root");
  } else if (m.type == MT_WHATSMINER_BIXBIT) {
    // WhatsMiner BiXBiT: перезагрузка через LuCI CGI (Digest Auth)
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

DNSServer dnsServer;
WebServer server(80);

static bool checkAuthOrBypass() {
  if (isPortalMode) return true;
  if (!server.authenticate(cfg.webUser.c_str(), cfg.webPass.c_str())) {
    server.requestAuthentication(DIGEST_AUTH, "MinerControl", "Login required");
    return false;
  }
  return true;
}

/* ======================================================================== */
/*  WEB PAGES: PORTAL (WiFi Setup)                                          */
/*  Редактировать в: portal_html.h                                          */
/* ======================================================================== */
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
  WiFi.mode(WIFI_AP);
  IPAddress apip(SETUP_AP_IP);
  WiFi.softAPConfig(apip, apip, IPAddress(255, 255, 255, 0));
  bool ok = WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD);
  Log.print("[AP] Точка доступа поднята: ");
  Log.print(SETUP_AP_SSID);
  Log.print(" / pass: ");
  Log.println(SETUP_AP_PASSWORD);
  Log.print("[AP] Captive portal IP: ");
  Log.println(WiFi.softAPIP());

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

static bool probeAntminer(const IPAddress& ip, uint16_t port = 4028, uint32_t to = 600) {
  WiFiClient c;
  if (!c.connect(ip, port, to)) return false;
  c.print("{\"command\":\"version\"}\n"); c.flush();
  String b = ""; uint32_t t = millis();
  while (millis() - t < 700) {
    if (c.available()) { b += (char)c.read(); if (b.endsWith("}")) break; }
    delay(2);
  }
  c.stop();
  return (b.indexOf("STATUS") >= 0 || b.length() > 8);
}
static bool probeWhatsminer(const IPAddress& ip, uint16_t port = 80, uint32_t to = 600) {
  WiFiClient c;
  if (!c.connect(ip, port, to)) return false;
  String req = "GET /api/system/status HTTP/1.1\r\nHost: "+ip.toString()+"\r\nAuthorization: Basic YWRtaW46MTIzNDU2Nzg5\r\nConnection: close\r\n\r\n";
  c.print(req); c.flush();
  String b = ""; uint32_t t = millis();
  while (millis() - t < 800) {
    if (c.available()) { b += (char)c.read(); }
    delay(2);
  }
  c.stop();
  return (b.indexOf("HTTP/1.1 200") >= 0 || b.indexOf("HTTP/1.1 401") >= 0 || b.indexOf("hashrate") >= 0);
}

static void scanNetworkTask(void* param) {
  scanRunning = true;
  scanBuf.clear();
  IPAddress my = WiFi.localIP();
  Log.println("[SCAN] Начало сканирования подсети /24 ...");

  for (uint8_t h = 1; h < 255; h++) {
    if (h == my[3]) continue;
    IPAddress ip(my[0], my[1], my[2], h);
    if (probeAntminer(ip, 4028, 500)) {
      ScanResult r; r.ip = ip; r.type = MT_ANTMINER_S19; r.port = 4028;
      r.name = "Antminer (порт 4028)";
      scanBuf.push_back(r);
    }
    if (probeWhatsminer(ip, 80, 500)) {
      ScanResult r; r.ip = ip; r.type = MT_WHATSMINER_BIXBIT; r.port = 80;
      r.name = "Whatsminer BiXBiT (HTTP)";
      scanBuf.push_back(r);
    }
    taskYIELD();
  }
  Log.printf("[SCAN] Завершено. Найдено %d майнеров.\n", (int)scanBuf.size());
  scanRunning = false;
  vTaskDelete(NULL);
}


/* ======================================================================== */
/*  WEB PAGES: MAIN DASHBOARD (STA Mode)                                    */
/*  Редактировать в: index_html.h                                           */
/* ======================================================================== */
#include "index_html.h"

/* === HANDLERS === */
void handleRoot() {
  if (!checkAuthOrBypass()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiStatus() {
  if (!checkAuthOrBypass()) return;
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    if (!cfg.miners[i].inUse) continue;
    MinerConfig& m = cfg.miners[i];
    MinerStatus&  s = minerState[i];
    JsonObject o = list.add<JsonObject>();
    o["idx"] = i;
    o["type"] = (uint8_t)m.type;
    o["ip"]   = m.ip.toString();
    o["port"] = m.apiPort;
    o["name"] = m.name.length() ? m.name : String("Miner #") + String(i+1);
    o["online"] = s.online;
    o["status"] = s.status;
    o["error"]  = s.lastError;
    o["errorMsg"] = s.errorMsg;
    o["hashrate_th"] = s.hashrateTHs;
    o["hashrate_5s_th"] = s.hashrate5sTHs;
    o["hashrate_avg_th"] = s.hashrateAvgTHs;
    o["power_w"] = s.powerW;
    o["chip_temp_avg"] = s.avgChipTempC;
    o["chip_temp_max"] = s.maxChipTempC;
    o["board_temp"] = s.avgBoardTempC;
    o["uptime"] = s.uptimeSec;
    o["accepted"] = s.acceptedShares;
    o["rejected"] = s.rejectedShares;
    o["rejected_rate_pct"] = s.rejectedRatePct;
    o["pool1_user"] = s.pool1User;
    o["pool2_user"] = s.pool2User;
    o["last_update_ms"] = s.lastUpdate;
    o["reboot_pending"] = s.rebootPending;
    o["reboot_result"] = s.rebootResult;
    JsonArray fa = o["fans"].to<JsonArray>();
    for (uint8_t k = 0; k < s.fanCount; k++) fa.add(s.fanRpm[k]);

    /* ---- Whatsminer BiXBiT extras (v2.1) ---- */
    o["is_whatsminer"] = s.isWhatsminer;
    if (s.isWhatsminer) {
      o["miner_model"]    = s.minerModel;
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
  JsonDocument doc;
  JsonArray list = doc["list"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    if (!cfg.miners[i].inUse) continue;
    MinerConfig& m = cfg.miners[i];
    JsonObject o = list.add<JsonObject>();
    o["idx"] = i;
    o["type"] = (uint8_t)m.type;
    o["name"] = m.name;
    o["ip"]   = m.ip.toString();
    o["port"] = m.apiPort;
    o["user"] = m.user;
    o["pass"] = m.pass;
  }
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

void handleApiMinerSave() {
  if (!checkAuthOrBypass()) return;
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "no body"); return; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, server.arg("plain"));
  if (e) { server.send(400, "text/plain", "bad json"); return; }

  int idx = d["idx"] | -1;
  MinerConfig* m = nullptr;
  if (idx >= 0 && idx < MAX_MINERS && cfg.miners[idx].inUse) {
    m = &cfg.miners[idx];
  } else {
    uint8_t freeIdx = minerFreeSlot();
    if (freeIdx == 0xFF) { server.send(507, "text/plain", "max miners reached"); return; }
    m = &cfg.miners[freeIdx];
    m->inUse = true;
  }
  m->type    = (MinerType)(d["type"] | 1);
  m->name    = d["name"] | "";
  String ip  = d["ip"] | "";
  m->ip.fromString(ip);
  m->apiPort = d["port"] | 0;
  m->user    = d["user"] | "";
  m->pass    = d["pass"] | "";
  minerState[m - cfg.miners] = MinerStatus();
  cfgSave();
  server.send(200, "application/json", "{\"ok\":true}");
}

static int uriIdx(const String& uri) {
  int slash = uri.lastIndexOf('/');
  if (slash < 0) return -1;
  return uri.substring(slash + 1).toInt();
}

void handleApiMinerDelete() {
  if (!checkAuthOrBypass()) return;
  int i = uriIdx(server.uri());
  if (i < 0 || i >= MAX_MINERS || !cfg.miners[i].inUse) { server.send(400, "text/plain", "bad idx"); return; }
  cfg.miners[i] = MinerConfig();
  minerState[i] = MinerStatus();
  cfgSave();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleApiRebootMiner() {
  if (!checkAuthOrBypass()) return;
  int i = uriIdx(server.uri());
  if (i < 0 || i >= MAX_MINERS || !cfg.miners[i].inUse) { server.send(400, "application/json", "{\"ok\":false,\"msg\":\"bad idx\"}"); return; }
  minerState[i].rebootPending = true;
  minerState[i].rebootResult  = "Отправка команды...";
  minerState[i].rebootTime    = millis();
  server.send(202, "application/json", "{\"ok\":true,\"msg\":\"reboot command queued\"}");
}

void handleApiScanStart() {
  if (!checkAuthOrBypass()) return;
  if (isPortalMode) { server.send(503, "text/plain", "Только в домашней сети"); return; }
  if (scanRunning) { server.send(200, "application/json", "{\"running\":true}"); return; }
  xTaskCreate(scanNetworkTask, "scan", 4096, NULL, 1, NULL);
  server.send(200, "application/json", "{\"running\":true}");
}

void handleApiScanState() {
  if (!checkAuthOrBypass()) return;
  JsonDocument doc;
  doc["running"] = scanRunning;
  doc["count"]   = (uint32_t)scanBuf.size();
  JsonArray arr = doc["results"].to<JsonArray>();
  for (auto& it : scanBuf) {
    JsonObject o = arr.add<JsonObject>();
    o["ip"]   = it.ip.toString();
    o["port"] = it.port;
    o["type"] = (uint8_t)it.type;
    o["name"] = it.name;
  }
  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
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
  Serial.begin(115200);
  delay(300);
  Log.println();
  Log.println(F("=========================================="));
  Log.println(F(" ESP32-C6-N16  Miner Controller v2.1"));
  Log.println(F(" NVS config + Captive Portal + N miners"));
  Log.println(F(" Whatsminer BiXBiT full field parser"));
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

  ArduinoOTA.setHostname("esp32-miner-ota");
  ArduinoOTA.setPassword(cfg.webPass.c_str());
  ArduinoOTA.onStart([](){Log.println("[OTA] Update...");});
  ArduinoOTA.onProgress([](unsigned int p,unsigned int t){Log.printf("[OTA] %u%%\r",(p*100)/t);});
  ArduinoOTA.onEnd([](){Log.println("\n[OTA] Done.");});
  ArduinoOTA.onError([](ota_error_t e){Log.printf("[OTA] err %u\n",e);});
  ArduinoOTA.begin();
  ElegantOTA.begin(&server, cfg.webUser.c_str(), cfg.webPass.c_str());

  server.on("/",                  handleRoot);
  server.on("/api/status",        handleApiStatus);
  server.on("/api/config",        handleApiConfig);
  server.on("/api/miner/save",    HTTP_POST, handleApiMinerSave);
  server.on("/api/miner/delete", HTTP_POST, handleApiMinerDelete);
  server.on("/api/reboot",    HTTP_POST, handleApiRebootMiner);
  server.on("/api/scan/start",    handleApiScanStart);
  server.on("/api/scan/state",    handleApiScanState);
  server.on("/api/reset",         HTTP_POST, handleApiReset);
  server.onNotFound([]() {
    const String& u = server.uri();
    if (u.startsWith("/api/reboot/") && server.method() == HTTP_POST) { handleApiRebootMiner(); return; }
    if (u.startsWith("/api/miner/delete/") && server.method() == HTTP_POST) { handleApiMinerDelete(); return; }
    if (u == "/api/logs") {
      if (!checkAuthOrBypass()) return;
      String q = server.arg("download");
      bool asFile = q.length() > 0 && (q == "1" || q == "true" || q == "y" || q == "yes");
      server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
      server.sendHeader("Pragma",        "no-cache");
      static String s_payload;
      s_payload = "";
      s_payload.reserve(LOG_RING_SIZE + 8);
      if (asFile) s_payload += "\xEF\xBB\xBF";
      class StringPrint : public Print {
      public:
        String& s;
        StringPrint(String& ss): s(ss) {}
        virtual size_t write(uint8_t c) override { s += (char)c; return 1; }
        virtual size_t write(const uint8_t* b, size_t n) override {
          size_t old = s.length();
          s.concat((const char*)b, n);
          return s.length() - old;
        }
      };
      { StringPrint sp(s_payload); logDump(sp); }
      if (asFile) {
        uint32_t upS = (uint32_t)(millis() / 1000);
        char fname[96];
        snprintf(fname, sizeof(fname), "esp32-minerctl-uptime%06lus.log", (unsigned long)upS);
        char disp[192];
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"; filename*=UTF-8''%s", fname, fname);
        server.sendHeader("Content-Disposition", disp);
      }
      server.send(200, "text/plain; charset=utf-8", s_payload);
      return;
    }
    if (u == "/api/logs/clear") {
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
    uint8_t on = 0;
    for (uint8_t i=0; i<MAX_MINERS; i++) if (cfg.miners[i].inUse && minerState[i].online) on++;
    Log.println(on);
  }
  if (isPortalMode) {
    dnsServer.processNextRequest();
  }
  ElegantOTA.loop();
  ArduinoOTA.handle();
  server.handleClient();

  if (!isPortalMode && WiFi.status() != WL_CONNECTED) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > WIFI_RECONNECT_MS) {
      lastTry = millis();
      Log.println("[WiFi] reconnect...");
      WiFi.reconnect();
    }
  }

  for (uint8_t i = 0; i < MAX_MINERS; i++) {
    // Обработка очереди перезагрузки
    if (minerState[i].rebootPending) {
      rebootMiner(i);
      continue;   // на этом цикле ничего не чистим
    }

    // Автоочистка сообщения о перезагрузке
    // Условие: сообщение есть, время запуска задано, и с момента запуска прошло TTL
    if (minerState[i].rebootResult.length() > 0 &&
        minerState[i].rebootTime > 0 &&
        (uint32_t)(millis() - minerState[i].rebootTime) > REBOOT_MSG_TTL_MS) {
      minerState[i].rebootResult = "";
      minerState[i].rebootTime   = 0;
    }

    // Дополнительно: если майнер уже снова онлайн и после перезагрузки прошло >30 сек — 
    // очищаем сообщение досрочно (чтобы не мозолило глаза, когда всё в порядке)
    if (minerState[i].rebootResult.length() > 0 &&
        minerState[i].online &&
        minerState[i].rebootTime > 0 &&
        (uint32_t)(millis() - minerState[i].rebootTime) > 30000) {
      minerState[i].rebootResult = "";
      minerState[i].rebootTime   = 0;
    }
  }
}
