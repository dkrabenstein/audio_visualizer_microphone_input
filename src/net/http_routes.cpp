#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "audio/audio_source.h"
#include "pattern/expr_vm.h"
#include "led/strip_config.h"
#include "net/wifi_manager.h"
#include "net/api_guard.h"
#include "net/json_body.h"
#include "pattern/slot_store.h"
#include "pattern/program_hash.h"
#include "led/light_state.h"
#include "led/render.h"
#include "net/http_routes.h"

// HTTP connection cap. Every open TCP connection costs lwIP buffers and AsyncTCP work on core 0, and
// that work competes with FastLED's RMT interrupt. With many browser pages open the board stalled and
// the watchdogs reset it. Past this many simultaneous connections a new one is refused straight away
// (closed before any request is parsed), so a crowd of pages degrades to "some requests fail and the
// page retries" instead of a reboot loop. 1-4 pages ran clean, and each connection is short-lived
// (Connection: close), so 6 leaves headroom for bursts without letting the pile-up grow.
static const uint8_t MAX_HTTP_CLIENTS = 6;
// While the setup access point is up, the cap is higher. A single browser opens up to 6 connections to
// one host by itself, and a phone or laptop that has just joined the setup network also fires its own
// "is there internet?" probes and often opens the setup page unasked. With a cap of 6, those together
// filled every slot and a real request was turned away (measured: "6 open now", requests failing in
// 20 ms). The setup network has at most 3 clients and no crowd of open pages, which is what the lower
// cap guards against.
static const uint8_t MAX_HTTP_CLIENTS_SETUP = 10;

class CappedWebServer : public AsyncWebServer {
public:
    explicit CappedWebServer(uint16_t port) : AsyncWebServer(port) {
        // Replace the base class's onClient handler with one that counts connections.
        _server.onClient([](void *s, AsyncClient *c) {
            if (c == nullptr) return;
            if (liveClients >= (wifi_manager_setup_ap_active() ? MAX_HTTP_CLIENTS_SETUP : MAX_HTTP_CLIENTS)) {
                refused++;
                c->close(true);
                c->free();
                delete c;
                return;
            }
            c->setRxTimeout(3);
            auto *r = new AsyncWebServerRequest((AsyncWebServer *)s, c);
            if (r == nullptr) {
                c->close(true);
                c->free();
                delete c;
                return;
            }
            liveClients++;
            // The request object lives exactly as long as its connection; this fires on close.
            r->onDisconnect([]() { if (liveClients > 0) liveClients--; });
        }, this);
    }
    static uint8_t liveClients;
    static uint32_t refused;
};
uint8_t  CappedWebServer::liveClients = 0;
uint32_t CappedWebServer::refused = 0;

static CappedWebServer server(80);

