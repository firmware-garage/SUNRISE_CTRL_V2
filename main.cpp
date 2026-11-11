/*
  Sunrise Light v2
  ESP8266 + WS2815 strip (144 px), green-on-black CRT control page.

  - First boot: no credentials -> opens AP "Sunrise-Setup" with a captive
    portal (WiFiManager). Pick your network there; it's remembered.
  - Page lives at http://sunrise.local (mDNS). The IP is shown on the page too.
  - Time from NTP, US Central with DST.
  - OTA updates via PlatformIO env nodemcuv2_ota, or in a browser at
    http://sunrise.local/update (user "admin", password OTA_PASSWORD).
    Upload .pio/build/nodemcuv2/firmware.bin there.

  LED data: GPIO3 (the RX pin) by default, sent by the I2S DMA engine, so
  WiFi interrupts can't corrupt the timing and the CPU isn't blocked.
  GPIO3 is not a boot strapping pin, so nothing the strip does to the data
  line at power-up can stop the ESP8266 from booting. Serial is TX only
  (logging still works, serial input doesn't). See LED_DATA_GPIO below to go
  back to the old GPIO2 (D4) / UART1 wiring.
*/

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <NeoPixelBus.h>
#include <time.h>

// ---------------- User config ----------------
#define HOSTNAME       "sunrise"                 // http://sunrise.local
#define SETUP_AP_NAME  "Sunrise-Setup"
#define OTA_PASSWORD   "changeme"                // match upload_flags in platformio.ini; also the /update page password
#define UPDATE_USER    "admin"                   // /update page user name
#define TZ_STRING      "CST6CDT,M3.2.0,M11.1.0"  // US Central, DST 2nd Sun Mar to 1st Sun Nov. Change if not in US Central
#define NTP_1          "pool.ntp.org"
#define NTP_2          "time.nist.gov"

const uint16_t NUM_LEDS = 144;
const uint32_t FRAME_MS = 33;                     // ~30 fps; plenty for slow fades

// LED data pin. On the ESP8266, NeoPixelBus can only drive the strip from
// hardware on GPIO1, GPIO2 or GPIO3:
//   3 = RX pin, I2S DMA (default). Not a strapping pin; an input during boot.
//   2 = D4, UART1 (the old wiring). GPIO2 must be HIGH at reset or the chip
//       won't boot, and the boot loader also uses it as a TX output, so the
//       strip can see garbage until setup() runs. Only use it behind a
//       74AHCT125 powered from the same 5 V as the board.
// GPIO1 (TX) is not offered: the ROM prints its 74880-baud boot log there.
#define LED_DATA_GPIO  3

#if LED_DATA_GPIO == 3
  typedef NeoEsp8266DmaWs2812xMethod   LedMethod;
#elif LED_DATA_GPIO == 2
  typedef NeoEsp8266Uart1Ws2812xMethod LedMethod;
#else
  #error "LED_DATA_GPIO must be 3 (RX, DMA) or 2 (D4, UART1)"
#endif

// This strip takes RGB byte order. If red and green come out swapped, change to NeoGrbFeature.
// Ws2812x timing has the 300 us reset the WS2815 needs.
NeoPixelBus<NeoRgbFeature, LedMethod> strip(NUM_LEDS);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

// ---------------- Settings (EEPROM) ----------------
struct Settings {
  uint32_t magic;
  uint8_t  enabled;
  uint8_t  startHour, startMin;
  uint8_t  sunriseMaxPct;
  uint8_t  sunsetMaxPct;
  uint8_t  pad;
  uint16_t sunriseMin, holdMin, rampDownMin, sunsetMin;
};
const uint32_t SETTINGS_MAGIC = 0x53554E32;       // "SUN2"
Settings cfg;

void loadDefaults() {
  //      magic           en  hh mm  srMax ssMax pad  srMin hold ramp ssMin
  cfg = { SETTINGS_MAGIC, 1,  6, 0,  100,  40,   0,   30,   30,  10,  20 };
}

