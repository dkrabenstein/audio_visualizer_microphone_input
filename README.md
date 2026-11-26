# audio_visualizer_microphone_input
# Overview
# Problem Statement
# Solution
# Archetecture
# Tech Stack
# How it works
# Background
# Known Issues and Limitations
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

1. Fill in the CLAUDE.md placeholders. "Build system" still says [Fill in: ...]. Add the framework (Arduino or ESP-IDF) and the environment name from platformio.ini. The "Laptop dev server" line should be a clean command, cd server && node server.js.
4. Set up the filesystem for data/. uploadfs needs a filesystem partition. Check platformio.ini for board_build.filesystem = littlefs and a partition table that leaves room for it.
6. Check the web UI files and the server. Make data/ the single source of truth. The laptop server should serve data/ directly, which satisfies the "keep in sync" rule without a second copy.
7. Keep power in mind. WiFi adds current draw on the ESP32. It shouldn't matter on a 40A LED supply, but WiFi radio activity can cause brief audio noise on the mic's I2S lines. Watch for this when you test.
8. Open a draft PR into main when you have a first commit. GitHub gave you the link: