#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Adafruit_NeoPixel.h>
#include <esp_wifi.h>

// --- LED ---
#define LED_PIN   27   // WS2812 RGB LED on ESP32-C5 Zero (GPIO27)
#define NUM_LEDS  1
Adafruit_NeoPixel led(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- WiFi ---
#include "credentials.h"


// --- Antenna switch ---
// GPIO26 controls the onboard RF switch on ESP32-C5 Zero:
//   HIGH = integrated PCB antenna (default)
//   LOW  = external IPEX connector
// NOTE: GPIO26 is a strapping pin; it is latched at reset and then freed as GPIO.
#define ANTENNA_SEL_PIN 26

// --- Serial input/output ---
// Connect external TX to GPIO4 (UART1 RX on ESP32-C5 Zero)
// Connect external RX to GPIO6 (UART1 TX on ESP32-C5 Zero)
// NOTE: GPIO5 was tried but is a JTAG (MTDO) pin with input-enabled default after reset,
//       causing unreliable TX drive. GPIO6 is a clean P2 pin with no restrictions.
#define SERIAL_RX_PIN  4
#define SERIAL_TX_PIN  6
#define SERIAL_BAUD    115200

// --- WebSocket + HTTP server ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
static bool serverPaused = false;

// --- Flags set from WS callback, consumed in loop() ---
static bool pendingPaused  = false;
static bool pendingResumed = false;

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        s->cleanupClients();  // evict stale connections immediately so new client isn't queued
        client->text(serverPaused ? "PAUSED" : "RESUMED");
    } else if (type == WS_EVT_DATA) {
        if (len == 8 && memcmp(data, "PAUSEALL", 8) == 0) {
            serverPaused = true;
            pendingPaused = true;
        } else if (len == 9 && memcmp(data, "RESUMEALL", 9) == 0) {
            serverPaused = false;
            pendingResumed = true;
        } else if (len > 3 && memcmp(data, "TX:", 3) == 0) {
            Serial.printf("[tx] sending %u bytes: %.*s\n",
                          (unsigned)(len - 3), (int)(len - 3), (const char*)data + 3);
            Serial1.write(data + 3, len - 3);
            Serial1.write('\n');
            char ack[128];
            int ackLen = snprintf(ack, sizeof(ack), "TXOK:%.*s",
                                  (int)(len - 3), (const char*)data + 3);
            if (ackLen > 0) {
                if (ackLen >= (int)sizeof(ack)) ackLen = sizeof(ack) - 1;
                s->textAll((uint8_t*)ack, ackLen);
            }
        }
    }
}

static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head>
<meta charset='utf-8'>
<title>ser2wifi</title>
<style>
  body { background:#111; color:#ccc; font-family:monospace; padding:1em; margin:0; }
  h2   { color:#4f4; margin-bottom:.3em; }
  #out { white-space:pre-wrap; word-break:break-all;
         background:#000; padding:1em; border:1px solid #333;
         height:82vh; overflow-y:auto;
         font-family:monospace; font-size:.9em; line-height:1.4; }
  #bar { display:flex; align-items:center; gap:.6em; margin-top:.5em; }
  #status { font-size:.8em; flex:1; color:#aaa; }
  button { background:#222; color:#ccc; border:1px solid #444;
           padding:.3em .9em; cursor:pointer; font-family:monospace; }
  button:hover { background:#333; }
  #btnPause      { color:#fc4; border-color:#fc4; }
  #btnResume     { color:#4f4; border-color:#4f4; display:none; }
  #btnPauseAll   { color:#f84; border-color:#f84; }
  #btnResumeAll  { color:#4af; border-color:#4af; display:none; }
  #btnCopy       { color:#58a; border-color:#58a; margin-left:auto; }
  #btnCopy:hover  { background:#111a2a; }
  #btnSave       { color:#58a; border-color:#58a; }
  #btnSave:hover  { background:#111a2a; }
  #btnClear      { color:#a55; border-color:#a55; }
  #btnClear:hover { background:#2a1111; }
  #sendBar { display:flex; gap:.4em; margin-top:.4em; }
  #sendInput { flex:1; background:#000; color:#ccc; border:1px solid #444;
               padding:.3em .6em; font-family:monospace; font-size:.9em; }
  #sendInput:focus { outline:none; border-color:#4af; }
  #btnSend { background:#222; color:#4af; border:1px solid #4af;
             padding:.3em .9em; cursor:pointer; font-family:monospace; }
  #btnSend:hover { background:#112; }
