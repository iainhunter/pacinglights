// Runner pacing light controller
// ESP32 + chained WS2811/FCOB strips on ONE data line
//
// Features:
// - WiFi access point at 192.168.4.1
// - Modern browser-based control page
// - Dropdown to select connected strip length: 1, 2, or 3 segments
// - One continuous virtual strip across the selected length
// - Moving dot pattern with soft faded tail
// - Speed shown as m/s, min/km, min/mi, and 400 m split
// - Reverse direction option
// - Startup sparkle mode with bright independent twinkles
//
// Notes:
// - Wire strips end-to-end on the same data line.
// - This sketch is written for a maximum chain of 3 x 7.5 m strips.
// - If colours appear swapped, change LED_COLOR_ORDER below.

#include <Arduino.h>
#include <FastLED.h>
#include <WebServer.h>
#include <WiFi.h>
#include <math.h>

// -----------------------------
// WiFi access point settings
// -----------------------------
static const char *AP_SSID = "PacingLights";
static const char *AP_PASSWORD = "runfast123"; // change this before deploying
WebServer server(80);

// -----------------------------
// LED strip configuration
// -----------------------------
static constexpr uint16_t LEDS_PER_METER = 24;    // from the strip listing
static constexpr uint16_t LEDS_PER_SEGMENT = 180; // 7.5 m * 24 sections/m
static constexpr uint16_t MAX_SEGMENTS = 3;
static constexpr uint16_t TOTAL_LEDS =
    LEDS_PER_SEGMENT * MAX_SEGMENTS;    // 540 max
static constexpr uint8_t DATA_PIN = 19; // single data line

// You said this colour order matches your strip.
// If colours are still odd, swap this between RGB / RBG / GRB.
#define LED_COLOR_ORDER RGB

CRGB leds[TOTAL_LEDS];

// -----------------------------
// Runtime settings
// -----------------------------
static bool animationRunning = true;
static bool reverseDirection = false;
static float paceSpeedMps = 6.70f;                 // initial speed
static uint8_t brightness = 80;                    // 16 to 255
static CRGB baseColor = CRGB::Green;               // initial colour
static uint16_t activeLeds = LEDS_PER_SEGMENT * 3; // default 3 segments

// Motion / dot shape
static constexpr float MARKER_SPACING_M = 4.0f;
static constexpr uint16_t MARKER_SPACING_LEDS =
    (uint16_t)(LEDS_PER_METER * MARKER_SPACING_M + 0.5f);
static constexpr uint8_t DOT_LENGTH_LEDS = 6;
static constexpr uint8_t FADE_LEDS = 6;
static constexpr uint16_t FRAME_MS = 20;

uint32_t lastFrameMs = 0;
float phaseLeds = 0.0f;

// Startup sparkle animation
static constexpr uint32_t BOOT_ANIMATION_MS = 5000;
uint32_t bootStartMs = 0;
uint32_t bootNextToggleMs[TOTAL_LEDS];
bool bootOn[TOTAL_LEDS];
uint8_t bootOnCount = 0;

