// network.h -- the network task (core 0): Wi-Fi, mDNS, OTA, the UDP audio receiver and the discovery beacon,
// plus the web server and the render watchdog.

#pragma once

void network_start();   // starts the network task on core 0