</style>
</head><body>
<h2>ser2wifi</h2>
<div id='out'></div>
<div id='bar'>
  <button id='btnPause'     onclick='doPause()'>&#9646;&#9646; PAUSE</button>
  <button id='btnResume'    onclick='doResume()'>&#9654; RESUME</button>
  <button id='btnPauseAll'  onclick='doPauseAll()'>&#9646;&#9646; PAUSE ALL</button>
  <button id='btnResumeAll' onclick='doResumeAll()'>&#9654; RESUME ALL</button>
  <span id='status'>connecting...</span>
  <button id='btnCopy'  onclick='doCopy()'>&#128203; COPY</button>
  <button id='btnSave'  onclick='doSave()'>&#8659; SAVE</button>
  <button id='btnClear' onclick='doClear()'>&#10005; CLEAR</button>
</div>
<div id='sendBar'>
  <input id='sendInput' type='text' placeholder='Type text to send via serial...' autocomplete='off'>
  <button id='btnSend' onclick='doSend()'>SEND</button>
</div>
<script>
const FG = {
  30:'#555', 31:'#c33', 32:'#3c3', 33:'#cc3',
  34:'#44c', 35:'#c3c', 36:'#3cc', 37:'#ccc',
  90:'#888', 91:'#f55', 92:'#5f5', 93:'#ff5',
  94:'#77f', 95:'#f5f', 96:'#5ff', 97:'#fff'
};
const BG = {
  40:'#000', 41:'#300', 42:'#030', 43:'#330',
  44:'#003', 45:'#303', 46:'#033', 47:'#555',
  100:'#333',101:'#f55',102:'#5f5',103:'#ff5',
  104:'#77f',105:'#f5f',106:'#5ff',107:'#fff'
};

const MAX_RAW = 200000;  // 200K circular buffer

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// Active style state — persists across chunks for incremental rendering
let ansiState = { fg:null, bg:null, bold:false, italic:false, underline:false, dim:false };

function ansiStateToStyle(st) {
  const s = [];
  if (st.fg)        s.push('color:'           + st.fg);
  if (st.bg)        s.push('background:'      + st.bg);
  if (st.bold)      s.push('font-weight:bold');
  if (st.italic)    s.push('font-style:italic');
  if (st.underline) s.push('text-decoration:underline');
  if (st.dim)       s.push('opacity:.6');
  return s.join(';');
}

function ansiStateActive(st) {
  return st.fg || st.bg || st.bold || st.italic || st.underline || st.dim;
}

// Apply SGR codes to a style state object, return whether style changed
function applySgr(codes, st) {
  for (const c of codes) {
    if (c === 0) {
      st.fg = null; st.bg = null;
      st.bold = st.italic = st.underline = st.dim = false;
    } else if (FG[c]) st.fg = FG[c];
      else if (BG[c]) st.bg = BG[c];
      else if (c === 1) st.bold      = true;
      else if (c === 3) st.italic    = true;
      else if (c === 4) st.underline = true;
      else if (c === 2) st.dim       = true;
      else if (c === 22) st.bold     = false;
      else if (c === 23) st.italic   = false;
      else if (c === 24) st.underline= false;
      else if (c === 39) st.fg       = null;
      else if (c === 49) st.bg       = null;
  }
}

