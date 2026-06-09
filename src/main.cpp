// Runner pacing light controller
// ESP32 + 3x WS2811/FCOB strips
//
// Features:
// - WiFi access point at 192.168.4.1
// - Modern browser-based control page
// - Three independent LED strip outputs
// - Moving dot pattern with wide spacing and soft fades
// - m/s, min/km, min/mi, and 400 m split readouts
// - Reverse direction option
// - Startup sparkle mode with random flashing colours
//
// Notes:
// - Board pins used here match your Sparkle Motion labels: 19, 22, 21.
// - If colours appear swapped, change LED_COLOR_ORDER between GRB and RGB.

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
static constexpr uint8_t NUM_STRIPS = 3;
static constexpr uint16_t LEDS_PER_METER = 24; // from the strip listing
static constexpr float MARKER_SPACING_M =
    3.0f; // quadrupled spacing between moving lights
static constexpr uint16_t MARKER_SPACING_LEDS =
    (uint16_t)(LEDS_PER_METER * MARKER_SPACING_M + 0.5f);
static constexpr uint16_t LEDS_PER_STRIP = 180; // 7.5 m * 24 sections/m
static constexpr uint16_t TOTAL_LEDS = LEDS_PER_STRIP * NUM_STRIPS;

// Change these to suit your wiring.
static constexpr uint8_t DATA_PINS[NUM_STRIPS] = {19, 22, 21};

// If colours are wrong, try RGB instead of GRB.
#define LED_COLOR_ORDER RGB

CRGB leds1[LEDS_PER_STRIP];
CRGB leds2[LEDS_PER_STRIP];
CRGB leds3[LEDS_PER_STRIP];
CRGB *strips[NUM_STRIPS] = {leds1, leds2, leds3};

// -----------------------------
// Runtime settings
// -----------------------------
static bool animationRunning = true;
static bool reverseDirection = false;
static float paceSpeedMps = 6.70f; // initial speed
static uint8_t brightness = 80;    // 1 to 255
static CRGB baseColor = CRGB::Blue;

// Dot appearance in LED sections.
static constexpr uint8_t DOT_LENGTH_LEDS = 6;
static constexpr uint8_t FADE_LEDS = 6;

// Frame timing
static constexpr uint16_t FRAME_MS = 20;
uint32_t lastFrameMs = 0;
float phaseLeds = 0.0f;

// Startup sparkle animation
static constexpr uint32_t BOOT_ANIMATION_MS = 5000;
uint32_t bootStartMs = 0;
uint32_t bootNextUpdateMs = 0;

// -----------------------------
// Simple web UI
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
      --bg2: #101a33;
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
      padding: 18px 4px 8px;
      margin-bottom: 18px;
    }

    h1 {
      margin: 0 0 8px;
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
      .card.third { grid-column: span 4; }
    }

    .status {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 8px 12px;
      border-radius: 999px;
      background: rgba(255,255,255,0.08);
      border: 1px solid rgba(255,255,255,0.10);
      font-weight: 700;
    }

    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: var(--accent2);
      box-shadow: 0 0 14px var(--accent2);
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

    input[type=range] {
      width: 100%;
      accent-color: var(--accent);
    }

    input[type=color] {
      width: 100%;
      height: 52px;
      border: 1px solid rgba(255,255,255,0.15);
      border-radius: 14px;
      background: transparent;
      padding: 4px;
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

    .hint {
      color: var(--muted);
      font-size: 0.95rem;
      margin-top: 8px;
    }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <h1>Pacing Lights</h1>
      <div class="subtitle">WiFi control for runner pacing dots, split times, and colour presets.</div>
    </div>

    <div class="grid">
      <div class="card">
        <div class="status"><span class="dot"></span><span id="status">Loading...</span></div>
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
            <input id="speed" type="range" min="0.01" max="13" step="0.01" value="6.70" />
            <div class="readout">Drag to set runner speed directly.</div>
          </div>

          <div>
            <label for="brightness">Brightness</label>
            <input id="brightness" type="range" min="1" max="255" step="1" value="80" />
            <div class="readout">Lower this for outdoor visibility without glare.</div>
          </div>

          <div>
            <label for="color">Dot colour</label>
            <input id="color" type="color" value="#00ff66" />
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

          <div class="hint">
            The device starts with a brief sparkle pattern: random colours flashing on about 10% of the LEDs.
          </div>
        </div>
      </div>
    </div>
  </div>

  <script>
    const speedEl = document.getElementById('speed');
    const brightnessEl = document.getElementById('brightness');
    const colorEl = document.getElementById('color');
    const reverseEl = document.getElementById('reverse');
    const statusEl = document.getElementById('status');
    const speedValEl = document.getElementById('speedVal');
    const splitValEl = document.getElementById('splitVal');
    const paceKmValEl = document.getElementById('paceKmVal');
    const paceMiValEl = document.getElementById('paceMiVal');

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

    async function sendState(running) {
      const payload = {
        running: running,
        reverse: reverseEl.checked,
        speed: parseFloat(speedEl.value),
        brightness: parseInt(brightnessEl.value, 10),
        color: colorEl.value
      };

      const res = await fetch('/set', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });

      const data = await res.json();
      statusEl.textContent = data.running ? 'Running' : 'Stopped';
      reverseEl.checked = !!data.reverse;
      if (data.color) colorEl.value = data.color;
      updateDerived();
    }

    async function refreshStatus() {
      const res = await fetch('/state');
      const data = await res.json();
      speedEl.value = data.speed;
      brightnessEl.value = data.brightness;
      reverseEl.checked = !!data.reverse;
      if (data.color) colorEl.value = data.color;
      statusEl.textContent = data.running ? 'Running' : 'Stopped';
      updateDerived();
    }

    speedEl.addEventListener('input', updateDerived);
    brightnessEl.addEventListener('input', () => {});
    speedEl.addEventListener('change', () => sendState(true));
    brightnessEl.addEventListener('change', () => sendState(true));
    colorEl.addEventListener('input', () => sendState(true));
    colorEl.addEventListener('change', () => sendState(true));
    reverseEl.addEventListener('change', () => sendState(true));

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