// -----------------------------
// Web UI
// -----------------------------
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Pacing Lights</title>
  <style>
    :root {
      color-scheme: dark;
      --bg1: #07111f;
      --card: rgba(255,255,255,0.07);
      --card-border: rgba(255,255,255,0.12);
      --text: #f5f7ff;
      --muted: #b8c0d9;
      --accent: #7c9cff;
      --accent2: #4de1b1;
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100vh;
      font-family: Inter, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      font-size: 18px;
      line-height: 1.45;
      color: var(--text);
      background: radial-gradient(circle at top, #18284f 0%, var(--bg1) 40%, #040814 100%);
    }

    .wrap {
      max-width: 920px;
      margin: 0 auto;
      padding: 20px;
    }

    .hero {
      padding: 4px 4px 0px;
      margin-bottom: 8px;
    }

    h1 {
      margin: 0 0 2px;
      font-size: 2rem;
      letter-spacing: -0.03em;
    }

    .subtitle {
      color: var(--muted);
      font-size: 1rem;
    }

    .grid {
      display: grid;
      grid-template-columns: repeat(12, 1fr);
      gap: 16px;
    }

    .card {
      grid-column: span 12;
      background: var(--card);
      border: 1px solid var(--card-border);
      border-radius: 20px;
      backdrop-filter: blur(10px);
      box-shadow: 0 16px 40px rgba(0,0,0,0.25);
      padding: 18px;
    }

    @media (min-width: 760px) {
      .card.half { grid-column: span 6; }
    }

    .controls {
      display: grid;
      gap: 14px;
    }

    label {
      display: block;
      margin-bottom: 6px;
      font-weight: 700;
    }

    input[type=range], select {
      width: 100%;
      accent-color: var(--accent);
      background: rgba(255,255,255,0.06);
      color: var(--text);
      border: 1px solid rgba(255,255,255,0.14);
      border-radius: 14px;
      padding: 12px;
      font-size: 1rem;
    }

    .readout {
      font-size: 1.1rem;
      color: var(--muted);
      margin-top: 6px;
    }

    .stat-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
      margin-top: 10px;
    }

    .stat {
      padding: 14px;
      border-radius: 16px;
      background: rgba(255,255,255,0.05);
      border: 1px solid rgba(255,255,255,0.08);
    }

    .stat .k {
      display: block;
      color: var(--muted);
      font-size: 0.95rem;
      margin-bottom: 4px;
    }

    .stat .v {
      font-size: 1.25rem;
      font-weight: 800;
      letter-spacing: -0.02em;
    }

    .buttons {
      display: flex;
      gap: 12px;
      flex-wrap: wrap;
      margin-top: 6px;
    }

    button {
      appearance: none;
      border: none;
      border-radius: 14px;
      padding: 12px 18px;
      font-size: 1rem;
      font-weight: 800;
      cursor: pointer;
      color: #08111f;
      background: linear-gradient(135deg, #7c9cff, #4de1b1);
      box-shadow: 0 10px 22px rgba(77,225,177,0.18);
    }

    button.secondary {
      background: rgba(255,255,255,0.12);
      color: var(--text);
      box-shadow: none;
      border: 1px solid rgba(255,255,255,0.10);
    }

    .checkline {
      display: flex;
      align-items: center;
      gap: 10px;
      font-weight: 700;
      margin-top: 4px;
    }

    input[type=checkbox] {
      width: 20px;
      height: 20px;
      accent-color: var(--accent2);
    }

    .swatches {
      display: grid;
      grid-template-columns: repeat(7, minmax(0, 1fr));
      gap: 10px;
    }

    .swatch {
      width: 100%;
      aspect-ratio: 1;
      border-radius: 14px;
      border: 2px solid rgba(255,255,255,0.18);
      box-shadow: inset 0 0 0 1px rgba(0,0,0,0.15);
      cursor: pointer;
      padding: 0;
    }

    .swatch.active {
      outline: 3px solid rgba(255,255,255,0.95);
      outline-offset: 2px;
    }

    .hint {
      color: var(--muted);
      font-size: 0.95rem;
      margin-top: 8px;
    }

    .speed-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 6px;
    }

    .speed-num-input {
      width: 75px;
      background: rgba(255,255,255,0.06);
      border: 1px solid rgba(255,255,255,0.14);
      border-radius: 8px;
      color: var(--text);
      padding: 4px 6px;
      font-size: 0.95rem;
      text-align: right;
      font-family: inherit;
    }

    .speed-slider-row {
      display: flex;
      align-items: center;
      gap: 10px;
    }

    button.adjust-btn {
      appearance: none;
      background: rgba(255,255,255,0.08);
      color: var(--text);
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 10px;
      width: 38px;
      height: 38px;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
      font-size: 1.2rem;
      font-weight: 700;
      user-select: none;
      flex-shrink: 0;
      box-shadow: none;
      padding: 0;
    }

    button.adjust-btn:hover {
      background: rgba(255,255,255,0.14);
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <h1>Pacing Lights</h1>
    </div>

    <div class="grid">
      <div class="card">
        <div class="stat-grid">
          <div class="stat">
            <span class="k">Speed</span>
            <span class="v" id="speedVal">—</span>
          </div>
          <div class="stat">
            <span class="k">400 m split</span>
            <span class="v" id="splitVal">—</span>
          </div>
          <div class="stat">
            <span class="k">Pace / km</span>
            <span class="v" id="paceKmVal">—</span>
          </div>
          <div class="stat">
            <span class="k">Pace / mile</span>
            <span class="v" id="paceMiVal">—</span>
          </div>
        </div>
      </div>

      <div class="card half">
        <div class="controls">
          <div>
            <label for="speed">Speed (m/s)</label>
            <div class="speed-slider-row">
              <button type="button" class="adjust-btn" id="decSpeed" aria-label="Decrease speed">-</button>
              <input id="speed" type="range" min="0.1" max="10.2" step="0.01" value="6.70" />
              <button type="button" class="adjust-btn" id="incSpeed" aria-label="Increase speed">+</button>
            </div>
          </div>

          <div>
            <label for="brightness">Brightness</label>
            <input id="brightness" type="range" min="16" max="255" step="1" value="80" />
          </div>

          <div>
            <label>Dot colour</label>
            <div class="swatches" id="swatches">
              <button class="swatch active" data-color="#00ff00" style="background:#00ff00" aria-label="Green"></button>
              <button class="swatch" data-color="#ff0000" style="background:#ff0000" aria-label="Red"></button>
              <button class="swatch" data-color="#0000ff" style="background:#0000ff" aria-label="Blue"></button>
              <button class="swatch" data-color="#00ffff" style="background:#00ffff" aria-label="Cyan"></button>
              <button class="swatch" data-color="#ff00ff" style="background:#ff00ff" aria-label="Magenta"></button>
              <button class="swatch" data-color="#ffff00" style="background:#ffff00" aria-label="Yellow"></button>
              <button class="swatch" data-color="#ffffff" style="background:#ffffff" aria-label="White"></button>
            </div>
          </div>
        </div>
      </div>

      <div class="card half">
        <div class="controls">
          <div class="checkline">
            <input id="reverse" type="checkbox" />
            <label for="reverse" style="margin:0;">Reverse direction</label>
          </div>

          <div class="buttons">
            <button onclick="sendState(true)">Start</button>
            <button class="secondary" onclick="sendState(false)">Stop</button>
          </div>

          <div>
            <label for="stripCount">Connected strips</label>
            <select id="stripCount">
              <option value="180">1 strip (7.5 m)</option>
              <option value="360">2 strips (15.0 m)</option>
              <option value="540" selected>3 strips (22.5 m)</option>
            </select>
          </div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const speedEl = document.getElementById('speed');
    const brightnessEl = document.getElementById('brightness');
    const stripCountEl = document.getElementById('stripCount');
    const swatchesEl = document.getElementById('swatches');
    const reverseEl = document.getElementById('reverse');
    const speedValEl = document.getElementById('speedVal');
    const splitValEl = document.getElementById('splitVal');
    const paceKmValEl = document.getElementById('paceKmVal');
    const paceMiValEl = document.getElementById('paceMiVal');
    let selectedColor = '#00ff00';

    function pad2(n) {
      return String(n).padStart(2, '0');
    }

    function formatMmSs(seconds) {
      if (!isFinite(seconds) || seconds <= 0) return '—';
      const total = Math.round(seconds);
      const m = Math.floor(total / 60);
      const s = total % 60;
      return `${m}:${pad2(s)}`;
    }

    function updateDerived() {
      const speed = parseFloat(speedEl.value);
      
      speedValEl.textContent = speed.toFixed(2) + ' m/s';
      splitValEl.textContent = formatMmSs(400 / speed);
      paceKmValEl.textContent = formatMmSs(1000 / speed) + ' /km';
      paceMiValEl.textContent = formatMmSs(1609.344 / speed) + ' /mi';
    }

    function adjustSpeed(amount) {
      let val = parseFloat(speedEl.value);
      val = Math.max(0.01, Math.min(10.2, val + amount));
      speedEl.value = val.toFixed(2);
      updateDerived();
      sendState(true);
    }

    function updateSwatchState() {
      document.querySelectorAll('.swatch').forEach(btn => {
        btn.classList.toggle('active', btn.dataset.color === selectedColor);
      });
    }

    async function sendState(running) {
      const payload = {
        running: running,
        reverse: reverseEl.checked,
        speed: parseFloat(speedEl.value),
        brightness: parseInt(brightnessEl.value, 10),
        color: selectedColor,
        stripCount: parseInt(stripCountEl.value, 10)
      };

      const res = await fetch('/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      const data = await res.json();
      reverseEl.checked = !!data.reverse;
      if (data.color) {
        selectedColor = data.color;
        updateSwatchState();
      }
      if (data.stripCount) {
        stripCountEl.value = String(data.stripCount);
      }
      updateDerived();
    }

    async function refreshStatus() {
      const res = await fetch('/state');
      const data = await res.json();
      speedEl.value = data.speed;
      brightnessEl.value = data.brightness;
      reverseEl.checked = !!data.reverse;
      if (data.color) {
        selectedColor = data.color;
        updateSwatchState();
      }
      if (data.stripCount) {
        stripCountEl.value = String(data.stripCount);
      }
      updateDerived();
    }

    swatchesEl.addEventListener('click', (e) => {
      const btn = e.target.closest('.swatch');
      if (!btn) return;
      selectedColor = btn.dataset.color;
      updateSwatchState();
      sendState(true);
    });

    speedEl.addEventListener('input', updateDerived);
    speedEl.addEventListener('change', () => sendState(true));
    document.getElementById('decSpeed').addEventListener('click', () => adjustSpeed(-0.01));
    document.getElementById('incSpeed').addEventListener('click', () => adjustSpeed(0.01));
    brightnessEl.addEventListener('change', () => sendState(true));
    stripCountEl.addEventListener('change', () => sendState(true));
    reverseEl.addEventListener('change', () => sendState(true));

    updateSwatchState();
    updateDerived();
    refreshStatus();
  </script>
</body>
</html>
)HTML";

// -----------------------------
// Helpers
// -----------------------------
static bool extractJsonBool(const String &body, const char *key, bool &out) {
  String patternTrue = String("\"") + key + "\":true";
  String patternFalse = String("\"") + key + "\":false";
  if (body.indexOf(patternTrue) >= 0) {
    out = true;
    return true;
  }
  if (body.indexOf(patternFalse) >= 0) {
    out = false;
    return true;
  }
  return false;
}

static bool extractJsonFloat(const String &body, const char *key, float &out) {
  String pattern = String("\"") + key + "\":";
  int pos = body.indexOf(pattern);
  if (pos < 0)
    return false;
  int start = pos + pattern.length();
  out = body.substring(start).toFloat();
  return true;
}

static bool extractJsonUInt16(const String &body, const char *key,
                              uint16_t &out) {
  String pattern = String("\"") + key + "\":";
  int pos = body.indexOf(pattern);
  if (pos < 0)
    return false;
  int start = pos + pattern.length();
  out = (uint16_t)body.substring(start).toInt();
  return true;
}

static bool extractJsonString(const String &body, const char *key,
                              String &out) {
  String pattern = String("\"") + key + "\":\"";
  int pos = body.indexOf(pattern);
  if (pos < 0)
    return false;
  int start = pos + pattern.length();
  int end = body.indexOf('"', start);
  if (end <= start)
    return false;
  out = body.substring(start, end);
  return true;
}

static CRGB parseHexColor(String hex) {
  hex.trim();
  if (hex.startsWith("#")) {
    hex.remove(0, 1);
  }
  if (hex.length() != 6) {
    return CRGB::Green;
  }

  char buf[7];
  hex.toCharArray(buf, sizeof(buf));
  long value = strtol(buf, nullptr, 16);
  uint8_t r = (value >> 16) & 0xFF;
  uint8_t g = (value >> 8) & 0xFF;
  uint8_t b = value & 0xFF;
  return CRGB(r, g, b);
}

static uint16_t activeLedsClamped() {
  if (activeLeds > TOTAL_LEDS)
    return TOTAL_LEDS;
  return activeLeds;
}

static void clearAllLeds() { fill_solid(leds, TOTAL_LEDS, CRGB::Black); }

static void renderLeds(float phaseOffsetLeds) {
  uint16_t maxLed = activeLedsClamped();

  for (uint16_t i = 0; i < maxLed; ++i) {
    leds[i] = CRGB::Black;

    float x = (float)i + phaseOffsetLeds;
    float local = fmodf(x, (float)MARKER_SPACING_LEDS);
    if (local < 0.0f) {
      local += MARKER_SPACING_LEDS;
    }

    // Measure from the leading edge of motion.
    // When not reversed, the leading edge is at the high end of the spacing
    // block. When reversed, the leading edge is at the low end of the spacing
    // block.
    float headDist =
        reverseDirection ? local : ((float)MARKER_SPACING_LEDS - local);

    if (headDist >= (float)MARKER_SPACING_LEDS) {
      headDist = 0.0f;
    }

    float dotStart = 0.0f;
    float dotEnd = (float)DOT_LENGTH_LEDS;
    float fadeEnd = dotEnd + (float)FADE_LEDS;

    if (headDist >= dotStart && headDist <= fadeEnd) {
      float intensity = 0.0f;

      if (headDist <= dotEnd) {
        // Brightest at the front, fading toward the rear.
        float t = headDist / max(1.0f, dotEnd);
        intensity = powf(1.0f - t, 0.35f);
      } else {
        // Extra fade behind the main lit section.
        float t = (headDist - dotEnd) / max(1.0f, (float)FADE_LEDS);
        intensity = powf(max(0.0f, 1.0f - t), 2.2f) * 0.35f;
      }

      if (intensity > 0.0f) {
        CRGB c = baseColor;
        c.nscale8_video((uint8_t)(255.0f * intensity));
        leds[i] = c;
      }
    }
  }

  for (uint16_t i = maxLed; i < TOTAL_LEDS; ++i) {
    leds[i] = CRGB::Black;
  }
}

static void applyBrightness() { FastLED.setBrightness(brightness); }

// Startup sparkles: independent twinkles with a brightness cap.
static void initBootAnimation() {
  for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
    bootOn[i] = false;
    bootNextToggleMs[i] = millis() + (uint32_t)random(50, 700);
  }
  bootOnCount = 0;
  clearAllLeds();
  FastLED.show();
}

static void updateBootAnimation() {
  uint32_t now = millis();

  if (now - bootStartMs >= BOOT_ANIMATION_MS) {
    clearAllLeds();
    FastLED.show();
    return;
  }

  // We want at most about 5% of LEDs on at a time.
  const uint16_t maxOn = max<uint16_t>(1, TOTAL_LEDS / 20);

  for (uint16_t i = 0; i < TOTAL_LEDS; ++i) {
    if (now < bootNextToggleMs[i])
      continue;

    if (bootOn[i]) {
      bootOn[i] = false;
      if (bootOnCount > 0)
        bootOnCount--;
      leds[i] = CRGB::Black;
      bootNextToggleMs[i] = now + (uint32_t)random(200, 700);
    } else {
      if (bootOnCount < maxOn) {
        bootOn[i] = true;
        bootOnCount++;

        // Bright coloured sparkle.
        CHSV hsv((uint8_t)random(0, 255), 255, 255);
        CRGB c;
        hsv2rgb_rainbow(hsv, c);
        leds[i] = c;

        // Short visible life so they twinkle independently.
        bootNextToggleMs[i] = now + (uint32_t)random(200, 700);
      } else {
        // Try again a little later so we never exceed the cap.
        bootNextToggleMs[i] = now + (uint32_t)random(80, 180);
      }
    }
  }

  FastLED.show();
}

