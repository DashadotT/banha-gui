/*
   ====================================================
   BANHA LoRa Node 2  (v2)
   RECEIVER + SUPABASE UPLOADER + WIFI WEB CONFIG

   WHAT CHANGED FROM THE PREVIOUS VERSION
   ====================================================
   1. ACKNOWLEDGMENTS
      Every valid START / STOP / DATA packet from Node 1
      now carries a transport sequence number (SEQ:n).
      As soon as Node 2 recognizes a well-formed packet it
      immediately replies with:

        NODE:2,TYPE:ACK,SEQ:<n>,ACKTYPE:<START|STOP|DATA>

      The ACK is sent BEFORE the (potentially slow)
      Supabase HTTP call, so Node 1 stops retrying as soon
      as the radio message itself got through, regardless
      of how long the WiFi/Supabase step takes.

   2. DUPLICATE-SAFE PROCESSING
      Node 1 will retransmit a message if it doesn't see
      an ACK in time, so Node 2 now recognizes duplicates
      two different ways and NEVER creates a duplicate
      database record because of a resend:
        - A short history of recently-seen SEQ numbers
          catches exact retransmissions (the most common
          case). A duplicate SEQ still gets an ACK sent
          back (in case the first ACK was lost) but is not
          reprocessed.
        - For DATA specifically, the PACKET number is also
          checked against the last packet number actually
          uploaded for the current recording, as a second
          safety net.

   3. MISSING DATA PACKET DETECTION
      If a DATA packet's PACKET number arrives higher than
      "last uploaded + 1", Node 2 logs exactly which
      packet number(s) appear to be missing. With Node 1
      now retrying until ACKed, this should be rare, but
      it's still detected and logged for visibility.

   Everything else - WiFi setup portal, non-blocking WiFi
   connection/reconnection, the dashboard web UI, and the
   Supabase create/upload/stop functions - is unchanged.
   ====================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <SPI.h>
#include <LoRa.h>
#include <stdlib.h>


// =====================================================
// ACCESS POINT SETTINGS
// =====================================================

const char* AP_SSID = "BANHA-SETUP";
const char* AP_PASSWORD = "banha@nbsc2026";


// =====================================================
// WEB SERVER
// =====================================================

WebServer server(80);


// =====================================================
// PREFERENCES
// =====================================================

Preferences preferences;


// =====================================================
// SAVED WIFI
// =====================================================

String savedWiFiSSID = "";
String savedWiFiPassword = "";


// =====================================================
// WIFI STATUS
// =====================================================

bool wifiConnected = false;
bool wifiConnectionAttempting = false;

unsigned long wifiConnectStartMillis = 0;
unsigned long lastWiFiReconnectAttempt = 0;

const unsigned long WIFI_CONNECT_TIMEOUT = 15000;
const unsigned long WIFI_RECONNECT_INTERVAL = 15000;


// =====================================================
// SUPABASE SETTINGS
// =====================================================

const char* SUPABASE_URL = "https://jeiolvlujtnmkwprppdu.supabase.co";

// Use ANON / PUBLISHABLE KEY
// DO NOT USE SERVICE_ROLE KEY
const char* SUPABASE_KEY =
  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImplaW9sdmx1anRubWt3cHJwcGR1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODc2NjQwNDksImV4cCI6MjEwMzI0MDA0OX0.I6vbxGR_mdU4p7hTxs9b2ASgBnd_5ltavPwlRuDdXr8";


// =====================================================
// BANHA TEST DEVICE UUID
// =====================================================

const char* DEVICE_ID = "11111111-1111-1111-1111-111111111111";


// =====================================================
// BUILT-IN LED
// =====================================================

#define LED_PIN 2


// =====================================================
// LORA PINS
// =====================================================

#define LORA_SCK 23
#define LORA_MISO 19
#define LORA_MOSI 17

#define LORA_SS 16
#define LORA_RST 14
#define LORA_DIO0 26

#define LORA_BAND 433E6


// =====================================================
// LORA STATUS
// =====================================================

bool loraReady = false;


// =====================================================
// SUPABASE STATUS
// =====================================================

String lastSupabaseStatus = "Waiting";


// =====================================================
// RECORDING SESSION
// =====================================================

bool isRecordingActive = false;

// UUID from Supabase recordings.id
String currentRecordingId = "";

// Local start time for duration calculation
unsigned long recordingStartMillis = 0;

// PACKET number of the last successfully uploaded DATA
// reading for the CURRENT recording. Reset to 0 whenever
// a new recording starts. Used both for duplicate
// detection and missing-packet detection.
unsigned long lastUploadedPacketNumber = 0;


// =====================================================
// RECEIVED DATA
// =====================================================

unsigned long receivedPacketNumber = 0;
float receivedAverageCO2 = 0.0f;
float receivedAverageTemp = 0.0f;
float receivedAverageNoise = 0.0f;


// =====================================================
// SEQUENCE-NUMBER DUPLICATE DETECTION (TRANSPORT LEVEL)
//
// Keeps a short rolling history of SEQ numbers that have
// already been seen/ACKed, so a retransmitted packet
// (Node 1 resending because it missed our ACK) is
// recognized and NOT reprocessed - it just gets another
// ACK sent back.
// =====================================================

#define SEQ_HISTORY_SIZE 20

uint32_t seqHistory[SEQ_HISTORY_SIZE];
int seqHistoryIndex = 0;
bool seqHistoryFull = false;

bool isDuplicateSeq(uint32_t seq) {
  int count = seqHistoryFull ? SEQ_HISTORY_SIZE : seqHistoryIndex;

  for (int i = 0; i < count; i++) {
    if (seqHistory[i] == seq) return true;
  }

  return false;
}

void markSeqProcessed(uint32_t seq) {
  seqHistory[seqHistoryIndex] = seq;
  seqHistoryIndex = (seqHistoryIndex + 1) % SEQ_HISTORY_SIZE;

  if (seqHistoryIndex == 0) seqHistoryFull = true;
}


// =====================================================
// START ACCESS POINT
// =====================================================

void startAccessPoint() {
  Serial.println();
  Serial.println("Starting BANHA WiFi Setup AP...");

  WiFi.mode(WIFI_AP_STA);

  bool started = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (started) {
    Serial.println();
    Serial.println("================================");
    Serial.println("BANHA SETUP WIFI READY");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Password: ");
    Serial.println(AP_PASSWORD);
    Serial.print("Setup URL: http://");
    Serial.println(WiFi.softAPIP());
    Serial.println("================================");
  } else {
    Serial.println("Failed to start BANHA AP!");
  }
}


// =====================================================
// LOAD SAVED WIFI
// =====================================================

void loadSavedWiFi() {
  preferences.begin("banha-wifi", true);

  savedWiFiSSID = preferences.getString("ssid", "");
  savedWiFiPassword = preferences.getString("password", "");

  preferences.end();

  Serial.println();
  Serial.println("Loading saved WiFi...");

  if (savedWiFiSSID.length() == 0) {
    Serial.println("No saved WiFi configuration.");
    return;
  }

  Serial.print("Saved SSID: ");
  Serial.println(savedWiFiSSID);
}


// =====================================================
// SAVE WIFI
// =====================================================

void saveWiFi(String ssid, String password) {
  preferences.begin("banha-wifi", false);

  preferences.putString("ssid", ssid);
  preferences.putString("password", password);

  preferences.end();

  savedWiFiSSID = ssid;
  savedWiFiPassword = password;

  Serial.println("WiFi credentials saved.");
}


// =====================================================
// START WIFI CONNECTION (NON-BLOCKING)
// =====================================================

void startWiFiConnection() {
  if (savedWiFiSSID.length() == 0) {
    Serial.println("No WiFi configured.");
    wifiConnected = false;
    wifiConnectionAttempting = false;
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiConnectionAttempting = false;
    return;
  }

  Serial.println();
  Serial.println("Starting WiFi connection...");
  Serial.print("SSID: ");
  Serial.println(savedWiFiSSID);

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(savedWiFiSSID.c_str(), savedWiFiPassword.c_str());

  wifiConnectionAttempting = true;
  wifiConnectStartMillis = millis();
  lastWiFiReconnectAttempt = millis();
}


// =====================================================
// MAINTAIN WIFI CONNECTION (NON-BLOCKING)
// =====================================================

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      Serial.println();
      Serial.println("WiFi connected!");
      Serial.print("Router IP: ");
      Serial.println(WiFi.localIP());
    }

    wifiConnected = true;
    wifiConnectionAttempting = false;
    return;
  }

  wifiConnected = false;

  if (wifiConnectionAttempting) {
    if (millis() - wifiConnectStartMillis < WIFI_CONNECT_TIMEOUT) {
      return;
    }

    Serial.println();
    Serial.println("WiFi connection attempt timed out.");
    Serial.println("BANHA-SETUP remains available.");

    WiFi.disconnect(false, false);

    wifiConnectionAttempting = false;
    lastWiFiReconnectAttempt = millis();

    return;
  }

  if (savedWiFiSSID.length() == 0) {
    return;
  }

  if (millis() - lastWiFiReconnectAttempt < WIFI_RECONNECT_INTERVAL) {
    return;
  }

  Serial.println();
  Serial.println("Retrying configured WiFi...");

  startWiFiConnection();
}


// =====================================================
// ENSURE WIFI FOR SUPABASE (NON-BLOCKING)
// =====================================================

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return true;
  }

  wifiConnected = false;

  if (!wifiConnectionAttempting) {
    if (savedWiFiSSID.length() > 0 && millis() - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL) {
      startWiFiConnection();
    }
  }

  return false;
}


// =====================================================
// SHARED HTML HEAD / STYLE
// =====================================================

String getSharedStyle() {
  String css =
    "<style>"
    ":root{"
    "--bg:#f1f5f9;"
    "--card:#ffffff;"
    "--text:#1e293b;"
    "--muted:#64748b;"
    "--border:#e2e8f0;"
    "--primary:#15803d;"
    "--primary-dark:#0f5c2c;"
    "--danger:#dc2626;"
    "--warn:#d97706;"
    "--radius:16px;"
    "}"
    "*{box-sizing:border-box;}"
    "body{"
    "margin:0;"
    "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;"
    "background:var(--bg);"
    "color:var(--text);"
    "-webkit-font-smoothing:antialiased;"
    "}"
    ".container{"
    "max-width:640px;"
    "margin:0 auto;"
    "padding:24px 18px 40px;"
    "}"
    ".topbar{"
    "text-align:center;"
    "padding:8px 0 22px;"
    "}"
    ".topbar .logo{"
    "display:inline-flex;"
    "align-items:center;"
    "justify-content:center;"
    "width:52px;"
    "height:52px;"
    "border-radius:14px;"
    "background:linear-gradient(135deg,#22c55e,var(--primary-dark));"
    "color:white;"
    "font-size:22px;"
    "font-weight:800;"
    "margin-bottom:10px;"
    "box-shadow:0 6px 16px rgba(21,128,61,.25);"
    "}"
    ".topbar h1{"
    "margin:0;"
    "font-size:20px;"
    "letter-spacing:.2px;"
    "}"
    ".topbar p{"
    "margin:4px 0 0;"
    "font-size:13px;"
    "color:var(--muted);"
    "}"
    ".card{"
    "background:var(--card);"
    "padding:22px;"
    "border-radius:var(--radius);"
    "margin-bottom:18px;"
    "border:1px solid var(--border);"
    "box-shadow:0 1px 3px rgba(0,0,0,.04);"
    "}"
    ".card.center{"
    "text-align:center;"
    "padding:40px 24px;"
    "}"
    "h2{"
    "font-size:15px;"
    "text-transform:uppercase;"
    "letter-spacing:.06em;"
    "color:var(--muted);"
    "margin:0 0 14px;"
    "font-weight:700;"
    "}"
    ".status-row{"
    "display:flex;"
    "align-items:center;"
    "justify-content:space-between;"
    "gap:10px;"
    "padding:11px 0;"
    "border-bottom:1px solid var(--border);"
    "}"
    ".status-row:last-child{"
    "border-bottom:none;"
    "}"
    ".status-label{"
    "font-size:13px;"
    "color:var(--muted);"
    "font-weight:600;"
    "}"
    ".status-value{"
    "font-size:14px;"
    "font-weight:600;"
    "text-align:right;"
    "word-break:break-word;"
    "max-width:60%;"
    "}"
    ".badge{"
    "display:inline-flex;"
    "align-items:center;"
    "gap:6px;"
    "font-size:13px;"
    "font-weight:700;"
    "padding:4px 10px;"
    "border-radius:999px;"
    "}"
    ".badge.ok{"
    "background:#dcfce7;"
    "color:#15803d;"
    "}"
    ".badge.bad{"
    "background:#fee2e2;"
    "color:#dc2626;"
    "}"
    ".badge.warn{"
    "background:#fef3c7;"
    "color:#b45309;"
    "}"
    ".badge .dot{"
    "width:7px;"
    "height:7px;"
    "border-radius:50%;"
    "background:currentColor;"
    "}"
    "label{"
    "display:block;"
    "margin-top:16px;"
    "font-weight:700;"
    "font-size:13px;"
    "color:var(--muted);"
    "}"
    "input{"
    "width:100%;"
    "padding:13px 14px;"
    "margin-top:7px;"
    "border:1.5px solid var(--border);"
    "border-radius:10px;"
    "font-size:16px;"
    "background:#f8fafc;"
    "transition:border-color .15s;"
    "}"
    "input:focus{"
    "outline:none;"
    "border-color:var(--primary);"
    "background:white;"
    "}"
    ".password-wrap{"
    "position:relative;"
    "margin-top:7px;"
    "}"
    ".password-wrap input{"
    "margin-top:0;"
    "padding-right:46px;"
    "}"
    ".toggle-password{"
    "position:absolute;"
    "top:0;"
    "right:2px;"
    "height:100%;"
    "width:42px;"
    "display:flex;"
    "align-items:center;"
    "justify-content:center;"
    "cursor:pointer;"
    "color:var(--muted);"
    "user-select:none;"
    "}"
    ".toggle-password svg{"
    "width:20px;"
    "height:20px;"
    "pointer-events:none;"
    "}"
    "button,.button{"
    "display:block;"
    "width:100%;"
    "padding:14px;"
    "margin-top:22px;"
    "background:var(--primary);"
    "color:white;"
    "border:none;"
    "border-radius:10px;"
    "font-size:16px;"
    "font-weight:700;"
    "cursor:pointer;"
    "text-align:center;"
    "text-decoration:none;"
    "box-sizing:border-box;"
    "transition:background .15s;"
    "}"
    "button:active,.button:active{"
    "background:var(--primary-dark);"
    "}"
    ".small{"
    "font-size:12.5px;"
    "color:var(--muted);"
    "line-height:1.5;"
    "}"
    ".steps p{"
    "margin:10px 0;"
    "font-size:14px;"
    "}"
    ".steps b{"
    "color:var(--text);"
    "}"
    ".spinner{"
    "width:56px;"
    "height:56px;"
    "border:5px solid var(--border);"
    "border-top-color:var(--primary);"
    "border-radius:50%;"
    "margin:0 auto 22px;"
    "animation:spin .8s linear infinite;"
    "}"
    "@keyframes spin{"
    "to{transform:rotate(360deg);}"
    "}"
    ".icon-circle{"
    "width:64px;"
    "height:64px;"
    "border-radius:50%;"
    "margin:0 auto 22px;"
    "display:flex;"
    "align-items:center;"
    "justify-content:center;"
    "}"
    ".icon-circle.success{"
    "background:#dcfce7;"
    "}"
    ".icon-circle.fail{"
    "background:#fee2e2;"
    "}"
    ".icon-circle svg{"
    "width:32px;"
    "height:32px;"
    "}"
    ".card.center h2{"
    "text-transform:none;"
    "letter-spacing:0;"
    "font-size:20px;"
    "color:var(--text);"
    "margin:0 0 8px;"
    "}"
    ".card.center p{"
    "margin:0 0 4px;"
    "font-size:14px;"
    "color:var(--muted);"
    "}"
    "</style>";

  return css;
}


// =====================================================
// HTML PAGE : DASHBOARD
// =====================================================

String getDashboardHTML() {
  bool routerConnected = (WiFi.status() == WL_CONNECTED);

  String wifiBadge;
  if (routerConnected) {
    wifiBadge = "<span class='badge ok'><span class='dot'></span>Connected</span>";
  } else if (wifiConnectionAttempting) {
    wifiBadge = "<span class='badge warn'><span class='dot'></span>Connecting...</span>";
  } else {
    wifiBadge = "<span class='badge bad'><span class='dot'></span>Disconnected</span>";
  }

  String loraBadge =
    loraReady
      ? "<span class='badge ok'><span class='dot'></span>Ready</span>"
      : "<span class='badge bad'><span class='dot'></span>Not Ready</span>";

  String recordingBadge =
    isRecordingActive
      ? "<span class='badge warn'><span class='dot'></span>Recording</span>"
      : "<span class='badge ok'><span class='dot'></span>Idle</span>";

  String supabaseBadgeClass = "ok";

  if (lastSupabaseStatus.indexOf("failed") != -1 || lastSupabaseStatus.indexOf("unavailable") != -1 || lastSupabaseStatus.indexOf("not found") != -1) {
    supabaseBadgeClass = "bad";
  } else if (lastSupabaseStatus == "Waiting" || lastSupabaseStatus.indexOf("...") != -1 || lastSupabaseStatus.indexOf("Creating") != -1 || lastSupabaseStatus.indexOf("Uploading") != -1 || lastSupabaseStatus.indexOf("Stopping") != -1) {
    supabaseBadgeClass = "warn";
  }

  String supabaseBadge =
    "<span class='badge " + supabaseBadgeClass + "'><span class='dot'></span>" + lastSupabaseStatus + "</span>";

  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='UTF-8'>"
    "<title>BANHA Node 2</title>"
    + getSharedStyle() + "</head>"
                         "<body>"
                         "<div class='container'>"
                         "<div class='topbar'>"
                         "<div class='logo'>B2</div>"
                         "<h1>BANHA Node 2</h1>"
                         "<p>LoRa Receiver &amp; WiFi Configuration</p>"
                         "</div>"

                         "<div class='card'>"
                         "<h2>System Status</h2>"
                         "<div class='status-row'>"
                         "<span class='status-label'>Router WiFi</span>"
                         "<span class='status-value'>"
    + wifiBadge + "</span>"
                  "</div>"
                  "<div class='status-row'>"
                  "<span class='status-label'>Configured Network</span>"
                  "<span class='status-value'>"
    + (savedWiFiSSID.length() > 0 ? savedWiFiSSID : String("None")) + "</span>"
                                                                      "</div>"
                                                                      "<div class='status-row'>"
                                                                      "<span class='status-label'>Connected SSID</span>"
                                                                      "<span class='status-value'>"
    + (routerConnected ? WiFi.SSID() : String("&mdash;")) + "</span>"
                                                            "</div>"
                                                            "<div class='status-row'>"
                                                            "<span class='status-label'>Router IP</span>"
                                                            "<span class='status-value'>"
    + (routerConnected ? WiFi.localIP().toString() : String("&mdash;")) + "</span>"
                                                                          "</div>"
                                                                          "<div class='status-row'>"
                                                                          "<span class='status-label'>Setup Hotspot</span>"
                                                                          "<span class='status-value'>"
    + String(AP_SSID) + " &middot; " + WiFi.softAPIP().toString() + "</span>"
                                                                    "</div>"
                                                                    "<div class='status-row'>"
                                                                    "<span class='status-label'>LoRa Radio</span>"
                                                                    "<span class='status-value'>"
    + loraBadge + "</span>"
                  "</div>"
                  "<div class='status-row'>"
                  "<span class='status-label'>Recording</span>"
                  "<span class='status-value'>"
    + recordingBadge + "</span>"
                       "</div>"
                       "<div class='status-row'>"
                       "<span class='status-label'>Supabase</span>"
                       "<span class='status-value'>"
    + supabaseBadge + "</span>"
                      "</div>"
                      "</div>"

                      "<div class='card'>"
                      "<h2>Change WiFi Configuration</h2>"
                      "<form id='wifiForm' action='/save' method='POST' onsubmit=\"return confirm('Save and connect to this WiFi network?');\">"
                      "<label>WiFi Name (SSID)</label>"
                      "<input type='text' name='ssid' autocomplete='off' value='"
    + savedWiFiSSID + "' required>"
                      "<label>WiFi Password</label>"
                      "<div class='password-wrap'>"
                      "<input type='password' id='wifiPassword' name='password' autocomplete='new-password' placeholder='Enter WiFi password'>"
                      "<span class='toggle-password' id='togglePasswordBtn' onclick='togglePassword()' role='button' tabindex='0' aria-label='Show password'>"
                      "<svg id='eyeIcon' viewBox='0 0 24 24' fill='none' xmlns='http://www.w3.org/2000/svg'>"
                      "<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/>"
                      "<circle cx='12' cy='12' r='3' stroke='currentColor' stroke-width='2'/>"
                      "</svg>"
                      "</span>"
                      "</div>"
                      "<button type='submit'>Save and Connect</button>"
                      "</form>"
                      "</div>"

                      "<div class='card steps'>"
                      "<h2>How to Access Setup</h2>"
                      "<p>1. Connect your phone or desktop to <b>BANHA-SETUP</b>.</p>"
                      "<p>2. Password: <b>banha@nbsc2026</b></p>"
                      "<p>3. Open <b>192.168.4.1</b> in your browser.</p>"
                      "<p class='small'>"
                      "Stay connected with BANHA&mdash;because every breath, sound, "
                      "and degree matters in creating a better learning environment."
                      "</p>"
                      "</div>"

                      "</div>"

                      "<script>"
                      "function togglePassword(){"
                      "var inp=document.getElementById('wifiPassword');"
                      "var icon=document.getElementById('eyeIcon');"
                      "var btn=document.getElementById('togglePasswordBtn');"
                      "if(inp.type==='password'){"
                      "inp.type='text';"
                      "icon.innerHTML=\"<path d='M17.94 17.94A10.94 10.94 0 0 1 12 20c-7 0-11-8-11-8a21.62 21.62 0 0 1 5.06-6.06M9.9 4.24A10.94 10.94 0 0 1 12 4c7 0 11 8 11 8a21.6 21.6 0 0 1-2.16 3.19M1 1l22 22' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/>\";"
                      "btn.setAttribute('aria-label','Hide password');"
                      "}else{"
                      "inp.type='password';"
                      "icon.innerHTML=\"<path d='M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/><circle cx='12' cy='12' r='3' stroke='currentColor' stroke-width='2'/>\";"
                      "btn.setAttribute('aria-label','Show password');"
                      "}"
                      "}"
                      "document.getElementById('togglePasswordBtn').addEventListener('keydown',function(e){"
                      "if(e.key==='Enter'||e.key===' '){e.preventDefault();togglePassword();}"
                      "});"
                      "</script>"

                      "</body>"
                      "</html>";

  return html;
}


// =====================================================
// HTML PAGE : CONNECTING / RESULT SCREEN
// =====================================================

String getConnectingHTML(String ssid) {
  String html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='UTF-8'>"
    "<title>Connecting - BANHA Node 2</title>"
    + getSharedStyle() + "</head>"
                         "<body>"
                         "<div class='container'>"
                         "<div class='topbar'>"
                         "<div class='logo'>B2</div>"
                         "<h1>BANHA Node 2</h1>"
                         "</div>"

                         "<div class='card center'>"
                         "<div id='spinner' class='spinner'></div>"
                         "<div id='iconSuccess' class='icon-circle success' style='display:none'>"
                         "<svg viewBox='0 0 24 24' fill='none' xmlns='http://www.w3.org/2000/svg'>"
                         "<path d='M5 13l4 4L19 7' stroke='#15803d' stroke-width='3' stroke-linecap='round' stroke-linejoin='round'/>"
                         "</svg>"
                         "</div>"
                         "<div id='iconFail' class='icon-circle fail' style='display:none'>"
                         "<svg viewBox='0 0 24 24' fill='none' xmlns='http://www.w3.org/2000/svg'>"
                         "<path d='M6 6l12 12M18 6L6 18' stroke='#dc2626' stroke-width='3' stroke-linecap='round' stroke-linejoin='round'/>"
                         "</svg>"
                         "</div>"
                         "<h2 id='title'>Connecting to WiFi...</h2>"
                         "<p id='subtitle'>Attempting to join <b>"
    + ssid + "</b></p>"
             "<p class='small' id='detail'>This can take up to 15 seconds.</p>"
             "<a href='/' id='backlink' class='button' style='display:none'>Return to Dashboard</a>"
             "</div>"

             "<p class='small' style='text-align:center'>"
             "BANHA-SETUP hotspot remains available the whole time."
             "</p>"

             "</div>"

             "<script>"
             "var targetSSID=\""
    + ssid + "\";"
             "var attempts=0;"
             "var maxAttempts=22;"

             "function showSuccess(data){"
             "document.getElementById('spinner').style.display='none';"
             "document.getElementById('iconSuccess').style.display='flex';"
             "document.getElementById('title').textContent='Connected!';"
             "document.getElementById('subtitle').innerHTML='Joined <b>'+data.ssid+'</b>';"
             "document.getElementById('detail').textContent='IP address: '+data.ip+' \\u2014 redirecting to dashboard...';"
             "document.getElementById('backlink').style.display='block';"
             "document.getElementById('backlink').textContent='Return to Dashboard';"
             "setTimeout(function(){window.location.href='/';},3000);"
             "}"

             "function showFail(){"
             "document.getElementById('spinner').style.display='none';"
             "document.getElementById('iconFail').style.display='flex';"
             "document.getElementById('title').textContent='Connection Failed';"
             "document.getElementById('subtitle').innerHTML='Could not connect to <b>'+targetSSID+'</b>';"
             "document.getElementById('detail').textContent='Check the WiFi name and password, then try again. BANHA-SETUP remains available.';"
             "document.getElementById('backlink').style.display='block';"
             "document.getElementById('backlink').textContent='Try Again';"
             "}"

             "function poll(){"
             "fetch('/status').then(function(r){return r.json();}).then(function(data){"
             "if(data.wifi_connected){showSuccess(data);return;}"
             "if(!data.wifi_connecting){showFail();return;}"
             "attempts++;"
             "if(attempts>maxAttempts){showFail();return;}"
             "setTimeout(poll,1000);"
             "}).catch(function(){"
             "attempts++;"
             "if(attempts>maxAttempts){showFail();return;}"
             "setTimeout(poll,1000);"
             "});"
             "}"

             "setTimeout(poll,1200);"
             "</script>"

             "</body>"
             "</html>";

  return html;
}


// =====================================================
// WEB: HOME
// =====================================================

void handleRoot() {
  server.send(200, "text/html", getDashboardHTML());
}


// =====================================================
// WEB: SAVE WIFI
// =====================================================

void handleSaveWiFi() {
  if (!server.hasArg("ssid")) {
    server.send(400, "text/plain", "SSID is required.");
    return;
  }

  String newSSID = server.arg("ssid");
  String newPassword = server.arg("password");

  newSSID.trim();

  if (newSSID.length() == 0) {
    server.send(400, "text/plain", "SSID cannot be empty.");
    return;
  }

  saveWiFi(newSSID, newPassword);

  Serial.println();
  Serial.println("New WiFi configuration received from dashboard.");
  Serial.print("SSID: ");
  Serial.println(newSSID);

  server.send(200, "text/html", getConnectingHTML(newSSID));

  wifiConnectionAttempting = false;

  WiFi.disconnect(false, false);

  lastWiFiReconnectAttempt = 0;

  startWiFiConnection();
}


// =====================================================
// WEB: STATUS API
// =====================================================

void handleStatus() {
  String json = "{";

  json += "\"wifi_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"wifi_connecting\":" + String(wifiConnectionAttempting ? "true" : "false");
  json += ",\"configured_ssid\":\"" + savedWiFiSSID + "\"";
  json += ",\"ssid\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("")) + "\"";
  json += ",\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("")) + "\"";
  json += ",\"ap_ssid\":\"" + String(AP_SSID) + "\"";
  json += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
  json += ",\"lora_ready\":" + String(loraReady ? "true" : "false");
  json += ",\"recording_active\":" + String(isRecordingActive ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
}


// =====================================================
// START WEB SERVER
// =====================================================

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSaveWiFi);
  server.on("/status", HTTP_GET, handleStatus);

  server.begin();

  Serial.println("Web server started.");
}


// =====================================================
// GET VALUE FROM LORA PACKET
// =====================================================

String getPacketValue(String message, String key) {
  int startIndex = message.indexOf(key);
  if (startIndex == -1) return "";

  startIndex += key.length();

  int endIndex = message.indexOf(",", startIndex);
  if (endIndex == -1) endIndex = message.length();

  return message.substring(startIndex, endIndex);
}


// =====================================================
// SEND ACK BACK TO NODE 1
// =====================================================

void sendAck(uint32_t seq, String ackType) {
  String message = "NODE:2,TYPE:ACK,SEQ:" + String(seq) + ",ACKTYPE:" + ackType;

  Serial.print("[ACK] Sending: ");
  Serial.println(message);

  LoRa.beginPacket();
  LoRa.print(message);
  LoRa.endPacket();

  // Go back to listening for the next packet from Node 1.
  LoRa.receive();
}


// =====================================================
// EXTRACT RECORDING UUID
// =====================================================

String extractRecordingId(String response) {
  int idKey = response.indexOf("\"id\":\"");
  if (idKey == -1) return "";

  idKey += 6;

  int endQuote = response.indexOf("\"", idKey);
  if (endQuote == -1) return "";

  return response.substring(idKey, endQuote);
}


// =====================================================
// CREATE RECORDING IN SUPABASE
// =====================================================

bool createRecordingInDatabase() {
  if (!ensureWiFi()) {
    Serial.println("Cannot create recording: WiFi unavailable.");
    lastSupabaseStatus = "WiFi unavailable";
    return false;
  }

  Serial.println();
  Serial.println("SUPABASE: CREATING RECORDING...");

  lastSupabaseStatus = "Creating recording";

  String endpoint = String(SUPABASE_URL) + "/rest/v1/recordings";

  HTTPClient http;
  http.begin(endpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=representation");

  String payload = "{";
  payload += "\"device_id\":\"";
  payload += DEVICE_ID;
  payload += "\",";
  payload += "\"status\":\"recording\"";
  payload += "}";

  Serial.println("POST:");
  Serial.println(endpoint);
  Serial.println("Payload:");
  Serial.println(payload);

  int httpCode = http.POST(payload);
  String response = http.getString();

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);
  Serial.print("Response: ");
  Serial.println(response);

  http.end();

  if (httpCode < 200 || httpCode >= 300) {
    lastSupabaseStatus = "Create failed: HTTP " + String(httpCode);
    return false;
  }

  currentRecordingId = extractRecordingId(response);

  if (currentRecordingId.length() == 0) {
    lastSupabaseStatus = "UUID not found";
    return false;
  }

  lastSupabaseStatus = "Recording created";

  Serial.print("Recording UUID: ");
  Serial.println(currentRecordingId);

  return true;
}


// =====================================================
// UPLOAD ENVIRONMENTAL DATA
// =====================================================

bool uploadEnvironmentalData() {
  if (!isRecordingActive) {
    Serial.println("Upload rejected: Recording inactive.");
    return false;
  }

  if (currentRecordingId.length() == 0) {
    Serial.println("Upload rejected: Recording UUID missing.");
    return false;
  }

  if (!ensureWiFi()) {
    Serial.println("Upload failed: WiFi unavailable.");
    lastSupabaseStatus = "Upload failed: WiFi unavailable";
    return false;
  }

  lastSupabaseStatus = "Uploading reading";

  String endpoint = String(SUPABASE_URL) + "/rest/v1/environmental_readings";

  HTTPClient http;
  http.begin(endpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  String payload = "{";
  payload += "\"recording_id\":\"";
  payload += currentRecordingId;
  payload += "\",";
  payload += "\"packet_number\":" + String(receivedPacketNumber) + ",";
  payload += "\"average_co2\":" + String(receivedAverageCO2, 2) + ",";
  payload += "\"average_temperature\":" + String(receivedAverageTemp, 2) + ",";
  payload += "\"average_noise\":" + String(receivedAverageNoise, 2);
  payload += "}";

  Serial.println();
  Serial.println("SUPABASE: UPLOADING READING...");
  Serial.println(payload);

  int httpCode = http.POST(payload);
  String response = http.getString();

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);
  Serial.print("Response: ");
  Serial.println(response);

  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    lastSupabaseStatus = "Reading uploaded";
    Serial.println("SUPABASE: READING UPLOADED!");
    return true;
  }

  lastSupabaseStatus = "Upload failed: HTTP " + String(httpCode);
  Serial.println("SUPABASE: READING UPLOAD FAILED!");

  return false;
}


// =====================================================
// STOP RECORDING IN SUPABASE
// =====================================================

bool stopRecordingInDatabase() {
  if (currentRecordingId.length() == 0) {
    Serial.println("No recording UUID available.");
    return false;
  }

  if (!ensureWiFi()) {
    lastSupabaseStatus = "Stop failed: WiFi unavailable";
    return false;
  }

  lastSupabaseStatus = "Stopping recording";

  unsigned long elapsedMilliseconds = millis() - recordingStartMillis;
  int durationSeconds = elapsedMilliseconds / 1000;

  Serial.print("Duration seconds: ");
  Serial.println(durationSeconds);

  String endpoint = String(SUPABASE_URL) + "/rest/v1/recordings?id=eq." + currentRecordingId;

  HTTPClient http;
  http.begin(endpoint);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=representation");

  String payload = "{";
  payload += "\"duration_seconds\":" + String(durationSeconds) + ",";
  payload += "\"status\":\"pending_assessment\"";
  payload += "}";

  Serial.println("PATCH:");
  Serial.println(endpoint);
  Serial.println("Payload:");
  Serial.println(payload);

  int httpCode = http.sendRequest("PATCH", payload);
  String response = http.getString();

  Serial.print("HTTP Code: ");
  Serial.println(httpCode);
  Serial.print("Response: ");
  Serial.println(response);

  http.end();

  if (httpCode >= 200 && httpCode < 300) {
    lastSupabaseStatus = "Recording stopped";
    Serial.println("SUPABASE: RECORDING STOPPED!");
    return true;
  }

  lastSupabaseStatus = "Stop failed: HTTP " + String(httpCode);
  Serial.println("SUPABASE: FAILED TO STOP RECORDING!");

  return false;
}


// =====================================================
// HANDLE START COMMAND
// =====================================================

void handleStartCommand(bool isDuplicate) {
  Serial.println();
  Serial.println("***** START COMMAND RECEIVED *****");

  if (isDuplicate) {
    Serial.println("[DUP] Duplicate START (already ACKed once) - not creating a new recording.");
    return;
  }

  if (isRecordingActive) {
    Serial.println("Duplicate START ignored (recording already active locally).");
    return;
  }

  bool created = createRecordingInDatabase();

  if (!created) {
    Serial.println("Database recording creation failed.");
    return;
  }

  isRecordingActive = true;
  recordingStartMillis = millis();

  // Fresh recording - reset packet tracking for
  // duplicate/missing-packet detection.
  lastUploadedPacketNumber = 0;

  Serial.println();
  Serial.println("================================");
  Serial.println("RECORDING SESSION STARTED");
  Serial.print("Recording UUID: ");
  Serial.println(currentRecordingId);
  Serial.println("================================");
}


// =====================================================
// HANDLE STOP COMMAND
// =====================================================

void handleStopCommand(bool isDuplicate) {
  Serial.println();
  Serial.println("***** STOP COMMAND RECEIVED *****");

  if (isDuplicate) {
    Serial.println("[DUP] Duplicate STOP (already ACKed once) - not stopping again.");
    return;
  }

  if (!isRecordingActive) {
    Serial.println("No active recording.");
    return;
  }

  bool stopped = stopRecordingInDatabase();

  if (!stopped) {
    Serial.println("Database stop failed.");
    return;
  }

  isRecordingActive = false;

  Serial.println("RECORDING SESSION STOPPED");

  currentRecordingId = "";
}


// =====================================================
// PARSE DATA PACKET
// =====================================================

bool parseDataPacket(String message) {
  String packetValue = getPacketValue(message, "PACKET:");
  String averageCO2Value = getPacketValue(message, "AVG_CO2:");
  String averageTempValue = getPacketValue(message, "AVG_TEMP:");
  String averageNoiseValue = getPacketValue(message, "AVG_NOISE:");

  if (packetValue.length() == 0 || averageCO2Value.length() == 0 || averageTempValue.length() == 0 || averageNoiseValue.length() == 0) {
    return false;
  }

  receivedPacketNumber = packetValue.toInt();
  receivedAverageCO2 = averageCO2Value.toFloat();
  receivedAverageTemp = averageTempValue.toFloat();
  receivedAverageNoise = averageNoiseValue.toFloat();

  return true;
}


// =====================================================
// HANDLE DATA COMMAND
//
// Handles PACKET-number-based duplicate detection (a
// second safety net beyond the SEQ-based check) and
// missing-packet detection/logging.
// =====================================================

void handleDataCommand(String message, bool isDuplicateSeqPacket) {
  if (!isRecordingActive) {
    Serial.println("DATA ignored: No active recording.");
    return;
  }

  bool valid = parseDataPacket(message);

  if (!valid) {
    Serial.println("Invalid DATA packet.");
    return;
  }

  Serial.print("PACKET: ");
  Serial.println(receivedPacketNumber);
  Serial.print("CO2: ");
  Serial.println(receivedAverageCO2);
  Serial.print("Temperature: ");
  Serial.println(receivedAverageTemp);
  Serial.print("Noise: ");
  Serial.println(receivedAverageNoise);

  // Duplicate check #1: same transport SEQ seen before
  // (already ACKed, Node 1 just didn't see the ACK).
  if (isDuplicateSeqPacket) {
    Serial.println("[DUP] Duplicate SEQ for this DATA packet - skipping upload, ACK already resent.");
    return;
  }

  // Duplicate check #2: PACKET number already uploaded
  // for this recording (extra safety net).
  if (receivedPacketNumber <= lastUploadedPacketNumber) {
    Serial.print("[DUP] PACKET:");
    Serial.print(receivedPacketNumber);
    Serial.println(" already uploaded for this recording - skipping.");
    return;
  }

  // Missing packet detection.
  if (lastUploadedPacketNumber > 0 && receivedPacketNumber > lastUploadedPacketNumber + 1) {
    Serial.print("[MISSING] Packet(s) ");
    Serial.print(lastUploadedPacketNumber + 1);

    if (receivedPacketNumber - 1 > lastUploadedPacketNumber + 1) {
      Serial.print(" through ");
      Serial.print(receivedPacketNumber - 1);
    }

    Serial.println(" never arrived.");
  }

  Serial.println("VALID DATA RECEIVED - uploading.");

  bool uploaded = uploadEnvironmentalData();

  if (uploaded) {
    lastUploadedPacketNumber = receivedPacketNumber;
  }
}


// =====================================================
// HANDLE LORA MESSAGE
// =====================================================

void handleLoRaMessage(String message) {
  if (!message.startsWith("NODE:1,")) {
    Serial.println("Rejected: Not from Node 1.");
    return;
  }

  String packetType = getPacketValue(message, "TYPE:");

  Serial.print("Packet TYPE: ");
  Serial.println(packetType);

  if (packetType != "START" && packetType != "STOP" && packetType != "DATA") {
    Serial.println("Unknown packet TYPE - no ACK sent.");
    return;
  }

  String seqStr = getPacketValue(message, "SEQ:");

  if (seqStr.length() == 0) {
    // No SEQ present (e.g. an older/incompatible sender).
    // We can't ACK or dedupe it - process it best-effort,
    // the way the system used to behave.
    Serial.println("[WARN] Packet has no SEQ field - processing without ACK/dedupe.");

    if (packetType == "START") {
      handleStartCommand(false);
    } else if (packetType == "STOP") {
      handleStopCommand(false);
    } else {
      handleDataCommand(message, false);
    }

    return;
  }

  uint32_t seq = (uint32_t)strtoul(seqStr.c_str(), NULL, 10);

  bool duplicate = isDuplicateSeq(seq);

  // ACK immediately - this is a transport-layer
  // acknowledgment that the radio packet arrived, sent
  // BEFORE any (potentially slow) Supabase work, and sent
  // even for duplicates in case our earlier ACK was lost.
  sendAck(seq, packetType);

  if (!duplicate) {
    markSeqProcessed(seq);
  }

  if (packetType == "START") {
    handleStartCommand(duplicate);
  } else if (packetType == "STOP") {
    handleStopCommand(duplicate);
  } else {
    handleDataCommand(message, duplicate);
  }
}


// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println();
  Serial.println("================================");
  Serial.println("BANHA NODE 2");
  Serial.println("LORA + SUPABASE + WIFI SETUP");
  Serial.println("================================");

  startAccessPoint();
  loadSavedWiFi();
  startWebServer();
  startWiFiConnection();

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

  Serial.println("Starting LoRa...");

  if (!LoRa.begin(LORA_BAND)) {
    Serial.println("LoRa FAILED!");
    loraReady = false;
  } else {
    loraReady = true;
    Serial.println("LoRa Ready!");
    LoRa.receive();
  }

  Serial.println();
  Serial.println("================================");
  Serial.println("NODE 2 READY");
  Serial.print("Always available setup WiFi: ");
  Serial.println(AP_SSID);
  Serial.print("Setup address: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("================================");
}


// =====================================================
// LOOP
// =====================================================

void loop() {
  // Highest priority: keep the web dashboard responsive.
  server.handleClient();

  // Maintain WiFi in the background (non-blocking).
  maintainWiFiConnection();

  if (!loraReady) {
    delay(5);
    return;
  }

  int packetSize = LoRa.parsePacket();

  if (packetSize <= 0) {
    delay(5);
    return;
  }

  String receivedMessage = "";

  while (LoRa.available()) {
    receivedMessage += (char)LoRa.read();
  }

  Serial.println();
  Serial.println("================================");
  Serial.print("LoRa: ");
  Serial.println(receivedMessage);

  handleLoRaMessage(receivedMessage);

  Serial.print("RSSI: ");
  Serial.println(LoRa.packetRssi());
  Serial.println("================================");

  digitalWrite(LED_PIN, HIGH);
  delay(100);
  digitalWrite(LED_PIN, LOW);
}