// Render raw ANSI text to HTML using the given state (mutated in place).
// closeAtEnd: if true, closes any open span at end (full re-render).
//             if false, closes the span but leaves state so next chunk re-opens it (incremental).
function renderAnsi(raw, st, closeAtEnd) {
  let out = '';
  let spanOpen = ansiStateActive(st);
  // Re-open span from previous state if we're continuing mid-color
  if (spanOpen) out += '<span style="' + ansiStateToStyle(st) + '">';
  const re = /\x1b\[([0-9;]*)m/g;
  let last = 0, m;
  while ((m = re.exec(raw)) !== null) {
    out += escHtml(raw.slice(last, m.index));
    last = m.index + m[0].length;
    const codes = m[1] === '' ? [0] : m[1].split(';').map(Number);
    const wasActive = ansiStateActive(st);
    applySgr(codes, st);
    const nowActive = ansiStateActive(st);
    if (wasActive) out += '</span>';
    if (nowActive) out += '<span style="' + ansiStateToStyle(st) + '">';
    spanOpen = nowActive;
  }
  out += escHtml(raw.slice(last));
  if (spanOpen) {
    out += '</span>';
    // For incremental mode, state is preserved so next chunk re-opens the span
  }
  return out;
}

function ansiToHtml(raw) {
  // Full re-render: reset state first
  ansiState = { fg:null, bg:null, bold:false, italic:false, underline:false, dim:false };
  return renderAnsi(raw, ansiState, true);
}

function ansiAppend(raw) {
  // Incremental: continue with current ansiState
  return renderAnsi(raw, ansiState, false);
}

const el     = document.getElementById('out');
const status = document.getElementById('status');
let paused   = false;
let rawBuf   = '';   // circular raw text buffer (15K max)
let totalBytes = 0;

function trimBuf() {
  if (rawBuf.length <= MAX_RAW) return false;
  // Trim to MAX_RAW, try to cut at a newline boundary
  const excess = rawBuf.length - MAX_RAW;
  const nl = rawBuf.indexOf('\n', excess);
  rawBuf = rawBuf.slice(nl >= 0 ? nl + 1 : excess);
  return true; // re-render needed
}

function appendData(text) {
  rawBuf += text;
  const needsRerender = trimBuf();
  if (needsRerender) {
    // Re-render full buffer — ansiToHtml resets ansiState internally
    el.innerHTML = ansiToHtml(rawBuf);
  } else {
    el.innerHTML += ansiAppend(text);
  }
  el.scrollTop = el.scrollHeight;
  totalBytes += text.length;
  status.innerHTML = '<span style="color:#4f4">&#9679; connected</span>'
                   + '<span style="color:#aaa"> &mdash; ' + rawBuf.length + ' / ' + MAX_RAW + ' bytes</span>';
}

function doPause() {
  paused = true;
  document.getElementById('btnPause').style.display  = 'none';
  document.getElementById('btnResume').style.display = 'inline-block';
  status.innerHTML = '<span style="color:#fc4">&#9646;&#9646; paused</span>';
}
function doResume() {
  paused = false;
  document.getElementById('btnPause').style.display  = 'inline-block';
  document.getElementById('btnResume').style.display = 'none';
}
function stripAnsi(s) {
  // CSI sequences: ESC [ <parameter bytes 0x20-0x3f>* <final byte 0x40-0x7e>
  // This covers all SGR, cursor, erase, mode, and private sequences (e.g. \x1b[?25l)
  return s.replace(/\x1b\[[\x20-\x3f]*[\x40-\x7e]/g, '')
          .replace(/\x1b[^[]/g, '');  // also strip other 2-char ESC sequences (e.g. ESC= ESC>)
}
function doCopy() {
  const text = stripAnsi(rawBuf);
  const ok = () => {
    const prev = status.innerHTML;
    status.innerHTML = '<span style="color:#5af">&#10003; copied to clipboard</span>';
    setTimeout(() => { status.innerHTML = prev; }, 1500);
  };
  const fail = () => {
    status.innerHTML = '<span style="color:#f55">copy failed</span>';
  };
  // navigator.clipboard requires HTTPS; fall back to execCommand for plain HTTP (local IP)
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(ok).catch(fail);
  } else {
    const ta = document.createElement('textarea');
    ta.value = text;
    ta.style.cssText = 'position:fixed;top:-9999px;left:-9999px;opacity:0';
    document.body.appendChild(ta);
    ta.focus();
    ta.select();
    try {
      document.execCommand('copy') ? ok() : fail();
    } catch(e) {
      fail();
    } finally {
      document.body.removeChild(ta);
    }
  }
}
function doSave() {
  const ts = new Date().toISOString().replace(/[:.]/g,'-').slice(0,19);
  const blob = new Blob([stripAnsi(rawBuf)], { type: 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'ser2wifi-' + ts + '.txt';
  a.click();
  URL.revokeObjectURL(a.href);
}
function doClear() {
  rawBuf = '';
  ansiState = { fg:null, bg:null, bold:false, italic:false, underline:false, dim:false };
  totalBytes = 0;
  el.innerHTML = '';
  status.innerHTML = '<span style="color:#aaa">cleared</span>';
}
function doPauseAll()  { ws.send('PAUSEALL');  }
function doResumeAll() { ws.send('RESUMEALL'); }
function doSend() {
  const inp = document.getElementById('sendInput');
  const txt = inp.value;
  if (!txt) return;
  ws.send('TX:' + txt);
}
document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('sendInput').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); doSend(); }
  });
});