static void updateAnimation() {
  if (millis() - bootStartMs < BOOT_ANIMATION_MS) {
    updateBootAnimation();
    return;
  }

  if (!animationRunning) {
    clearAllLeds();
    FastLED.show();
    delay(10);
    return;
  }

  uint32_t now = millis();
  uint32_t elapsed = now - lastFrameMs;
  if (elapsed < FRAME_MS) {
    return;
  }
  lastFrameMs = now;

  float dt = elapsed / 1000.0f;
  float ledsPerSecond = paceSpeedMps * (float)LEDS_PER_METER;
  float delta = ledsPerSecond * dt;
  phaseLeds += reverseDirection ? delta : -delta;

  if (phaseLeds > 100000.0f || phaseLeds < -100000.0f) {
    phaseLeds = fmodf(phaseLeds, (float)MARKER_SPACING_LEDS);
  }

  renderLeds(phaseLeds);
  FastLED.show();
}

static String jsonState() {
  String out = "{";
  out += "\"running\":" + String(animationRunning ? "true" : "false") + ",";
  out += "\"reverse\":" + String(reverseDirection ? "true" : "false") + ",";
  out += "\"speed\":" + String(paceSpeedMps, 2) + ",";
  out += "\"brightness\":" + String(brightness) + ",";
  out += "\"stripCount\":" + String(activeLeds) + ",";

  char hex[7];
  snprintf(hex, sizeof(hex), "%02x%02x%02x", baseColor.r, baseColor.g,
           baseColor.b);
  out += "\"color\":\"#";
  out += hex;
  out += "\"}";
  return out;
}

