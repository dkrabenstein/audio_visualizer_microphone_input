// json_body.h -- shared by the POST routes that read a JSON body (/api/pattern, /api/tuning, /api/strips, /api/wifi).
//
// Those routes do their work, and answer, inside the body callback, which the web server only calls when the request
// actually has a body. So the "request complete" handler would normally be empty. With an empty body the callback never
// runs, nothing answers, and the board closes the connection after about 3.5 s. Use this as the handler instead:
// it answers 400 at once when there is no body, and does nothing otherwise (the body callback has already answered).

#pragma once
#include <ESPAsyncWebServer.h>
#include "param_parse.h"

// ---- Form / query parameters (the small POST routes: /api/select, /api/white, /api/power, /api/brightness) ----

// The parameter's value from the form body, else from the query string; false if it is in neither.
static inline bool get_param(AsyncWebServerRequest *r, const char *name, String &out) {
    const AsyncWebParameter *p = r->hasParam(name, true) ? r->getParam(name, true)
                               : (r->hasParam(name, false) ? r->getParam(name, false) : nullptr);
    if (!p) return false;
    out = p->value();
    return true;
}

static inline void send_error(AsyncWebServerRequest *r, const String &message) {
    r->send(400, "application/json", String("{\"error\":\"") + message + "\"}");
}

// get_param + strict whole-number parse (see param_parse.h) + range check, answering 400 itself on any failure.
// Errors read "missing <name> parameter" or "<name> must be a whole number <lo>-<hi>".
// Returns true only if *out is valid and the caller should carry on.
static inline bool require_int_param(AsyncWebServerRequest *r, const char *name, int lo, int hi, int *out) {
    String v;
    if (!get_param(r, name, v)) { send_error(r, String("missing ") + name + " parameter"); return false; }
    if (!parse_whole_number(v.c_str(), lo, hi, out)) {
        send_error(r, String(name) + " must be a whole number " + lo + "-" + hi);
        return false;
    }
    return true;
}

static inline void reject_empty_body(AsyncWebServerRequest *r) {
    if (r->contentLength() == 0) r->send(400, "application/json", "{\"error\":\"empty body\"}");
}
