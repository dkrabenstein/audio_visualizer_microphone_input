// json_body.h -- shared by the POST routes that read a JSON body (/api/pattern, /api/tuning, /api/strips, /api/wifi).
//
// Those routes do their work, and answer, inside the body callback, which the web server only calls when the request
// actually has a body. So the "request complete" handler would normally be empty. With an empty body the callback never
// runs, nothing answers, and the board closes the connection after about 3.5 s. Use this as the handler instead:
// it answers 400 at once when there is no body, and does nothing otherwise (the body callback has already answered).

#pragma once
#include <ESPAsyncWebServer.h>

static inline void reject_empty_body(AsyncWebServerRequest *r) {
    if (r->contentLength() == 0) r->send(400, "application/json", "{\"error\":\"empty body\"}");
}
