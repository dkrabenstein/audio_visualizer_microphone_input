// wifi_manager.cpp -- see wifi_manager.h

#include "wifi_manager.h"

#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "secrets.h"

// Password of the setup access point. WPA2 needs at least 8 characters. Override it in secrets.h.
// The AP only exists while the board has no working network, but anyone in range who knows this can
// join it and point the board at another network, so pick your own if the board will be handed out.
#ifndef AP_PASSWORD
#define AP_PASSWORD "visualizer"
#endif

static const char     *NVS_NAMESPACE      = "wifi";
static const uint32_t  CONNECT_TIMEOUT_MS = 20000;   // one attempt at joining a network
static const uint32_t  AP_RETRY_MS        = 30000;   // while the AP is up, retry the saved network this often
static const uint32_t  TRY_ARM_MS         = 500;     // ignore the old connection right after a switch

struct Creds {
    char ssid[33];   // 32 characters max in 802.11, plus the terminator
    char pass[65];   // 63 characters max for WPA2, plus the terminator
};

enum WmState : uint8_t {
    WM_CONNECTING,   // joining the saved network (boot, or after a failed trial)
    WM_CONNECTED,    // on the saved network
    WM_AP,           // setup AP is up; the saved network (if any) is still retried in the background
    WM_TRYING        // the user submitted a new network; testing it before it is saved
};

static const char *WM_NAMES[] = { "connecting", "connected", "ap", "trying" };

static volatile WmState g_state = WM_CONNECTING;
static Creds     g_cur = {};            // the saved network ("" ssid = none)
static Creds     g_try = {};            // the network being tested
static bool      g_apUp = false;
static char      g_apSsid[24] = "";
static char      g_error[96] = "";      // why the last attempt failed, shown in the web UI
static const char *g_host = "visualizer";
static uint32_t  g_deadlineMs = 0;
static uint32_t  g_retryMs = 0;
static uint32_t  g_armMs = 0;
static DNSServer g_dns;

// A request from the web handler (AsyncTCP task) to the network task. The handler fills g_pending
// and raises the flag; the loop copies it out. The spinlock keeps the two from seeing half a write.
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static Creds        g_pending = {};
static volatile bool g_pendingFlag = false;
static volatile bool g_forgetFlag = false;
// When the request arrived. Starting a trial (or forgetting) drops the board off the network it is
// on, so doing it the instant the request lands can beat the HTTP reply out of the door and the
// client never hears "ok". The loop waits this long first, so the reply is already on its way.
static volatile uint32_t g_requestMs = 0;
static const uint32_t REPLY_GRACE_MS = 400;

// ---------------------------------------------------------------- Persistence (NVS)
// Namespace "wifi": "ssid", "pass", and "seeded". The flag records that the compile-time network
// from secrets.h (if it has one) was already copied in once, so "forget" stays forgotten.
static void loadCreds() {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return;
    if (!p.isKey("ssid") && !p.getBool("seeded", false)) {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
        // One-time migration for boards flashed before this feature: keep the old network.
        if (strlen(WIFI_SSID) > 0) {
            p.putString("ssid", WIFI_SSID);
            p.putString("pass", WIFI_PASSWORD);
        }
#endif
        p.putBool("seeded", true);
    }
    String s = p.getString("ssid", "");
    String w = p.getString("pass", "");
    p.end();
    strlcpy(g_cur.ssid, s.c_str(), sizeof(g_cur.ssid));
    strlcpy(g_cur.pass, w.c_str(), sizeof(g_cur.pass));
}

static bool saveCreds(const Creds &c) {
    Preferences p;
    if (!p.begin(NVS_NAMESPACE, false)) return false;
    const bool ok = p.putString("ssid", c.ssid) > 0 && (c.pass[0] == 0 || p.putString("pass", c.pass) > 0);
    if (c.pass[0] == 0) p.remove("pass");   // an open network: don't keep an old password around
    p.end();
    return ok;
}

static void eraseCreds() {
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) { p.remove("ssid"); p.remove("pass"); p.end(); }
}