// ---------------------------------------------------------------- HTTP Handlers
static void handlePatternBody(AsyncWebServerRequest *req, uint8_t *data,
                              size_t len, size_t index, size_t total) {
    static String buf;
    if (index == 0) buf = "";
    buf.concat((const char *)data, len);
    if (index + len != total) return;

    JsonDocument doc;
    if (deserializeJson(doc, buf)) {
        req->send(400, "application/json", "{\"error\":\"malformed JSON\"}");
        return;
    }

    if ((doc["abi"] | 0) != EXPR_ABI_VERSION) {
        req->send(400, "application/json", "{\"error\":\"ABI mismatch - reload page\"}");
        return;
    }

    const int slot = doc["slot"] | -1;
    if (slot < 0 || slot >= NUM_SLOTS) {
        req->send(400, "application/json", "{\"error\":\"slot out of range\"}");
        return;
    }

    JsonArray code = doc["code"];
    if (code.isNull() || code.size() == 0 || code.size() > EXPR_MAX_OPS) {
        req->send(400, "application/json", "{\"error\":\"bad code length\"}");
        return;
    }

    ExprProgram p{};
    p.count = 0;
    for (JsonArray op : code) {
        if (op.size() < 1) {
            req->send(400, "application/json", "{\"error\":\"bad op\"}");
            return;
        }
        // An opcode is one byte. Read it as a whole number first, so a value such as 272 is refused instead of
        // being cut down to 16 (add). `| -1` gives -1 when the value is not a whole number at all.
        const int opcode = op[0] | -1;
        if (opcode < 0 || opcode > 255) {
            req->send(400, "application/json", "{\"error\":\"opcode must be a whole number 0-255\"}");
            return;
        }
        p.ops[p.count].op  = (uint8_t)opcode;
        p.ops[p.count].arg = op.size() > 1 ? (float)(op[1] | 0.0f) : 0.0f;
        p.count++;
    }

    // expr_eval() has no runtime checks, so nothing reaches it unvalidated.
    const ExprError err = expr_validate(&p);
    if (err != EXPR_OK) {
        JsonDocument e;
        e["error"] = expr_error_str(err);
        String es;
        serializeJson(e, es);
        req->send(400, "application/json", es);
        return;
    }

    const char *name = doc["name"] | "untitled";
    if (!ram_store_save((uint8_t)slot, name, &p)) {
        req->send(500, "application/json", "{\"error\":\"failed to save to RAM\"}");
        return;
    }

    JsonDocument r;
    r["ok"] = true; 
    r["slot"] = slot;
    r["ops"] = p.count; 
    r["depth"] = p.maxDepth;
    String s; 
    serializeJson(r, s);
    req->send(200, "application/json", s);
}

void http_routes_begin() {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    // Any origin may call the API, because the laptop-hosted page is served from another origin.
    // That is safe only because every change needs the X-Api-Token header (see api_guard.h): a
    // stray page can read, but cannot write. The header has to be listed here or browsers refuse
    // to send it cross-origin.
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, X-Api-Token");

    // First, so it sees every request before the real routes can answer it.
    api_guard_install(server);

    server.onNotFound([](AsyncWebServerRequest *request) {
        // While the setup AP is up, unknown hosts (phone connectivity probes) go to the setup page.
        if (wifi_manager_captive_redirect(request)) return;
        if (request->method() == HTTP_OPTIONS) {
            request->send(200);
        } else {
            request->send(404);
        }
    });

    // POST /api/pattern
    server.on("/api/pattern", HTTP_POST,
              reject_empty_body, nullptr, handlePatternBody);

    // POST /api/select
    server.on("/api/select", HTTP_POST, [](AsyncWebServerRequest *r) {
        // Strict whole number in range: the range check happens before the cast to a byte, so 256 cannot wrap to 0.
        int slot;
        if (!require_int_param(r, "slot", 0, NUM_SLOTS - 1, &slot)) return;
        if (!ram_store_activate((uint8_t)slot)) {
            send_error(r, "invalid slot index");   // the page matches this text to re-upload presets after a reboot
            return;
        }
        g_whiteK = 0;   // picking a pattern or colour ends any white light
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/white   kelvin=2000..6500 (or kelvin=off), form body or query string
    server.on("/api/white", HTTP_POST, [](AsyncWebServerRequest *r) {
        String v;
        if (!get_param(r, "kelvin", v)) { send_error(r, "missing kelvin parameter"); return; }
        if (v == "off") {
            g_whiteK = 0;
            r->send(200, "application/json", "{\"ok\":true,\"kelvin\":null}");
            return;
        }
        int k;
        if (!parse_whole_number(v.c_str(), WHITE_MIN_K, WHITE_MAX_K, &k)) {
            send_error(r, String("kelvin must be a whole number ") + WHITE_MIN_K + "-" + WHITE_MAX_K + " or off");
            return;
        }
        g_whiteK = (uint16_t)k;
        r->send(200, "application/json", String("{\"ok\":true,\"kelvin\":") + k + "}");
    });

    // GET /api/slots
    server.on("/api/slots", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument d;
        d["active"] = ram_store_activeSlot();
        d["fps"] = g_fps;
        d["evalUs"] = g_evalUs;
        d["frames"] = g_frames;        // advances every rendered frame; a stuck value means rendering stalled
        d["stage"]  = g_renderStage;   // where the render task is: see g_renderStage
        const uint16_t wk = g_whiteK;
        if (wk) {
            d["white"] = wk;   // active white light's colour temperature (K)
            const CRGB w = kelvinToRGB(wk);
            JsonArray rgb = d["whiteRgb"].to<JsonArray>();   // the exact colour being shown
            rgb.add(w.r); rgb.add(w.g); rgb.add(w.b);
        } else {
            d["white"] = nullptr;
        }
        AudioFeatures af;
        audio_source_get(&af);
        d["level"] = af.level;  // latest audio level; 0 = silence / no input
        d["source"] = audio_source_kind() == AUDIO_SRC_MIC ? "mic" : "udp";
        d["brightness"] = g_brightnessPct;
        d["on"] = (bool)g_lightsOn;   // cast: ArduinoJson would serialise a volatile bool as 0/1

        StripConfig sc;
        strip_config_get(&sc);
        JsonArray stripLens = d["stripLengths"].to<JsonArray>();
        for (int i = 0; i < NUM_STRIPS; i++) {
            stripLens.add(sc.count[i]);
        }

        JsonArray a = d["slots"].to<JsonArray>();
        for (uint8_t i = 0; i < NUM_SLOTS; i++) {
            JsonObject o = a.add<JsonObject>();
            o["i"] = i; 
            o["used"] = slot_store_get(i).used;
            o["name"] = slot_store_get(i).name; 
            o["ops"] = slot_store_get(i).program.count;
            if (slot_store_get(i).used) {
                char hx[9];
                snprintf(hx, sizeof(hx), "%08x", (unsigned)programHash(slot_store_get(i).program));
                o["hash"] = hx;   // see programHash(); ArduinoJson copies the string
            }
        }

        String s; 
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // GET /api/status -- health numbers for the Setup tab's Diagnostics panel, read-only.
    // resetReason says why the board last restarted, so a crash can be diagnosed without a USB cable.
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r) {
        const char *reason;
        bool abnormal = false;   // a restart the firmware did not ask for
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   reason = "power on"; break;
            case ESP_RST_SW:        reason = "software restart"; break;
            case ESP_RST_DEEPSLEEP: reason = "wake from deep sleep"; break;
            case ESP_RST_EXT:       reason = "reset pin"; break;
            case ESP_RST_PANIC:     reason = "panic (crash)"; abnormal = true; break;
            case ESP_RST_INT_WDT:   reason = "interrupt watchdog"; abnormal = true; break;
            case ESP_RST_TASK_WDT:  reason = "task watchdog"; abnormal = true; break;
            case ESP_RST_WDT:       reason = "other watchdog"; abnormal = true; break;
            case ESP_RST_BROWNOUT:  reason = "brownout (supply dipped)"; abnormal = true; break;
            default:                reason = "unknown"; break;
        }
        JsonDocument d;
        d["uptimeS"] = (uint32_t)(esp_timer_get_time() / 1000000);   // millis() wraps at 49 days; this does not
        d["resetReason"] = reason;
        d["abnormalReset"] = abnormal;
        d["freeHeap"] = ESP.getFreeHeap();
        d["minFreeHeap"] = ESP.getMinFreeHeap();   // lowest since boot: shows leaks and near misses
        d["wifiConnected"] = WiFi.status() == WL_CONNECTED;
        d["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
        d["refused"] = CappedWebServer::refused;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // POST /api/power   on=1 or on=0 (form body or query string)
    server.on("/api/power", HTTP_POST, [](AsyncWebServerRequest *r) {
        String v;
        if (!get_param(r, "on", v)) { send_error(r, "missing on parameter"); return; }
        if (v != "0" && v != "1") { send_error(r, "on must be 0 or 1"); return; }
        g_lightsOn = (v == "1");
        r->send(200, "application/json", String("{\"ok\":true,\"on\":") + (g_lightsOn ? "true" : "false") + "}");
    });

    // POST /api/brightness   value=0..100 (form body or query string)
    server.on("/api/brightness", HTTP_POST, [](AsyncWebServerRequest *r) {
        int pct;
        if (!require_int_param(r, "value", 0, 100, &pct)) return;
        g_brightnessPct = (uint8_t)pct;
        r->send(200, "application/json", String("{\"ok\":true,\"value\":") + pct + "}");
    });

    // GET /api/audio -- latest audio features, read-only. The web UI's live
    // sound meter polls this. Same data the expression VM sees.
    server.on("/api/audio", HTTP_GET, [](AsyncWebServerRequest *r) {
        AudioFeatures af;
        audio_source_get(&af);
        JsonDocument d;
        JsonArray b = d["bands"].to<JsonArray>();
        for (int i = 0; i < 8; i++) b.add(af.bands[i]);
        d["level"] = af.level;
        d["bass"]  = af.bass;
        d["mid"]   = af.mid;
        d["treb"]  = af.treb;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // GET /api/tuning -- current mic level settings plus the live per-band level in
    // dBFS ("raw"), so the UI can show each band against its floor.
    server.on("/api/tuning", HTTP_GET, [](AsyncWebServerRequest *r) {
        AudioTuning t; float raw[8]; bool cal;
        audio_tuning_get(&t, raw, &cal);
        JsonDocument d;
        JsonArray fl = d["floors"].to<JsonArray>();
        JsonArray rw = d["raw"].to<JsonArray>();
        for (int i = 0; i < 8; i++) { fl.add(t.floorDb[i]); rw.add(raw[i]); }
        d["margin"]  = t.marginDb;
        d["minCeil"] = t.minCeilDb;
        d["smoothness"] = t.smoothness;
        d["calibrating"] = cal;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // POST /api/tuning  JSON {floors:[8], margin, minCeil, smoothness} -- any field optional.
    // Applies live; not saved until POST /api/mic/save.
    server.on("/api/tuning", HTTP_POST,
              reject_empty_body, nullptr,
              [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static String buf;
        if (index == 0) buf = "";
        buf.concat((const char *)data, len);
        if (index + len != total) return;

        JsonDocument doc;
        if (deserializeJson(doc, buf)) {
            req->send(400, "application/json", "{\"error\":\"malformed JSON\"}");
            return;
        }
        AudioTuning t;
        audio_tuning_get(&t, nullptr, nullptr);
        JsonArray fl = doc["floors"];
        if (!fl.isNull()) {
            if (fl.size() != 8) {
                req->send(400, "application/json", "{\"error\":\"floors needs 8 values\"}");
                return;
            }
            for (int i = 0; i < 8; i++) t.floorDb[i] = fl[i] | t.floorDb[i];
        }
        t.marginDb  = doc["margin"]  | t.marginDb;
        t.minCeilDb = doc["minCeil"] | t.minCeilDb;
        t.smoothness = doc["smoothness"] | t.smoothness;
        audio_tuning_set(t);   // clamps to safe ranges
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/mic/calibrate -- ~3 s: make the current room the floor table.
    server.on("/api/mic/calibrate", HTTP_POST, [](AsyncWebServerRequest *r) {
        audio_tuning_calibrate();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/mic/save -- persist the current settings to flash.
    server.on("/api/mic/save", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (audio_tuning_save()) r->send(200, "application/json", "{\"ok\":true}");
        else r->send(500, "application/json", "{\"error\":\"save failed\"}");
    });

    // POST /api/mic/reset -- back to the built-in defaults (also clears the saved copy).
    server.on("/api/mic/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
        audio_tuning_reset();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // GET /api/strips -- LED count and direction per strip, plus the limits and the as-built counts.
    server.on("/api/strips", HTTP_GET, [](AsyncWebServerRequest *r) {
        StripConfig c, def;
        strip_config_get(&c);
        strip_config_defaults(&def);
        JsonDocument d;
        JsonArray cnt = d["counts"].to<JsonArray>();
        JsonArray rev = d["reversed"].to<JsonArray>();
        JsonArray dfl = d["defaults"].to<JsonArray>();
        for (int i = 0; i < NUM_STRIPS; i++) {
            cnt.add(c.count[i]);
            rev.add(c.reversed[i] != 0);
            dfl.add(def.count[i]);
        }
        d["max"] = STRIP_MAX_LEDS;
        String s;
        serializeJson(d, s);
        r->send(200, "application/json", s);
    });

    // POST /api/strips  JSON {counts:[4], reversed:[4]} -- either field optional.
    // Applies live; not saved until POST /api/leds/save.
    server.on("/api/strips", HTTP_POST,
              reject_empty_body, nullptr,
              [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static String buf;
        if (index == 0) buf = "";
        buf.concat((const char *)data, len);
        if (index + len != total) return;

        JsonDocument doc;
        if (deserializeJson(doc, buf)) {
            req->send(400, "application/json", "{\"error\":\"malformed JSON\"}");
            return;
        }
        StripConfig c;
        strip_config_get(&c);

        JsonArray cnt = doc["counts"];
        if (!cnt.isNull()) {
            if (cnt.size() != NUM_STRIPS) {
                req->send(400, "application/json", "{\"error\":\"counts needs 4 values\"}");
                return;
            }
            for (int i = 0; i < NUM_STRIPS; i++) {
                const int v = cnt[i] | -1;
                if (v < 1 || v > STRIP_MAX_LEDS) {
                    JsonDocument e;
                    e["error"] = String("each count must be a whole number 1-") + STRIP_MAX_LEDS;
                    String es;
                    serializeJson(e, es);
                    req->send(400, "application/json", es);
                    return;
                }
                c.count[i] = (uint16_t)v;
            }
        }
        JsonArray rev = doc["reversed"];
        if (!rev.isNull()) {
            if (rev.size() != NUM_STRIPS) {
                req->send(400, "application/json", "{\"error\":\"reversed needs 4 values\"}");
                return;
            }
            for (int i = 0; i < NUM_STRIPS; i++) c.reversed[i] = (rev[i] | false) ? 1 : 0;
        }
        strip_config_set(c);
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/leds/save -- persist the strip settings to flash.
    server.on("/api/leds/save", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (strip_config_save()) r->send(200, "application/json", "{\"ok\":true}");
        else r->send(500, "application/json", "{\"error\":\"save failed\"}");
    });

    // POST /api/leds/reset -- back to the strips as built (also clears the saved copy).
    server.on("/api/leds/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
        strip_config_reset();
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // POST /api/auth -- changes nothing. It is a POST, so the API guard only lets it through with
    // the right token; the web page calls it after a pairing link to check that the token works.
    server.on("/api/auth", HTTP_POST, [](AsyncWebServerRequest *r) {
        r->send(200, "application/json", "{\"ok\":true}");
    });

    // Wi-Fi network: status, scan, set (tested before it is saved), forget.
    wifi_manager_register_routes(server);

    // Web UI: serve data/www (uploaded to LittleFS as /www) at the site root.
    // Registered after the /api routes so those always match first.
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

    server.begin();
}

void http_log_refusals() {
    // Say so when the connection cap turns clients away (at most once a second), so a burst of
    // refused connections shows up on the serial monitor instead of looking like a flaky network.
    static uint32_t lastRefused = 0, lastRefusedMs = 0;
    if (CappedWebServer::refused != lastRefused && millis() - lastRefusedMs >= 1000) {
        lastRefusedMs = millis();
        Serial.printf("HTTP: %u connections refused so far (cap %u, %u open now)\n",
                      (unsigned)CappedWebServer::refused, (unsigned)MAX_HTTP_CLIENTS,
                      (unsigned)CappedWebServer::liveClients);
        lastRefused = CappedWebServer::refused;
    }
}