void clampSettings() {
  cfg.enabled       = cfg.enabled ? 1 : 0;
  cfg.startHour     = constrain(cfg.startHour, 0, 23);
  cfg.startMin      = constrain(cfg.startMin, 0, 59);
  cfg.sunriseMaxPct = constrain(cfg.sunriseMaxPct, 1, 100);
  cfg.sunsetMaxPct  = constrain(cfg.sunsetMaxPct, 1, 100);
  cfg.sunriseMin    = constrain(cfg.sunriseMin, 1, 180);
  cfg.holdMin       = constrain(cfg.holdMin, 0, 240);
  cfg.rampDownMin   = constrain(cfg.rampDownMin, 0, 120);
  cfg.sunsetMin     = constrain(cfg.sunsetMin, 1, 180);
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void loadSettings() {
  EEPROM.get(0, cfg);
  if (cfg.magic != SETTINGS_MAGIC) {
    loadDefaults();
    saveSettings();
  }
  clampSettings();
}

// ---------------- State machine ----------------
enum class Mode : uint8_t { Idle, Sunrise, Hold, RampDown, Sunset };
Mode     mode      = Mode::Idle;
uint32_t modeStart = 0;                           // millis() when the current mode began
bool     otaActive = false;

const char* modeName(Mode m) {
  switch (m) {
    case Mode::Sunrise:  return "SUNRISE";
    case Mode::Hold:     return "HOLD";
    case Mode::RampDown: return "RAMP DOWN";
    case Mode::Sunset:   return "SUNSET";
    default:             return "IDLE";
  }
}

uint32_t modeDurationMs(Mode m) {
  switch (m) {
    case Mode::Sunrise:  return cfg.sunriseMin  * 60000UL;
    case Mode::Hold:     return cfg.holdMin     * 60000UL;
    case Mode::RampDown: return cfg.rampDownMin * 60000UL;
    case Mode::Sunset:   return cfg.sunsetMin   * 60000UL;
    default:             return 0;
  }
}

// backdateMs lets a mode start "in the past" (catch-up after a reboot,
// or carrying overshoot into the next mode). Unsigned math handles wrap.
void setMode(Mode m, uint32_t backdateMs = 0) {
  mode = m;
  modeStart = millis() - backdateMs;
}

uint32_t modeElapsed() { return millis() - modeStart; }

float modeProgress() {
  uint32_t dur = modeDurationMs(mode);
  if (dur == 0) return 1.0f;
  uint32_t el = modeElapsed();
  return el >= dur ? 1.0f : (float)el / (float)dur;
}

// Chains through zero-length modes (hold = 0, ramp = 0) in one call.
void advanceMode() {
  while (mode != Mode::Idle) {
    uint32_t dur = modeDurationMs(mode);
    uint32_t el  = modeElapsed();
    if (el < dur) break;
    uint32_t over = el - dur;
    switch (mode) {
      case Mode::Sunrise: setMode(Mode::Hold, over);     break;
      case Mode::Hold:    setMode(Mode::RampDown, over); break;
      default:            setMode(Mode::Idle);           break;
    }
  }
}

// ---------------- Color ----------------
struct Col  { float r, g, b; };
struct Stop { float t; Col c; };

// Palette values are perceptual (pre-gamma).
const Stop SUNRISE_PAL[] = {
  { 0.00f, { 0.20f, 0.00f, 0.35f } },   // deep purple
  { 0.18f, { 0.45f, 0.00f, 0.30f } },   // plum
  { 0.36f, { 0.85f, 0.05f, 0.05f } },   // red
  { 0.55f, { 1.00f, 0.30f, 0.00f } },   // orange
  { 0.75f, { 1.00f, 0.70f, 0.10f } },   // yellow
  { 0.90f, { 1.00f, 0.88f, 0.55f } },   // pale gold
  { 1.00f, { 1.00f, 0.95f, 0.85f } },   // warm white
};

// Blue channel is zero in every stop: no blue wavelengths at night.
const Stop SUNSET_PAL[] = {
  { 0.00f, { 0.45f, 1.00f, 0.00f } },   // yellow-green
  { 0.25f, { 1.00f, 0.85f, 0.00f } },   // amber
  { 0.50f, { 1.00f, 0.45f, 0.00f } },   // orange
  { 0.75f, { 1.00f, 0.15f, 0.00f } },   // red-orange
  { 1.00f, { 0.70f, 0.00f, 0.00f } },   // deep red
};

static inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
static inline float smooth(float x)  { x = clamp01(x); return x * x * (3.0f - 2.0f * x); }

Col lerpCol(const Col& a, const Col& b, float t) {
  return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
}

Col samplePalette(const Stop* s, uint8_t n, float t) {
  t = clamp01(t);
  for (uint8_t i = 1; i < n; i++) {
    if (t <= s[i].t) {
      float span = s[i].t - s[i - 1].t;
      float f = span > 0.0f ? (t - s[i - 1].t) / span : 0.0f;
      return lerpCol(s[i - 1].c, s[i].c, f);
    }
  }
  return s[n - 1].c;
}

// Gamma 2.2 lookup. A LUT because powf() in soft-float would eat the CPU.
uint8_t gammaLut[1024];
void buildGamma() {
  for (int i = 0; i < 1024; i++)
    gammaLut[i] = (uint8_t)(powf(i / 1023.0f, 2.2f) * 255.0f + 0.5f);
}
static inline uint8_t toByte(float v) { return gammaLut[(uint16_t)(clamp01(v) * 1023.0f + 0.5f)]; }

// Both effects are symmetric about the center, so compute half and mirror.
void setPair(uint16_t i, const Col& c, float bright) {
  RgbColor px(toByte(c.r * bright), toByte(c.g * bright), toByte(c.b * bright));
  strip.SetPixelColor(i, px);
  strip.SetPixelColor(NUM_LEDS - 1 - i, px);
}

// p: 0..1 through the sunrise. bright: 0..1 overall level.
// Final frame: white across the middle half, end quarters blending back to purple.
void renderSunrise(float p, float bright) {
  const float c = (NUM_LEDS - 1) * 0.5f;
  const uint8_t n = sizeof(SUNRISE_PAL) / sizeof(SUNRISE_PAL[0]);
  for (uint16_t i = 0; i < (NUM_LEDS + 1) / 2; i++) {
    float d = (c - i) / c;                                   // 1 at the ends, ~0 at center
    float shape = (d <= 0.5f) ? 1.0f : 1.0f - smooth((d - 0.5f) * 2.0f);
    float k = 0.6f * d;                                      // outer pixels start later
    float front = smooth((p - k) / (1.0f - k));              // bloom from the middle
    setPair(i, samplePalette(SUNRISE_PAL, n, shape * front), bright);
  }
}

// q: 0..1 through the sunset. Lit band collapses toward the middle,
// dims, and slides green -> amber -> red. Edges run a little ahead in color.
void renderSunset(float q, float maxBright) {
  const float c = (NUM_LEDS - 1) * 0.5f;
  const uint8_t n = sizeof(SUNSET_PAL) / sizeof(SUNSET_PAL[0]);
  float halfWidth = 1.15f - 1.05f * q;                       // full strip -> small ember
  float bright = maxBright * (1.0f - q);
  for (uint16_t i = 0; i < (NUM_LEDS + 1) / 2; i++) {
    float d = (c - i) / c;
    float mask = clamp01((halfWidth - d) / 0.15f);           // soft edge
    float t = clamp01(q * 1.1f + 0.25f * d);
    setPair(i, samplePalette(SUNSET_PAL, n, t), bright * mask);
  }
}

void renderFrame() {
  float p  = modeProgress();
  float sr = cfg.sunriseMaxPct / 100.0f;
  float ss = cfg.sunsetMaxPct / 100.0f;
  switch (mode) {
    case Mode::Sunrise:  renderSunrise(p, sr * (0.3f + 0.7f * p)); break;
    case Mode::Hold:     renderSunrise(1.0f, sr);                   break;
    case Mode::RampDown: renderSunrise(1.0f, sr * (1.0f - p));      break;
    case Mode::Sunset:   renderSunset(p, ss);                       break;
    default:             strip.ClearTo(RgbColor(0));                break;
  }
}

// ---------------- Schedule ----------------
bool timeValid() { return time(nullptr) > 1700000000; }

int  lastTriggerKey = -1;     // which day's sunrise already ran
bool catchUpChecked = false;  // first valid-time check after boot

void checkSchedule() {
  if (!timeValid()) return;

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);

  long nowSec   = lt.tm_hour * 3600L + lt.tm_min * 60L + lt.tm_sec;
  long startSec = cfg.startHour * 3600L + cfg.startMin * 60L;
  long since    = nowSec - startSec;
  if (since < 0) since += 86400L;

  // Normal trigger window is the first minute. Right after boot, widen it to
  // the whole sunrise + hold so a power blip mid-sunrise picks up where it was.
  long window = 60;
  if (!catchUpChecked) {
    window = (long)(cfg.sunriseMin + cfg.holdMin) * 60L;
    catchUpChecked = true;
  }
  if (!cfg.enabled) return;

  time_t startedAt = now - since;
  struct tm st;
  localtime_r(&startedAt, &st);
  int key = st.tm_year * 400 + st.tm_yday;

  if (since < window && key != lastTriggerKey) {
    lastTriggerKey = key;
    setMode(Mode::Sunrise, (uint32_t)since * 1000UL);
    advanceMode();                             // jumps to HOLD if we're past the ramp
  }
}

