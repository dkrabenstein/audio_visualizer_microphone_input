# audio_visualizer_microphone_input
# Overview
# Problem Statement
# Solution
# Archetecture
# Tech Stack
# How it works
# Background
# Known Issues and Limitations
- decrease board's footprint
- move microphone to outside
- add circuit protection for level shifter
# Hardware
# Features
# Performance Metrics
# Liscense
# Acknowledgements

Equations:
default equation (tested)
hue = 0.3·x + 0.03·t + bass + 1.5·level

rotate color, level + bass (tested)
0.4 * t + 2 * 1.5 * bass + 1.5*level

dominent frequency (tested)
 7 *  (0.07*bass + 0.43*mid + 0.86*treb) / (bass + mid + treb)

Spectrum analyer hue shift (tested)
 0.745*x + 3*(bass*sat(1-2*x^2) + mid*sat(1-abs(2*x^2-1)) + treb*sat(2*x^2-1))

Level line (tested)
1 + sat((sat(sqrt(level*bass ^ 3)*2) - x)*160) * (-0.33 - .33*x - .2*bass)

Level line 2 (tested)
.99 + 1.1 * sat((sat(sqrt(level*bass ^ 3)*2) - x)*160) * (-.24 - .2*x - .2*bass)

spectrum bars (tested)
0.5 - 1.3*sat(1.5*(band(0)*sat(1-abs(x*3-0)) + band(2)*sat(1-abs(x*3-1)) + band(5)*sat(1-abs(x*3-2)) + band(7)*sat(1-abs(x*3-3))))

bass bloom (tested)
.55 + 12*sat(bass*level*.5)*tri(x)

- Settings after the crash storms: I haven't re-checked that your strips, mic tuning and brightness still match the snapshot since the last six-page test. They did before it, and the tuning is saved in flash. It's a quick check at /api/tuning and /api/strips if you want to confirm. Has this already been done?

- ask about current functionallity
Is there any additional functionallity that would be worth adding?
2. Wi-Fi setup without reflashing. The SSID and password are compiled in from secrets.h. A fallback access point with a small setup page would let you move the board to another network or hand it to someone else. It's also a strong portfolio feature.
3. A diagnostics panel. Show uptime, reset reason, free heap, Wi-Fi signal strength and the refused-connection count. The reset reason would have told us about the panics without a USB cable. It's cheap to add as an /api/status endpoint plus a section on the Setup tab.
4. Basic API protection. The API allows any origin (Access-Control-Allow-Origin: *) and every POST is open to anyone on your network. A simple shared token would stop a stray page or device from changing your lights. The OTA path is already password-protected, so the web API is the weaker spot.

- Beat or tempo detection. You already have per-band energy, so a beat pulse for patterns to use is a natural next feature.
- WebSocket or server-sent events instead of polling. One connection per page would replace repeated requests, which helps the connection limit we just hit.
- Per-strip patterns. Let each of the four strips run a different pattern, or mirror one.


- ask about adding tests
Are there any more tests that should be run?
Are there any tests that should be put in the tests directory
- ask about refactoring
Is there any refactoring that needs to be done?
- ask about resume portfolio viability
Outside of documentation in the readme file, is there anything I should change before adding this project to my portfolio?
