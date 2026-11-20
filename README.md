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


To do:
decrease abrupt changes in color
make brightness adjustable
add white light
add plain color
mic sensitivity control
explain setup
High pass filter hardware?
have default patterns stored on board

Split advanved configurations into two different tabs. The first tab should be titled custom. At the top is should say make your own pattern. The page should include the entire expr3ession card, the preview card, the brightness card, and the uplaod and refresh buttons. The other tab should be called setup. The top should say configure your lights. The page should include the esp32 connection and the microphone tunning. The microphone tunning should no longer be hidden. Add these new tabs to the home page.

On the home page: remove the "live from lights" subtitle. Move the lights on button and brightness slider down so it's more centered in the card.