function setServerPaused(p) {
  document.getElementById('btnPauseAll').style.display  = p ? 'none'         : 'inline-block';
  document.getElementById('btnResumeAll').style.display = p ? 'inline-block' : 'none';
}

const dec = new TextDecoder();
let ws;
let reconnectTimer = null;
let watchdogTimer  = null;
const WATCHDOG_MS  = 6000;
function resetWatchdog() {
  clearTimeout(watchdogTimer);
  watchdogTimer = setTimeout(() => {
    status.innerHTML = '<span style="color:#f84">&#9679; timeout, reconnecting...</span>';
    scheduleReconnect();
  }, WATCHDOG_MS);
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  status.innerHTML = '<span style="color:#f44">&#9679; disconnected, reconnecting...</span>';
  reconnectTimer = setTimeout(() => { reconnectTimer = null; connect2(); }, 1000);
}

function connect2() {
  if (ws) {
    ws.onopen    = null;
    ws.onmessage = null;
    ws.onclose   = null;
    ws.onerror   = null;
    if (ws.readyState !== WebSocket.CLOSED) ws.close();
  }
  ws = new WebSocket('ws://__IP__/ws');
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    status.innerHTML = '<span style="color:#4f4">&#9679; connected</span>';
    resetWatchdog();
  };

  ws.onmessage = (e) => {
    resetWatchdog();
    if (typeof e.data === 'string') {
      if      (e.data === 'PAUSED')  setServerPaused(true);
      else if (e.data === 'RESUMED') setServerPaused(false);
      else if (e.data.startsWith('TXOK:')) {
        if (!paused) appendData('\x1b[36m>> ' + e.data.substring(5) + '\x1b[0m\n');
      }
      // else: PING or unknown control message — ignore
      return;
    }
    if (paused) return;
    appendData(dec.decode(e.data));
  };

  ws.onclose = () => { clearTimeout(watchdogTimer); scheduleReconnect(); };
  ws.onerror = () => { clearTimeout(watchdogTimer); scheduleReconnect(); };
}

connect2();
</script>
</body></html>)HTML";

static String cachedPage;

void buildPage() {
    cachedPage = PAGE;
    cachedPage.replace("__IP__", WiFi.localIP().toString());
}

void handleRoot(AsyncWebServerRequest* req) {
    req->send(200, "text/html", cachedPage);
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
    led.setPixelColor(0, led.Color(r, g, b));
    led.show();
}