// ---------------- Web ----------------
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SUNRISE</title>
<style>
:root{--g:#33ff66;--dim:#1c8a3a;--panel:#020d04}
*{box-sizing:border-box}
html,body{margin:0;background:#000}
body{color:var(--g);font:15px/1.3 "Courier New",Courier,monospace;text-shadow:0 0 4px rgba(51,255,102,.55);
padding:10px 10px;min-height:100vh;min-height:100dvh;background:radial-gradient(ellipse at center,#031a08 0%,#000 75%)}
body::after{content:"";position:fixed;inset:0;pointer-events:none;z-index:9;
background:repeating-linear-gradient(to bottom,rgba(0,0,0,.28) 0,rgba(0,0,0,.28) 1px,transparent 1px,transparent 3px)}
.wrap{max-width:560px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:baseline;gap:8px;margin:0 0 8px}
h1{font-size:18px;letter-spacing:2px;margin:0;font-weight:normal;white-space:nowrap}
.sub{color:var(--dim);font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.cur{display:inline-block;width:.6em;height:1em;background:var(--g);vertical-align:-2px;margin-left:3px;animation:b 1s steps(1) infinite}
@keyframes b{50%{opacity:0}}
fieldset{border:1px solid var(--dim);background:var(--panel);margin:0 0 9px;padding:3px 8px 8px;min-width:0}
legend{padding:0 4px;letter-spacing:1px;font-size:13px}
label{display:block}
.row{display:flex;justify-content:space-between;align-items:center;gap:8px;margin:6px 0}
.row label{display:flex;align-items:center;gap:6px}
.g3{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px;margin:6px 0}
.c2{grid-column:span 2}
.k{display:flex;justify-content:space-between;font-size:12px;color:var(--dim);margin-bottom:2px;white-space:nowrap;overflow:hidden}
.k b{font-weight:normal;color:var(--g)}
.v{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
input[type=number],input[type=time]{width:100%;background:#000;color:var(--g);border:1px solid var(--dim);
font:inherit;padding:5px 5px;text-shadow:inherit;color-scheme:dark}
input[type=time]{width:118px}
input[type=range]{width:100%;margin:2px 0;accent-color:var(--g)}
input[type=checkbox]{-webkit-appearance:none;appearance:none;margin:0;flex:none;width:18px;height:18px;background:#000;border:1px solid var(--g);box-shadow:0 0 4px rgba(51,255,102,.55);cursor:pointer;display:grid;place-content:center}
input[type=checkbox]:checked{background:var(--g);box-shadow:0 0 8px var(--g)}
input[type=checkbox]:checked::after{content:"";width:4px;height:9px;border:solid #000;border-width:0 2px 2px 0;transform:translateY(-1px) rotate(45deg)}
button{background:#000;color:var(--g);border:1px solid var(--g);font:inherit;font-size:12px;letter-spacing:1px;padding:11px 2px;
cursor:pointer;text-shadow:inherit;white-space:nowrap}
button:hover,button:active{background:var(--g);color:#000;text-shadow:none}
.btns{display:flex;gap:6px}.btns button{flex:1;min-width:0}
.save{width:100%;font-size:13px;letter-spacing:2px}
.bar{border:1px solid var(--dim);height:10px;margin-top:6px}
.fill{height:100%;width:0;background:var(--g);box-shadow:0 0 8px var(--g);transition:width .5s}
#msg{min-height:1.25em;margin:4px 0;font-size:12px}
.foot{color:var(--dim);font-size:11px}.foot a{color:var(--dim)}
@media(max-width:350px){body{padding:8px 6px}.k{font-size:10px}button{font-size:11px;letter-spacing:0}fieldset{padding:3px 6px 8px}}
</style></head><body><div class="wrap">
<header><h1>&gt; SUNRISE_CTRL<span class="cur"></span></h1><div class="sub" id="host">sunrise.local</div></header>
<fieldset><legend>STATUS</legend>
<div class="g3">
<div><div class="k">CLOCK</div><div class="v" id="clock">--:--:--</div></div>
<div><div class="k">MODE</div><div class="v" id="mode">----</div></div>
<div><div class="k">REMAINING</div><div class="v" id="remain">--</div></div>
</div>
<div class="bar"><div class="fill" id="fill"></div></div>
</fieldset>
<form id="f">
<fieldset><legend>SUNRISE</legend>
<div class="row"><label for="enabled"><input type="checkbox" id="enabled" name="enabled">ALARM ENABLED</label>
<label for="start">START<input type="time" id="start" name="start" required></label></div>
<div class="g3">
<label><span class="k">DURATION (MIN)</span><input type="number" id="sunriseMin" name="sunriseMin" min="1" max="180"></label>
<label><span class="k">HOLD (MIN)</span><input type="number" id="holdMin" name="holdMin" min="0" max="240"></label>
<label><span class="k">RAMP DN (MIN)</span><input type="number" id="rampDownMin" name="rampDownMin" min="0" max="120"></label>
</div>
<label><span class="k">MAX BRIGHT<b id="sunriseMaxV"></b></span><input type="range" id="sunriseMax" name="sunriseMax" min="1" max="100"></label>
</fieldset>
<fieldset><legend>SUNSET</legend>
<div class="g3">
<label><span class="k">DURATION (MIN)</span><input type="number" id="sunsetMin" name="sunsetMin" min="1" max="180"></label>
<label class="c2"><span class="k">MAX BRIGHT<b id="sunsetMaxV"></b></span><input type="range" id="sunsetMax" name="sunsetMax" min="1" max="100"></label>
</div>
</fieldset>
<button type="submit" class="save">SAVE SETTINGS</button>
</form>
<div id="msg"></div>
<fieldset><legend>CONTROL</legend><div class="btns">
<button onclick="act('sunset')">START SUNSET</button>
<button onclick="act('sunrise')">TEST SUNRISE</button>
<button onclick="act('off')">LIGHTS OFF</button>
</div></fieldset>
<div class="foot"><span id="net"></span> // <a href="#" onclick="wifiReset();return false">RESET WIFI</a></div>
</div>
<script>
const $=id=>document.getElementById(id);
let loaded=false;
function fmt(s){if(!s||s<=0)return'--';const m=Math.floor(s/60),x=s%60;return m+'m '+String(x).padStart(2,'0')+'s';}
function msg(t){const v='> '+t;$('msg').textContent=v;setTimeout(()=>{if($('msg').textContent===v)$('msg').textContent='';},4000);}
['sunriseMax','sunsetMax'].forEach(k=>$(k).addEventListener('input',()=>$(k+'V').textContent=$(k).value+'%'));
async function poll(){
 try{const s=await (await fetch('/api/state')).json();
  $('clock').textContent=s.timeValid?s.clock:'NTP WAIT';
  $('mode').textContent=s.mode;$('remain').textContent=fmt(s.remainSec);
  $('fill').style.width=s.progress+'%';
  $('host').textContent=s.host;$('net').textContent=s.ip+' // RSSI '+s.rssi+' dBm';
  if(!loaded){loaded=true;$('enabled').checked=s.enabled;$('start').value=s.start;
   ['sunriseMin','holdMin','rampDownMin','sunsetMin','sunriseMax','sunsetMax'].forEach(k=>$(k).value=s[k]);
   ['sunriseMax','sunsetMax'].forEach(k=>$(k+'V').textContent=s[k]+'%');}
 }catch(e){$('mode').textContent='LINK LOST';}
}
$('f').addEventListener('submit',async e=>{e.preventDefault();
 try{const r=await fetch('/api/settings',{method:'POST',body:new URLSearchParams(new FormData($('f')))});
  msg(r.ok?'SETTINGS SAVED':'SAVE FAILED');}catch(x){msg('SAVE FAILED');}poll();});
async function act(a,pw){
 try{const b={do:a};if(pw!==undefined)b.pw=pw;
  const r=await fetch('/api/action',{method:'POST',body:new URLSearchParams(b)});
  msg(r.ok?a.toUpperCase()+' OK':(r.status==403?'WRONG PASSWORD':'COMMAND FAILED'));}catch(x){msg('COMMAND FAILED');}poll();}
function wifiReset(){const p=prompt('Forget WiFi and reboot into setup mode?\nOTA password:');if(p)act('wifireset',p);}
poll();setInterval(poll,2000);
</script></body></html>)HTML";

void handleState() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char clock[9] = "--:--:--";
  if (timeValid()) strftime(clock, sizeof(clock), "%H:%M:%S", &lt);

  uint32_t remain = 0;
  int progress = 0;
  if (mode != Mode::Idle) {
    uint32_t dur = modeDurationMs(mode), el = modeElapsed();
    remain   = el < dur ? (dur - el) / 1000UL : 0;
    progress = (int)(modeProgress() * 100.0f + 0.5f);
  }

  String ip = WiFi.localIP().toString();
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"clock\":\"%s\",\"timeValid\":%s,\"mode\":\"%s\",\"progress\":%d,\"remainSec\":%lu,"
    "\"enabled\":%s,\"start\":\"%02u:%02u\",\"sunriseMin\":%u,\"holdMin\":%u,"
    "\"rampDownMin\":%u,\"sunriseMax\":%u,\"sunsetMax\":%u,\"sunsetMin\":%u,"
    "\"ip\":\"%s\",\"host\":\"%s.local\",\"rssi\":%d}",
    clock, timeValid() ? "true" : "false", modeName(mode), progress, (unsigned long)remain,
    cfg.enabled ? "true" : "false", cfg.startHour, cfg.startMin,
    cfg.sunriseMin, cfg.holdMin, cfg.rampDownMin, cfg.sunriseMaxPct, cfg.sunsetMaxPct, cfg.sunsetMin,
    ip.c_str(), HOSTNAME, (int)WiFi.RSSI());
  server.send(200, "application/json", buf);
}

uint16_t argU(const char* name, long lo, long hi, uint16_t cur) {
  if (!server.hasArg(name)) return cur;
  return (uint16_t)constrain(server.arg(name).toInt(), lo, hi);
}

void handleSettings() {
  uint8_t oldH = cfg.startHour, oldM = cfg.startMin;

  if (server.hasArg("start")) {                // "HH:MM" from <input type=time>
    String s = server.arg("start");
    int colon = s.indexOf(':');
    if (colon > 0) {
      cfg.startHour = constrain(s.substring(0, colon).toInt(), 0, 23);
      cfg.startMin  = constrain(s.substring(colon + 1).toInt(), 0, 59);
    }
  }
  cfg.enabled       = server.hasArg("enabled") ? 1 : 0;   // unchecked boxes aren't posted
  cfg.sunriseMin    = argU("sunriseMin", 1, 180, cfg.sunriseMin);
  cfg.holdMin       = argU("holdMin", 0, 240, cfg.holdMin);
  cfg.rampDownMin   = argU("rampDownMin", 0, 120, cfg.rampDownMin);
  cfg.sunriseMaxPct = argU("sunriseMax", 1, 100, cfg.sunriseMaxPct);
  cfg.sunsetMaxPct  = argU("sunsetMax", 1, 100, cfg.sunsetMaxPct);
  cfg.sunsetMin     = argU("sunsetMin", 1, 180, cfg.sunsetMin);

  // New start time: allow it to fire today even if today's already ran (handy for testing).
  if (cfg.startHour != oldH || cfg.startMin != oldM) lastTriggerKey = -1;

  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAction() {
  String a = server.arg("do");
  if      (a == "sunset")  setMode(Mode::Sunset);
  else if (a == "sunrise") setMode(Mode::Sunrise);
  else if (a == "off")     setMode(Mode::Idle);
  else if (a == "wifireset") {
    // Wiping the credentials strands the light in setup-AP mode, so it takes
    // the OTA password. Anyone on the LAN could do it before.
    if (server.arg("pw") != OTA_PASSWORD) {
      server.send(403, "application/json", "{\"ok\":false}");
      return;
    }
    server.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    WiFiManager wm;
    wm.resetSettings();
    ESP.restart();
    return;
  } else {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// Dim green center pixels while the setup portal is open, so you know it's waiting.
void portalIndicator(WiFiManager*) {
  strip.ClearTo(RgbColor(0));
  strip.SetPixelColor(NUM_LEDS / 2 - 1, RgbColor(0, 24, 0));
  strip.SetPixelColor(NUM_LEDS / 2, RgbColor(0, 24, 0));
  strip.Show();
}

// Send an all-off frame and wait until it has gone out.
void blankStrip() {
  strip.ClearTo(RgbColor(0));
  while (!strip.CanShow()) yield();
  strip.Show();
  while (!strip.CanShow()) yield();
}

// ---------------- Setup / loop ----------------
void setup() {
#if LED_DATA_GPIO == 3
  // TX only: GPIO3 belongs to the I2S output. Serial.begin() must come
  // before strip.Begin(), or it takes the pin back and no data goes out.
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
#else
  Serial.begin(115200);
#endif
  Serial.println(F("\nSunrise Light v2"));

  // Blank the strip before anything slow (WiFi can block for seconds).
  // Twice, 50 ms apart: on a shared 12 V supply the strip's chips can still
  // be coming out of undervoltage when the first frame goes out.
  strip.Begin();
  blankStrip();
  delay(50);
  blankStrip();
  buildGamma();

  EEPROM.begin(sizeof(Settings));
  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFiManager wm;
  wm.setHostname(HOSTNAME);
  wm.setConfigPortalTimeout(300);              // 5 min, then reboot and retry
  wm.setAPCallback(portalIndicator);
  if (!wm.autoConnect(SETUP_AP_NAME)) {
    ESP.restart();
  }
  blankStrip();
  Serial.print(F("IP: "));
  Serial.println(WiFi.localIP());

  configTime(TZ_STRING, NTP_1, NTP_2);

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaActive = true;
    blankStrip();
  });
  ArduinoOTA.onError([](ota_error_t) { otaActive = false; });
  ArduinoOTA.begin();                          // also starts mDNS as HOSTNAME.local
  MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/action", HTTP_POST, handleAction);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  httpUpdater.setup(&server, "/update", UPDATE_USER, OTA_PASSWORD);  // browser OTA page
  server.begin();
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  MDNS.update();
  if (otaActive) return;

  static uint32_t lastSched = 0;
  if (millis() - lastSched >= 1000) {
    lastSched = millis();
    checkSchedule();
  }

  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= FRAME_MS && strip.CanShow()) {
    lastFrame = millis();
    advanceMode();
    renderFrame();
    strip.Show();
  }
}