// ---------------------------------------------------------------- Radio helpers
static void startSta(const Creds &c) {
    WiFi.mode(g_apUp ? WIFI_AP_STA : WIFI_STA);
    WiFi.setSleep(false);          // streaming audio and OTA need the radio awake
    WiFi.setAutoReconnect(true);
    WiFi.begin(c.ssid, c.pass);
}

static void startAp() {
    if (g_apUp) return;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(g_apSsid, sizeof(g_apSsid), "Visualizer-%02X%02X", mac[4], mac[5]);
    WiFi.mode(WIFI_AP_STA);        // keep the STA half alive so the saved network can still be retried
    // Channel 1, visible, at most 3 clients: this is a setup tool, not a hotspot.
    WiFi.softAP(g_apSsid, AP_PASSWORD, 1, 0, 3);
    // Answer every DNS lookup with our own address, so a phone's "is there internet?" probe lands
    // on the board and it offers the setup page.
    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dns.start(53, "*", WiFi.softAPIP());
    g_apUp = true;
    Serial.printf("Setup AP: join \"%s\" (password \"%s\"), then open http://%s/\n",
                  g_apSsid, AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
    if (!g_apUp) return;
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_apUp = false;
    Serial.println("Setup AP closed");
}

// Join the saved network, or open the AP if there isn't one.
static void connectSaved() {
    if (g_cur.ssid[0]) {
        Serial.printf("Connecting to Wi-Fi \"%s\"\n", g_cur.ssid);
        startSta(g_cur);
        g_state = WM_CONNECTING;
        g_deadlineMs = millis() + CONNECT_TIMEOUT_MS;
    } else {
        Serial.println("No Wi-Fi network saved");
        startAp();
        g_state = WM_AP;
        g_retryMs = millis() + AP_RETRY_MS;
    }
}

// ---- Scanning
// The HTTP handler only asks for a scan and reads the cached answer; the scan itself runs in the
// network task (wifi_manager_loop). An async scan started from the web server's task kept coming
// back empty on the board while it was joined to a network (15+ networks in range, 0 reported),
// whereas a blocking scan from a normal task works. It blocks the network task for a few seconds;
// that pauses OTA and the UDP/beacon work only, and only when the user presses Scan. Rendering runs
// on the other core.
enum ScanState : uint8_t { SCAN_IDLE, SCAN_REQUESTED, SCAN_RUNNING, SCAN_DONE };
static volatile ScanState g_scanState = SCAN_IDLE;
static String g_scanJson;                     // the cached result while g_scanState == SCAN_DONE

static void runScan() {
    struct Net { String ssid; int rssi; bool secure; };
    static Net nets[20];
    int count = 0;

    // A scan can still come back empty if something interrupts it, so ask a few times before
    // believing there is nothing in range.
    for (int attempt = 0; attempt < 3 && count == 0; attempt++) {
        const int n = WiFi.scanNetworks(false, false);   // blocking; hidden networks left out
        for (int i = 0; i < n; i++) {
            const String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;
            const int rssi = WiFi.RSSI(i);
            const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            // The same name on several access points: keep the strongest.
            int at = -1;
            for (int k = 0; k < count; k++) if (nets[k].ssid == ssid) { at = k; break; }
            if (at >= 0) { if (rssi > nets[at].rssi) { nets[at].rssi = rssi; nets[at].secure = secure; } continue; }
            if (count < 20) nets[count++] = { ssid, rssi, secure };
        }
        WiFi.scanDelete();
    }
    for (int i = 1; i < count; i++) {       // insertion sort, strongest first
        Net t = nets[i];
        int k = i - 1;
        while (k >= 0 && nets[k].rssi < t.rssi) { nets[k + 1] = nets[k]; k--; }
        nets[k + 1] = t;
    }
    JsonDocument d;
    d["scanning"] = false;
    JsonArray a = d["networks"].to<JsonArray>();
    for (int i = 0; i < count; i++) {
        JsonObject o = a.add<JsonObject>();
        o["ssid"] = nets[i].ssid;
        o["rssi"] = nets[i].rssi;
        o["secure"] = nets[i].secure;
    }
    String s;
    serializeJson(d, s);
    g_scanJson = s;
    g_scanState = SCAN_DONE;
    Serial.printf("Wi-Fi scan: %d networks\n", count);
}

// GET /api/wifi/scan -- the first call asks for a scan and answers {"scanning":true}; poll until it
// answers with the list. Scanning briefly takes the radio off the current channel, so it only
// runs when the user asks.
static void handleScan(AsyncWebServerRequest *r) {
    if (g_scanState == SCAN_DONE) {
        g_scanState = SCAN_IDLE;               // the next GET starts a fresh scan
        r->send(200, "application/json", g_scanJson);
        return;
    }
    if (g_scanState == SCAN_IDLE) g_scanState = SCAN_REQUESTED;
    r->send(200, "application/json", "{\"scanning\":true}");
}

// ---------------------------------------------------------------- Public: state machine
void wifi_manager_begin(const char *mdnsHost) {
    if (mdnsHost) g_host = mdnsHost;
    WiFi.persistent(false);        // the credentials live in our own NVS namespace, not the SDK's
    WiFi.mode(WIFI_STA);
    loadCreds();
    connectSaved();
}

bool wifi_manager_connected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifi_manager_loop() {
    const uint32_t now = millis();
    const bool up = wifi_manager_connected();
    if (g_apUp) g_dns.processNextRequest();

    if (g_scanState == SCAN_REQUESTED) {
        g_scanState = SCAN_RUNNING;
        runScan();
        return;
    }

    // "Forget this network": drop the saved credentials and fall back to the setup AP.
    if (g_forgetFlag && (int32_t)(now - g_requestMs) >= (int32_t)REPLY_GRACE_MS) {
        g_forgetFlag = false;
        eraseCreds();
        memset(&g_cur, 0, sizeof(g_cur));
        g_error[0] = 0;
        WiFi.disconnect(false, true);
        startAp();
        g_state = WM_AP;
        g_retryMs = now + AP_RETRY_MS;
        return;
    }

    // A new network from the web UI: test it before replacing the saved one, so a typo can't strand
    // the board.
    if (g_pendingFlag && g_state != WM_TRYING && (int32_t)(now - g_requestMs) >= (int32_t)REPLY_GRACE_MS) {
        portENTER_CRITICAL(&g_mux);
        g_try = g_pending;
        g_pendingFlag = false;
        portEXIT_CRITICAL(&g_mux);
        g_error[0] = 0;
        Serial.printf("Trying Wi-Fi \"%s\"\n", g_try.ssid);
        WiFi.disconnect(false, false);   // leave the old network; keep the SDK's config untouched
        startSta(g_try);
        g_state = WM_TRYING;
        g_deadlineMs = now + CONNECT_TIMEOUT_MS;
        g_armMs = now + TRY_ARM_MS;
        return;
    }

    switch (g_state) {
    case WM_CONNECTING:
        if (up) {
            g_state = WM_CONNECTED;
            stopAp();
        } else if ((int32_t)(now - g_deadlineMs) >= 0) {
            Serial.println("Saved Wi-Fi not reachable, opening the setup AP");
            startAp();
            g_state = WM_AP;
            g_retryMs = now + AP_RETRY_MS;
        }
        break;

    case WM_CONNECTED:
        // A dropped link is rejoined by the SDK's auto-reconnect. If it stays down, offer the AP so
        // the board is never unreachable.
        if (!up) {
            g_state = WM_CONNECTING;
            g_deadlineMs = now + CONNECT_TIMEOUT_MS;
        }
        break;

    case WM_AP:
        if (up && g_cur.ssid[0]) {            // the saved network came back (e.g. the router booted late)
            g_state = WM_CONNECTED;
            stopAp();
        } else if (g_cur.ssid[0] && (int32_t)(now - g_retryMs) >= 0) {
            g_retryMs = now + AP_RETRY_MS;
            startSta(g_cur);
        }
        break;

    case WM_TRYING:
        if (up && (int32_t)(now - g_armMs) >= 0 && strcmp(WiFi.SSID().c_str(), g_try.ssid) == 0) {
            const bool saved = saveCreds(g_try);
            g_cur = g_try;
            Serial.printf("Wi-Fi \"%s\" works%s\n", g_cur.ssid, saved ? ", saved" : " (could not save to flash)");
            g_state = WM_CONNECTED;
            stopAp();
        } else if ((int32_t)(now - g_deadlineMs) >= 0) {
            snprintf(g_error, sizeof(g_error),
                     "Could not join \"%s\". Check the password, and that it is a 2.4 GHz network in range.",
                     g_try.ssid);
            Serial.println(g_error);
            WiFi.disconnect(false, false);
            connectSaved();   // back to the previous network, or the AP if there was none
        }
        break;
    }
}

// ---------------------------------------------------------------- HTTP
static void sendJson(AsyncWebServerRequest *r, int code, const JsonDocument &d) {
    String s;
    serializeJson(d, s);
    r->send(code, "application/json", s);
}

static void sendError(AsyncWebServerRequest *r, int code, const char *msg) {
    JsonDocument d;
    d["error"] = msg;
    sendJson(r, code, d);
}

// Never includes a password: this endpoint is reachable by anyone on the network.
static void handleStatus(AsyncWebServerRequest *r) {
    JsonDocument d;
    const bool up = wifi_manager_connected();
    d["state"] = WM_NAMES[g_state];
    d["connected"] = up;
    d["ssid"] = g_cur.ssid;
    if (up) {
        d["ip"] = WiFi.localIP().toString();
        d["rssi"] = WiFi.RSSI();
    }
    d["host"] = String(g_host) + ".local";
    d["ap"] = g_apUp;
    if (g_apUp) {
        d["apSsid"] = g_apSsid;
        d["apIp"] = WiFi.softAPIP().toString();
    }
    if (g_error[0]) d["error"] = g_error;
    sendJson(r, 200, d);
}

void wifi_manager_register_routes(AsyncWebServer &server) {
    // Registration order matters: the server matches "/api/wifi" as a prefix of "/api/wifi/...", so
    // the more specific routes must come first or the status handler answers for them.
    server.on("/api/wifi/scan", HTTP_GET, handleScan);

    // POST /api/wifi/forget -- erase the saved network and open the setup AP.
    server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *r) {
        g_requestMs = millis();
        g_forgetFlag = true;
        r->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/wifi", HTTP_GET, handleStatus);

    // POST /api/wifi  JSON {ssid, password}. Answers at once; the board then tests the network in
    // the background. Poll GET /api/wifi for "trying" -> "connected" (or back to "ap" with an error).
    server.on("/api/wifi", HTTP_POST,
              [](AsyncWebServerRequest *r) {}, nullptr,
              [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static String buf;
        if (index == 0) buf = "";
        buf.concat((const char *)data, len);
        if (index + len != total) return;

        JsonDocument doc;
        if (deserializeJson(doc, buf)) { sendError(req, 400, "malformed JSON"); return; }
        const String ssid = doc["ssid"] | "";
        const String pass = doc["password"] | "";
        if (ssid.length() < 1 || ssid.length() > 32) { sendError(req, 400, "network name must be 1-32 characters"); return; }
        // WPA2 passphrases are 8-63 characters; an empty one means an open network.
        if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63)) {
            sendError(req, 400, "password must be 8-63 characters (leave it empty for an open network)");
            return;
        }
        if (g_state == WM_TRYING || g_pendingFlag) { sendError(req, 409, "still trying the previous network"); return; }

        portENTER_CRITICAL(&g_mux);
        strlcpy(g_pending.ssid, ssid.c_str(), sizeof(g_pending.ssid));
        strlcpy(g_pending.pass, pass.c_str(), sizeof(g_pending.pass));
        g_requestMs = millis();
        g_pendingFlag = true;
        portEXIT_CRITICAL(&g_mux);
        req->send(200, "application/json", "{\"ok\":true}");
    });
}

bool wifi_manager_captive_redirect(AsyncWebServerRequest *request) {
    if (!g_apUp || request->method() == HTTP_OPTIONS) return false;
    const String ap = WiFi.softAPIP().toString();
    if (request->host() == ap) return false;        // a real 404 on our own address
    request->redirect("http://" + ap + "/#setup");
    return true;
}

bool wifi_manager_request_on_setup_ap(AsyncWebServerRequest *request) {
    if (!g_apUp || request->client() == nullptr) return false;
    return request->client()->localIP() == WiFi.softAPIP();   // the address the client connected to
}

bool wifi_manager_setup_ap_active() {
    return g_apUp;
}
