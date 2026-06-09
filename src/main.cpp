// Runner pacing light controller
// ESP32 + 3x WS2811/FCOB strips
//
// Features:
// - WiFi access point at 192.168.4.1
// - Browser-based control page
// - Three independent LED strip outputs
// - Moving dot pattern with wider spacing
// - Soft fade around each dot
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
    4.0f; // quadrupled spacing between moving lights
static constexpr uint16_t MARKER_SPACING_LEDS =
    (uint16_t)(LEDS_PER_METER * MARKER_SPACING_M + 0.5f);
static constexpr uint16_t LEDS_PER_STRIP = 180; // 7.5 m * 24 sections/m

// Change these to suit your wiring.
static constexpr uint8_t DATA_PINS[NUM_STRIPS] = {19, 22, 21};

// If colours are wrong, try RGB instead of GRB.
#define LED_COLOR_ORDER GRB

CRGB leds1[LEDS_PER_STRIP];
CRGB leds2[LEDS_PER_STRIP];
CRGB leds3[LEDS_PER_STRIP];
CRGB *strips[NUM_STRIPS] = {leds1, leds2, leds3};

// -----------------------------
// Runtime settings
// -----------------------------
static bool animationRunning = true;
static float paceSpeedMps = 3.0f; // 1 to 13 m/s
static uint8_t brightness = 80;   // 1 to 255
static CRGB baseColor = CRGB::Green;

// Dot appearance in LED sections.
static constexpr uint8_t DOT_LENGTH_LEDS = 6;
static constexpr uint8_t FADE_LEDS = 6;

// Frame timing
static constexpr uint16_t FRAME_MS = 20;
uint32_t lastFrameMs = 0;
float phaseLeds = 0.0f;

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
    body { font-family: Arial, sans-serif; max-width: 720px; margin: 20px auto; padding: 0 12px; }
    .card { border: 1px solid #ddd; border-radius: 12px; padding: 16px; margin-bottom: 16px; }
    label { display: block; margin-top: 12px; }
    input[type=range] { width: 100%; }
    input[type=color] { width: 100%; height: 44px; border: 1px solid #ccc; border-radius: 8px; padding: 0; }
    button { padding: 10px 14px; margin-right: 8px; margin-top: 10px; }
    .status { font-weight: bold; }
  </style>
</head>
<body>
  <h1>Pacing Lights</h1>

  <div class="card">
    <div>Status: <span id="status" class="status">Loading...</span></div>
    <div>Speed: <span id="speedVal"></span> m/s</div>
    <div>Brightness: <span id="brightVal"></span></div>
  </div>

  <div class="card">
    <label for="speed">Pace speed (m/s)</label>
    <input id="speed" type="range" min="1" max="13" step="0.1" value="3" />

    <label for="brightness">Brightness</label>
    <input id="brightness" type="range" min="1" max="255" step="1" value="80" />

    <label for="color">Dot colour</label>
    <input id="color" type="color" value="#00ff66" />

    <div>
      <button onclick="sendState(true)">Start</button>
      <button onclick="sendState(false)">Stop</button>
    </div>
  </div>

  <script>
    const speedEl = document.getElementById('speed');
    const brightnessEl = document.getElementById('brightness');
    const colorEl = document.getElementById('color');
    const statusEl = document.getElementById('status');
    const speedValEl = document.getElementById('speedVal');
    const brightValEl = document.getElementById('brightVal');

    function updateLabels() {
      speedValEl.textContent = Number(speedEl.value).toFixed(1);
      brightValEl.textContent = brightnessEl.value;
    }

    async function sendState(running) {
      const payload = {
        running: running,
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
      updateLabels();
    }

    async function refreshStatus() {
      const res = await fetch('/state');
      const data = await res.json();
      speedEl.value = data.speed;
      brightnessEl.value = data.brightness;
      if (data.color) colorEl.value = data.color;
      statusEl.textContent = data.running ? 'Running' : 'Stopped';
      updateLabels();
    }

    speedEl.addEventListener('change', () => sendState(true));
    brightnessEl.addEventListener('change', () => sendState(true));
    colorEl.addEventListener('change', () => sendState(true));
    speedEl.addEventListener('input', updateLabels);
    brightnessEl.addEventListener('input', updateLabels);

    updateLabels();
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

static void updateAnimation() {
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
  phaseLeds += ledsPerSecond * dt;

  // Keep the phase from growing indefinitely.
  if (phaseLeds > 100000.0f) {
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

  lastFrameMs = millis();
}

void loop() {
  server.handleClient();
  updateAnimation();
}