// Called only once from setup(). Feeds the WDT while waiting.
// Does NOT call WiFi.disconnect() — WiFi.mode(STA)+begin() already starts the
// STA cleanly. A redundant disconnect causes a double STA-start and intermittent
// ASSOC_FAIL. We also must NOT hard-disconnect a connection that is mid-DHCP, or
// the AP reports ASSOC_LEAVE (reason 8) and we lose a good association.
void wifiConnect() {
    WiFi.begin(SSID, PASSWORD);  // credentials stored for autoReconnect; scans all channels
    uint32_t start = millis();
    uint32_t lastBlink = 0;
    bool ledOn = false;
    // Note: on ESP32-C3 the first association after a cold boot usually fails once
    // with ASSOC_FAIL (203) and the driver's own auto-retry succeeds ~1s later.
    // We do NOT call begin() again here — the driver rejects it while "connecting"
    // ("sta is connecting, return error"). We only force a fresh begin() if the
    // driver has gone fully idle for a long time (stuck), to avoid a permanent hang.
    while (WiFi.status() != WL_CONNECTED) {
        uint32_t now = millis();
        if (now - lastBlink >= 250) {
            lastBlink = now;
            ledOn = !ledOn;
            setLed(ledOn ? 255 : 0, 0, 0);
        }
        // Hard fallback: if still not connected after 20s, restart the connection.
        if (now - start >= 20000) {
            start = now;
            WiFi.disconnect();
            delay(100);
            WiFi.begin(SSID, PASSWORD);
        }
        delay(10);
    }
    setLed(0, 255, 0);
}

// Set in the WiFi event task on GOT_IP; consumed (non-blocking) in loop().
static volatile bool needRebuildPage = false;

// WiFi event handler — updates LED without blocking loop()
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            Serial.printf("[%lu][wifi] STA start\n", (unsigned long)millis());
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.printf("[%lu][wifi] associated to AP, ch %u\n",
                          (unsigned long)millis(), info.wifi_sta_connected.channel);
            // IP not yet assigned; stay dim orange
            setLed(80, 40, 0);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.printf("[%lu][wifi] got IP %s\n",
                          (unsigned long)millis(), WiFi.localIP().toString().c_str());
            // Do NOT block here — this runs in the WiFi event task. Blocking it
            // (delay + heavy String copy) stalls the stack and can trigger drops.
            // Defer buildPage() to loop() via a flag. LED is driven by loop().
            needRebuildPage = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // reason codes: 2=auth_expire 15=4way_handshake_timeout 201=no_ap_found
            //               202=auth_fail 203=assoc_fail 8=assoc_leave
            Serial.printf("[%lu][wifi] DISCONNECTED, reason %u\n",
                          (unsigned long)millis(), info.wifi_sta_disconnected.reason);
            // LED is driven entirely by loop() from WiFi.status(); nothing to do here.
            break;
        default:
            break;
    }
}

// Pending bytes to broadcast to WS clients
static char wsbatch[1024];
static int  wsbatch_len = 0;
static uint32_t ws_last_flush = 0;

void setup() {
    Serial.begin(115200);     // USB-CDC log port
    delay(300);               // give the USB serial host time to attach
    Serial.println();
    Serial.println("[boot] ser2wifi starting");

    led.begin();
    led.setBrightness(40);
    setLed(255, 80, 0);

    // Select external IPEX antenna (LOW). Change to HIGH to use the integrated PCB antenna.
    pinMode(ANTENNA_SEL_PIN, OUTPUT);
    digitalWrite(ANTENNA_SEL_PIN, LOW);

    Serial1.begin(SERIAL_BAUD, SERIAL_8N1, SERIAL_RX_PIN, SERIAL_TX_PIN);

    WiFi.onEvent(onWifiEvent);  // must be before WiFi.mode()
    WiFi.mode(WIFI_STA);

    // Regulatory domain: allow channels 1-13 (EU). Without this the driver may
    // refuse/deprioritize channel 13 where the AP lives, causing ASSOC_FAIL.
    wifi_country_t ctry = { .cc = "EU", .schan = 1, .nchan = 13,
                            .max_tx_power = 84, .policy = WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&ctry);

    // Disable WiFi power-save and modem sleep. At weak signal (~-82 dBm) power-save
    // is a common cause of slow/failed association and laggy WebSocket traffic.
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    WiFi.setAutoReconnect(true);

    Serial.printf("[wifi] connecting to SSID '%s' ...\n", SSID);
    uint32_t t0 = millis();
    wifiConnect();
    Serial.printf("[wifi] associated in %lu ms, RSSI %d dBm\n",
                  (unsigned long)(millis() - t0), WiFi.RSSI());

    buildPage();              // initial build (safe here — not in event context)
    needRebuildPage = false;

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.on("/", HTTP_GET, handleRoot);
    server.begin();
    Serial.println("[http] server started");
}

