
/*
 * Desk Buddy + Web control - ESP32-C3 Mini
 *  - Pan servo (base, 0-180°)   : GPIO1
 *  - Tilt servo (head, 10° swing): GPIO2
 *  - SSD1306 128x64 I2C OLED    : SDA GPIO8, SCL GPIO9
 *
 * Libraries: Adafruit SSD1306, Adafruit GFX, ESP32Servo (v3.x)
 * Board option: USB CDC On Boot = Enabled
 *
 * On boot the OLED shows the IP. Open http://<that IP>/ on a phone/laptop
 * on the same Wi-Fi (or try http://deskbuddy.local/).
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

// ---------------- Wi-Fi credentials ----------------
// Copy secrets.example.h to secrets.h and put your Wi-Fi details there.
// secrets.h is git-ignored so real credentials never get committed.
#include "secrets.h"

// Must stay above all functions (Arduino auto-prototype order)
enum Mood { NORMAL, HAPPY, SLEEPY, ANGRY, SAD, SURPRISED, LOVE, WINK, DIZZY };

// ---------------- Pins ----------------
#define PIN_PAN   1
#define PIN_TILT  2
#define PIN_SDA   8
#define PIN_SCL   9

// ---------------- Wi-Fi ----------------
#define WIFI_TIMEOUT_MS 15000
#define WIFI_LOW_TX     0      // set 1 if the C3 Super Mini won't connect
#define WIFI_RETRY_MS   10000  // reconnect attempt interval after a drop

// ---------------- Display ----------------
#define SCR_W     128
#define SCR_H     64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCR_W, SCR_H, &Wire, -1);

// ---------------- Servo limits ----------------
const int PAN_MIN = 0, PAN_MAX = 180, PAN_CENTER = 90;
const int PAN_HOME = PAN_CENTER;          // base returns here after every mode
const int TILT_CENTER = 90;               // head neutral - trim to your mechanics
const int TILT_UP     = TILT_CENTER + 5;  // swap signs if head moves the wrong way
const int TILT_DOWN   = TILT_CENTER - 5;  // total travel = 10°
const bool PAN_INVERT = false;            // flip if eyes look opposite to base turn

const uint32_t FRAME_MS = 20;             // 50 fps update

// ---------------- Servo axis with rate limiting ----------------
struct Axis {
  Servo s;
  float cur, tgt, speed = 1.0f;
  int lo, hi, last = -1;

  void begin(int pin, int lo_, int hi_, int start) {
    lo = min(lo_, hi_); hi = max(lo_, hi_);
    cur = tgt = start;
    s.setPeriodHertz(50);
    s.attach(pin, 500, 2400);
    write();
  }
  void set(float t, float sp) { tgt = constrain(t, lo, hi); speed = sp; }
  bool arrived() const { return cur == tgt; }
  void update() {
    float d = tgt - cur;
    if (fabsf(d) <= speed) cur = tgt;
    else cur += (d > 0 ? speed : -speed);
    write();
  }
  void write() {
    int a = lroundf(cur);
    if (a != last) { s.write(a); last = a; }
  }
};
Axis pan, tilt;

// ---------------- Motion queue ----------------
struct Move { int16_t pan, tilt; float pSpd, tSpd; uint16_t hold; };
const uint8_t QSZ = 16;
Move q[QSZ];
uint8_t qHead = 0, qLen = 0;
bool moveStarted = false, holding = false;
uint32_t holdUntil = 0;

void push(int p, int t, float ps, float ts, uint16_t hold) {
  if (qLen >= QSZ) return;
  q[(qHead + qLen) % QSZ] = { (int16_t)p, (int16_t)t, ps, ts, hold };
  qLen++;
}

void clearQueue() {
  qHead = 0;
  qLen = 0;
  moveStarted = false;
  holding = false;
}

void runQueue(uint32_t now) {
  if (!qLen) return;
  Move &m = q[qHead];
  if (!moveStarted) {
    pan.set(m.pan, m.pSpd);
    tilt.set(m.tilt, m.tSpd);
    moveStarted = true;
    holding = false;
    return;
  }
  if (!holding && pan.arrived() && tilt.arrived()) {
    holding = true;
    holdUntil = now + m.hold;
  }
  if (holding && (int32_t)(now - holdUntil) >= 0) {
    qHead = (qHead + 1) % QSZ;
    qLen--;
    moveStarted = false;
  }
}

// ---------------- Eyes ----------------
struct Eyes {
  float w = 36, h = 36, ox = 0, oy = 0;     // current
  float tw = 36, th = 36, tox = 0, toy = 0; // target
  Mood mood = NORMAL;
} eyes;

const int EYE_L_CX = 38, EYE_R_CX = 90;
bool blinking = false;
uint32_t blinkEnd = 0, nextBlink = 0;

void setMood(Mood m) {
  eyes.mood = m;
  switch (m) {
    case NORMAL:    eyes.tw = 36; eyes.th = 36; break;
    case HAPPY:     eyes.tw = 38; eyes.th = 32; break;
    case SLEEPY:    eyes.tw = 36; eyes.th = 30; break;
    case ANGRY:     eyes.tw = 36; eyes.th = 32; break;
    case SAD:       eyes.tw = 34; eyes.th = 32; break;
    case SURPRISED: eyes.tw = 42; eyes.th = 44; break;
    case LOVE:
    case WINK:
    case DIZZY:     eyes.tw = 36; eyes.th = 36; break;
  }
}

static inline float lerpf(float a, float b, float k) { return a + (b - a) * k; }

void updateEyes(uint32_t now) {
  // Eyes lead the head: gaze follows servo *targets*, not current position
  float px = (pan.tgt - PAN_CENTER) / 90.0f * 14.0f;
  eyes.tox = PAN_INVERT ? -px : px;
  eyes.toy = (TILT_CENTER - tilt.tgt) / 5.0f * 6.0f * (TILT_UP > TILT_CENTER ? 1 : -1);

  // Auto blink
  if (!blinking && (int32_t)(now - nextBlink) >= 0) {
    blinking = true;
    blinkEnd = now + 90;
  }
  if (blinking && (int32_t)(now - blinkEnd) >= 0) {
    blinking = false;
    bool dbl = random(100) < 20;
    nextBlink = now + (dbl ? 180 : (eyes.mood == SLEEPY ? random(1200, 2500) : random(2000, 5500)));
  }

  float targetH = blinking ? 2 : eyes.th;
  eyes.h  = lerpf(eyes.h,  targetH, blinking ? 0.6f : 0.35f);
  eyes.w  = lerpf(eyes.w,  eyes.tw, 0.3f);
  eyes.ox = lerpf(eyes.ox, eyes.tox, 0.4f);
  eyes.oy = lerpf(eyes.oy, eyes.toy, 0.4f);
}

void drawHeart(int cx, int cy, int s) {
  int r = s / 4;
  oled.fillCircle(cx - r, cy - r / 2, r, SSD1306_WHITE);
  oled.fillCircle(cx + r, cy - r / 2, r, SSD1306_WHITE);
  oled.fillTriangle(cx - 2 * r, cy - r / 4, cx + 2 * r, cy - r / 4, cx, cy + s / 2, SSD1306_WHITE);
}

void drawX(int cx, int cy, int s) {
  int h = s / 2;
  for (int o = -2; o <= 2; o++) {
    oled.drawLine(cx - h + o, cy - h, cx + h + o, cy + h, SSD1306_WHITE);
    oled.drawLine(cx + h + o, cy - h, cx - h + o, cy + h, SSD1306_WHITE);
  }
}

void drawEye(int cx, bool left) {
  int ecx = cx + (int)eyes.ox;
  int ecy = SCR_H / 2 + (int)eyes.oy;
  bool midBlink = eyes.h < 6;

  // Special eye shapes
  if (eyes.mood == LOVE && !midBlink) {
    int s = 32 + (int)(4 * sinf(millis() / 120.0f));   // pulsing heart
    drawHeart(ecx, ecy, s);
    return;
  }
  if (eyes.mood == DIZZY) {
    drawX(ecx, ecy, 24);
    return;
  }
  if (eyes.mood == WINK && !left) {
    oled.fillRoundRect(ecx - 18, ecy - 2, 36, 5, 2, SSD1306_WHITE);
    return;
  }

  // Standard rounded eye
  int w = (int)eyes.w;
  int h = max(2, (int)eyes.h);
  int x = ecx - w / 2;
  int y = ecy - h / 2;
  int r = min(10, h / 2);

  oled.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);
  if (midBlink) return; // skip overlays mid-blink

  switch (eyes.mood) {
    case HAPPY:   // cheeks push up -> curved bottom cut
      oled.fillRoundRect(x - 2, y + (int)(h * 0.6f), w + 4, h, r, SSD1306_BLACK);
      break;
    case SLEEPY:  // heavy upper lid
      oled.fillRect(x - 1, y - 1, w + 2, h / 2 + 1, SSD1306_BLACK);
      break;
    case ANGRY:   // brows slant down toward the nose
      if (left) oled.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + h / 2, SSD1306_BLACK);
      else      oled.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1,     y + h / 2, SSD1306_BLACK);
      break;
    case SAD:     // brows slant down to the outside
      if (left) oled.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x - 1,     y + h / 2, SSD1306_BLACK);
      else      oled.fillTriangle(x - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + h / 2, SSD1306_BLACK);
      break;
    default: break;
  }
}

void render() {
  oled.clearDisplay();
  drawEye(EYE_L_CX, true);
  drawEye(EYE_R_CX, false);
  oled.display();
}

void showText(const char* l1, const char* l2) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 16);
  oled.println(l1);
  oled.setCursor(0, 32);
  oled.println(l2);
  oled.display();
}

// ---------------- Actions (web + auto) ----------------
bool autoMode = true;
uint32_t nextAction = 0;
bool wasBusy = false;

bool runAction(const String& a) {
  int p = (int)pan.cur;
  clearQueue();

  if (a == "smile") {
    setMood(HAPPY);
    push(p, TILT_UP,   2, 0.5f, 60);
    push(p, TILT_DOWN, 2, 0.5f, 60);
    push(p, TILT_CENTER, 2, 0.4f, 2500);
  } else if (a == "dance") {
    setMood(HAPPY);
    for (int i = 0; i < 4; i++) {
      push(55,  TILT_UP,   4.0f, 1.0f, 60);
      push(125, TILT_DOWN, 4.0f, 1.0f, 60);
    }
    push(PAN_CENTER, TILT_CENTER, 3.0f, 0.5f, 800);
  } else if (a == "headdance") {        // base still, head bobs to a beat
    setMood(HAPPY);
    for (int i = 0; i < 7; i++) {
      push(p, TILT_UP,   2.0f, 1.0f, 40);
      push(p, TILT_DOWN, 2.0f, 1.0f, 40);
    }
    push(p, TILT_CENTER, 2.0f, 0.5f, 600);
  } else if (a == "angry") {
    setMood(ANGRY);
    for (int i = 0; i < 3; i++) {
      push(p - 15, TILT_DOWN, 3.5f, 0.5f, 0);
      push(p + 15, TILT_DOWN, 3.5f, 0.5f, 0);
    }
    push(p, TILT_DOWN, 2, 0.4f, 1500);
  } else if (a == "sad") {
    setMood(SAD);
    push(p, TILT_DOWN, 1, 0.15f, 3000);
  } else if (a == "surprised") {
    setMood(SURPRISED);
    push(p, TILT_UP, 2, 1.5f, 1500);
    push(p, TILT_CENTER, 2, 0.3f, 300);
  } else if (a == "sleepy") {
    setMood(SLEEPY);
    push(PAN_CENTER, TILT_DOWN, 0.8f, 0.1f, 4000);
  } else if (a == "love") {
    setMood(LOVE);
    for (int i = 0; i < 2; i++) {
      push(75,  TILT_UP, 1.0f, 0.3f, 200);
      push(105, TILT_UP, 1.0f, 0.3f, 200);
    }
    push(PAN_CENTER, TILT_CENTER, 1.0f, 0.3f, 1500);
  } else if (a == "wink") {
    setMood(WINK);
    push(p, TILT_UP, 1, 0.5f, 1200);
    push(p, TILT_CENTER, 1, 0.3f, 200);
  } else if (a == "dizzy") {
    setMood(DIZZY);
    for (int i = 0; i < 3; i++) {
      push(70,  TILT_UP,   3.0f, 0.8f, 0);
      push(110, TILT_DOWN, 3.0f, 0.8f, 0);
    }
    push(PAN_CENTER, TILT_CENTER, 1.5f, 0.4f, 1200);
  } else if (a == "curious") {
    setMood(NORMAL);
    push(35,  TILT_CENTER, 1.0f, 0.3f, 500);
    push(145, TILT_CENTER, 1.0f, 0.3f, 500);
    push(PAN_CENTER, TILT_CENTER, 1.2f, 0.3f, 600);
  } else if (a == "yes") {
    setMood(NORMAL);
    for (int i = 0; i < 3; i++) {
      push(p, TILT_DOWN, 2, 0.7f, 40);
      push(p, TILT_UP,   2, 0.7f, 40);
    }
    push(p, TILT_CENTER, 2, 0.5f, 800);
  } else if (a == "no") {
    setMood(NORMAL);
    for (int i = 0; i < 3; i++) {
      push(p - 20, TILT_CENTER, 3.0f, 0.5f, 0);
      push(p + 20, TILT_CENTER, 3.0f, 0.5f, 0);
    }
    push(p, TILT_CENTER, 2, 0.4f, 800);
  } else if (a == "left") {
    setMood(NORMAL);
    push(PAN_INVERT ? 30 : 150, TILT_CENTER, 2.0f, 0.4f, 2000);
  } else if (a == "right") {
    setMood(NORMAL);
    push(PAN_INVERT ? 150 : 30, TILT_CENTER, 2.0f, 0.4f, 2000);
  } else if (a == "center") {
    setMood(NORMAL);
    push(PAN_CENTER, TILT_CENTER, 2.0f, 0.4f, 1500);
  } else {
    return false;
  }
  return true;
}

void pickAction() {   // random idle behaviour (auto mode)
  int a = random(100);
  if      (a < 35) { setMood(NORMAL);
                     push(random(30, 151), random(TILT_DOWN, TILT_UP + 1), 1.8f, 0.4f, random(800, 2000)); }
  else if (a < 48) runAction("smile");
  else if (a < 58) runAction("curious");
  else if (a < 66) runAction("surprised");
  else if (a < 73) runAction("angry");
  else if (a < 80) runAction("sad");
  else if (a < 85) runAction("wink");
  else if (a < 89) runAction("love");
  else             runAction("sleepy");
}

void behaviour(uint32_t now) {
  bool busy = qLen > 0;
  if (wasBusy && !busy) {             // action finished
    setMood(NORMAL);
    if (lroundf(pan.tgt) != PAN_HOME)  // return base servo to home
      push(PAN_HOME, (int)tilt.tgt, 2.0f, 0.4f, 0);
    nextAction = now + random(1200, 3500);
  }
  wasBusy = busy;
  if (autoMode && !busy && (int32_t)(now - nextAction) >= 0) pickAction();
}

// ---------------- Web UI ----------------
WebServer server(80);

const char PAGE[] = R"rawliteral(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8">
<meta name="google" content="notranslate">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk Buddy</title>
<style>
 body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px;text-align:center}
 h2{margin:8px 0 16px}
 .g{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;max-width:420px;margin:auto}
 button{padding:16px 4px;font-size:15px;border:0;border-radius:12px;background:#2a2a2a;color:#eee}
 button:active{background:#4a7cff}
 .row{max-width:420px;margin:18px auto}
 input[type=range]{width:100%}
 #st{margin-top:14px;color:#8f8;min-height:1.2em}
</style></head><body>
<h2>Desk Buddy</h2>
<div class="g">
 <button onclick="act('smile')">😊 Smile</button>
 <button onclick="act('dance')">💃 Dance</button>
 <button onclick="act('headdance')">🎵 Head Dance</button>
 <button onclick="act('angry')">😠 Angry</button>
 <button onclick="act('sad')">😢 Sad</button>
 <button onclick="act('surprised')">😮 Surprised</button>
 <button onclick="act('sleepy')">😴 Sleepy</button>
 <button onclick="act('love')">😍 Love</button>
 <button onclick="act('wink')">😉 Wink</button>
 <button onclick="act('dizzy')">😵 Dizzy</button>
 <button onclick="act('curious')">🤔 Curious</button>
 <button onclick="act('yes')">👍 Yes</button>
 <button onclick="act('no')">👎 No</button>
 <button onclick="act('left')">⬅ Left</button>
 <button onclick="act('center')">⏺ Center</button>
 <button onclick="act('right')">Right ➡</button>
</div>
<div class="row">Look<br><input type="range" min="0" max="180" value="90" onchange="look(this.value)"></div>
<div class="row"><label><input type="checkbox" id="ab" onchange="au(this.checked)"> Auto mode (random idle moves)</label></div>
<div id="st"></div>
<script>
 const st=document.getElementById('st'), ab=document.getElementById('ab');
 function show(p){p.then(r=>r.text()).then(t=>st.textContent=t).catch(()=>st.textContent='offline');}
 function act(n){show(fetch('/act?name='+n));}
 function look(v){show(fetch('/look?pan='+v));}
 function au(c){show(fetch('/auto?on='+(c?1:0)));}
 fetch('/auto').then(r=>r.text()).then(t=>ab.checked=t.endsWith('on'));
</script>
</body></html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", PAGE);
}

void handleAct() {
  String name = server.arg("name");
  if (runAction(name)) server.send(200, "text/plain", "ok: " + name);
  else                 server.send(400, "text/plain", "unknown action: " + name);
}

void handleLook() {
  int p = constrain(server.arg("pan").toInt(), PAN_MIN, PAN_MAX);
  if (PAN_INVERT) p = PAN_MAX - p;
  clearQueue();
  setMood(NORMAL);
  push(p, TILT_CENTER, 2.5f, 0.5f, 3000);
  server.send(200, "text/plain", "look: " + String(p));
}

void handleAuto() {
  if (server.hasArg("on")) autoMode = server.arg("on") == "1";
  server.send(200, "text/plain", autoMode ? "auto: on" : "auto: off");
}

bool mdnsUp = false;

void startMDNS() {
  if (mdnsUp) MDNS.end();
  mdnsUp = MDNS.begin("deskbuddy");
  if (mdnsUp) MDNS.addService("http", "tcp", 80);
}

// Called every loop: retries Wi-Fi after a drop without blocking animation
void maintainWiFi(uint32_t now) {
  static bool wasConnected = false;
  static uint32_t lastTry = 0;
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wasConnected) {          // just (re)connected
    WiFi.setSleep(false);
    startMDNS();
    Serial.printf("Desk Buddy: open http://%s/\n", WiFi.localIP().toString().c_str());
  }
  if (!connected && now - lastTry >= WIFI_RETRY_MS) {
    lastTry = now;
    Serial.println("WiFi lost - reconnecting");
    WiFi.reconnect();
  }
  wasConnected = connected;
}

void connectWiFi() {
  showText("Connecting WiFi...", WIFI_SSID_SECRET);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID_SECRET, WIFI_PASS_SECRET);
#if WIFI_LOW_TX
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(250);

  // Server is started regardless, so it works as soon as Wi-Fi (re)connects
  server.on("/", handleRoot);
  server.on("/act", handleAct);
  server.on("/look", handleLook);
  server.on("/auto", handleAuto);
  server.begin();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);            // faster web response
    String ip = WiFi.localIP().toString();
    // mDNS is started by maintainWiFi() on the first loop

    showText("Open in browser:", ip.c_str());
    delay(4000);
  } else {
    Serial.println("WiFi failed - running offline");
    showText("WiFi failed", "Running offline");
    delay(2000);
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  randomSeed(esp_random());

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
    while (true) delay(1000);
  }

  // Attach servos first so their signal lines don't float (jitter)
  // during the Wi-Fi wait
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  pan.begin(PIN_PAN, PAN_MIN, PAN_MAX, PAN_CENTER);
  tilt.begin(PIN_TILT, TILT_DOWN, TILT_UP, TILT_CENTER);

  connectWiFi();

  // Wake-up: start sleepy, open eyes, little happy nod
  setMood(SLEEPY);
  eyes.h = 2;
  push(PAN_CENTER, TILT_DOWN, 1, 0.2f, 1200);
  push(PAN_CENTER, TILT_UP,   1, 0.5f, 200);
  push(PAN_CENTER, TILT_CENTER, 1, 0.5f, 300);
  nextBlink = millis() + 1500;
}

void loop() {
  uint32_t now = millis();
  maintainWiFi(now);
  if (WiFi.status() == WL_CONNECTED) server.handleClient();

  static uint32_t lastFrame = 0;
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  behaviour(now);
  runQueue(now);
  pan.update();
  tilt.update();
  updateEyes(now);
  render();
}