// -----------------------------
// HTTP handlers
// -----------------------------
static void handleRoot() { server.send(200, "text/html", INDEX_HTML); }

static void handleState() { server.send(200, "application/json", jsonState()); }

static void handleSet() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"Missing JSON body\"}");
    return;
  }

  String body = server.arg("plain");

  bool run = animationRunning;
  if (extractJsonBool(body, "running", run)) {
    animationRunning = run;
  }

  bool rev = reverseDirection;
  if (extractJsonBool(body, "reverse", rev)) {
    reverseDirection = rev;
  }

  float speed = paceSpeedMps;
  if (extractJsonFloat(body, "speed", speed)) {
    paceSpeedMps = constrain(speed, 0.01f, 10.2f);
  }

  float bright = brightness;
  if (extractJsonFloat(body, "brightness", bright)) {
    int val = (int)bright;
    val = constrain(val, 16, 255);
    brightness = (uint8_t)val;
    applyBrightness();
  }

  uint16_t stripCount = activeLeds;
  if (extractJsonUInt16(body, "stripCount", stripCount)) {
    if (stripCount < LEDS_PER_SEGMENT)
      stripCount = LEDS_PER_SEGMENT;
    if (stripCount > TOTAL_LEDS)
      stripCount = TOTAL_LEDS;
    activeLeds = stripCount;
  }

  String colorHex;
  if (extractJsonString(body, "color", colorHex)) {
    baseColor = parseHexColor(colorHex);
  }

  server.send(200, "application/json", jsonState());
}

// -----------------------------
// Setup / loop
// -----------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  randomSeed((uint32_t)(micros() ^ (millis() << 16)));

  FastLED.addLeds<WS2811, DATA_PIN, LED_COLOR_ORDER>(leds, TOTAL_LEDS);
  FastLED.setDither(true);
  applyBrightness();
  clearAllLeds();
  FastLED.show();

  WiFi.mode(WIFI_AP);
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();

  bootStartMs = millis();
  initBootAnimation();
  lastFrameMs = millis();
}

void loop() {
  server.handleClient();
  updateAnimation();
}