void loop() {
    // Read serial into batch buffer
    while (Serial1.available()) {
        char c = (char)Serial1.read();
        if (wsbatch_len < (int)sizeof(wsbatch)) {
            wsbatch[wsbatch_len++] = c;
        }
    }

    uint32_t now = millis();

    // Rebuild the cached page if a new IP was obtained (deferred from event task)
    if (needRebuildPage) {
        needRebuildPage = false;
        buildPage();
    }

    // Handle PAUSE ALL broadcast
    if (pendingPaused) {
        pendingPaused = false;
        wsbatch_len = 0;  // discard pending batch
        ws.textAll("PAUSED");
    }

    // Handle RESUME ALL broadcast
    if (pendingResumed) {
        pendingResumed = false;
        ws.textAll("RESUMED");
    }

    // Flush batch to all WS clients every 50ms (or when batch is full)
    if (!serverPaused && wsbatch_len > 0 && (now - ws_last_flush >= 50 || wsbatch_len >= (int)sizeof(wsbatch))) {
        ws.binaryAll((uint8_t*)wsbatch, wsbatch_len);
        wsbatch_len   = 0;
        ws_last_flush = now;
    }

    // Periodic PING so clients detect dead connections quickly
    static uint32_t lastPing = 0;
    if (now - lastPing >= 3000) {
        lastPing = now;
        ws.textAll("PING");
    }

    static uint32_t lastCleanup = 0;
    if (now - lastCleanup >= 1000) {
        lastCleanup = now;
        ws.cleanupClients();
    }

    // --- LED status: single source of truth, driven from real WiFi.status() ---
    // This must run every loop so the LED can NEVER get stuck (e.g. reconnect with a
    // retained DHCP lease fires no GOT_IP event, so events alone are unreliable).
    //   connected            -> solid green
    //   recently disconnected -> orange (reconnecting), give it time
    //   long outage (>15s)    -> red blink, and kick a manual reconnect
    static uint32_t disconnectedSince = 0;
    static uint32_t ledBlink = 0;
    static bool     ledOn = false;
    static uint32_t lastKick = 0;

    if (WiFi.status() == WL_CONNECTED) {
        disconnectedSince = 0;
        setLed(0, 255, 0);                 // solid green
    } else {
        if (disconnectedSince == 0) disconnectedSince = now;
        uint32_t down = now - disconnectedSince;

        if (down < 15000) {
            // Still within grace window: steady orange, let autoReconnect work.
            setLed(255, 60, 0);
        } else {
            // Long outage: blink red and periodically force a reconnect attempt.
            if (now - ledBlink >= 250) {
                ledBlink = now;
                ledOn = !ledOn;
                setLed(ledOn ? 255 : 0, 0, 0);
            }
            if (now - lastKick >= 10000) {
                lastKick = now;
                Serial.printf("[%lu][wifi] long outage, forcing reconnect\n",
                              (unsigned long)now);
                WiFi.disconnect();
                delay(50);
                WiFi.begin(SSID, PASSWORD);
            }
        }
    }
}
