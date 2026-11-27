// http_routes.h -- the web server: the REST API under /api and the web UI files from LittleFS.

#pragma once

void http_routes_begin();    // registers every route and starts the server on port 80
void http_log_refusals();    // call regularly: prints (at most once a second) when the connection cap turned clients away
