// network_config.h -- the board's network names and ports, in one place (pins are in pins.h).
//
// The laptop dev server (server/server.js) has its own copy of the two ports below, as its defaults, and the
// beacon text. They must match: test/js/test_firmware_sync.js reads this file and server.js and fails if not.

#pragma once
#include <stdint.h>

// Advertised over mDNS, so the board is reachable as http://visualizer-esp-32.local whatever IP DHCP hands out.
// Also the Wi-Fi hostname and the OTA hostname. A string literal (a macro) so it can be joined to other text.
#define MDNS_HOSTNAME "visualizer-esp-32"

// UDP port the board listens on for audio features sent by the PC (the simulated-audio stream).
constexpr uint16_t UDP_PORT = 49152;

// Presence beacon: the board broadcasts a tiny packet on this port every BEACON_INTERVAL_MS so the PC server
// learns the current IP within seconds of any DHCP change, without relying on mDNS.
constexpr uint16_t BEACON_PORT = 49153;
constexpr uint32_t BEACON_INTERVAL_MS = 2000;