static void setGlobalLed(uint16_t index, const CRGB &c) {
  if (index >= TOTAL_LEDS)
    return;
  uint16_t stripIndex = index / LEDS_PER_STRIP;
  uint16_t localIndex = index % LEDS_PER_STRIP;
  strips[stripIndex][localIndex] = c;
}

static void clearAllStrips() {
  for (uint8_t s = 0; s < NUM_STRIPS; ++s) {
    fill_solid(strips[s], LEDS_PER_STRIP, CRGB::Black);
  }
}

static void renderStrip(CRGB *leds, uint16_t count, float phaseOffsetLeds) {
  for (uint16_t i = 0; i < count; ++i) {
    leds[i] = CRGB::Black;

    // Position of this section within one spacing unit.
    float x = (float)i + phaseOffsetLeds;
    float local = fmodf(x, (float)MARKER_SPACING_LEDS);
    if (local < 0.0f) {
      local += MARKER_SPACING_LEDS;
    }

    // Dot centre is in the middle of each spacing region.
    float centre = (float)MARKER_SPACING_LEDS * 0.5f;
    float dist = fabsf(local - centre);

    float halfDot = (float)DOT_LENGTH_LEDS * 0.5f;
    float intensity = 0.0f;

    if (dist <= halfDot) {
      intensity = 1.0f;
    } else if (dist <= halfDot + FADE_LEDS) {
      intensity = 1.0f - ((dist - halfDot) / (float)FADE_LEDS);
    }

    if (intensity > 0.0f) {
      CRGB c = baseColor;
      c.nscale8_video((uint8_t)(255.0f * intensity));
      leds[i] = c;
    }
  }
}

static void applyBrightness() { FastLED.setBrightness(brightness); }

static void updateBootAnimation() {
  uint32_t now = millis();

  if (now - bootStartMs >= BOOT_ANIMATION_MS) {
    clearAllStrips();
    FastLED.show();
    return;
  }

  if (now < bootNextUpdateMs) {
    return;
  }

  clearAllStrips();

  const uint16_t litCount = max<uint16_t>(1, TOTAL_LEDS / 10); // about 10%
  for (uint16_t i = 0; i < litCount; ++i) {
    uint16_t idx = (uint16_t)random(TOTAL_LEDS);
    CHSV hsv((uint8_t)random(0, 255), 255, (uint8_t)random(120, 255));
    setGlobalLed(idx, hsv);
  }

  FastLED.show();
  bootNextUpdateMs = now + (uint32_t)random(80, 350);
}

static void updateAnimation() {
  // Startup sparkle mode.
  if (millis() - bootStartMs < BOOT_ANIMATION_MS) {
    updateBootAnimation();
    return;
  }

  if (!animationRunning) {
    clearAllStrips();
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
  phaseLeds += reverseDirection ? -delta : delta;

  // Keep the phase from growing indefinitely.
  if (phaseLeds > 100000.0f || phaseLeds < -100000.0f) {
    phaseLeds = fmodf(phaseLeds, (float)MARKER_SPACING_LEDS);
  }

  for (uint8_t s = 0; s < NUM_STRIPS; ++s) {
    renderStrip(strips[s], LEDS_PER_STRIP, phaseLeds);
  }

  FastLED.show();
}

static String jsonState() {
  String out = "{";
  out += "\"running\":" + String(animationRunning ? "true" : "false") + ",";
  out += "\"reverse\":" + String(reverseDirection ? "true" : "false") + ",";
  out += "\"speed\":" + String(paceSpeedMps, 2) + ",";
  out += "\"brightness\":" + String(brightness) + ",";

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
    paceSpeedMps = constrain(speed, 1.0f, 13.0f);
  }

  float bright = brightness;
  if (extractJsonFloat(body, "brightness", bright)) {
    int val = (int)bright;
    val = constrain(val, 1, 255);
    brightness = (uint8_t)val;
    applyBrightness();
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

  // LED setup
  FastLED.addLeds<WS2811, DATA_PINS[0], LED_COLOR_ORDER>(leds1, LEDS_PER_STRIP);
  FastLED.addLeds<WS2811, DATA_PINS[1], LED_COLOR_ORDER>(leds2, LEDS_PER_STRIP);
  FastLED.addLeds<WS2811, DATA_PINS[2], LED_COLOR_ORDER>(leds3, LEDS_PER_STRIP);
  FastLED.setDither(true);
  applyBrightness();
  clearAllStrips();
  FastLED.show();

  // Access point mode with explicit IP so phones can reach it reliably.
  WiFi.mode(WIFI_AP);
  IPAddress local_IP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/state", HTTP_GET, handleState);
  server.on("/set", HTTP_POST, handleSet);
  server.begin();

  bootStartMs = millis();
  bootNextUpdateMs = 0;
  lastFrameMs = millis();
}

void loop() {
  server.handleClient();
  updateAnimation();
}
