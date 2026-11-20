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
explain setup
High pass filter hardware?
have default patterns stored on board
Consider adding OTA so board doesn't need to be flashed by cable

Make the % lable on the sliders 2 px larger. 

Make the "now showing" text on the home page the same lettering as the ✓ patterns loaded message on the custom tab

Change the website background to a gradient from navy to dark gray


Adjust the sensitivity of each frequency band by changing the noise floor. The noise floor is volume above which sound is registered. 

Drag a floor to just above that band's "now" reading in a quiet room, or press Calibrate for manual adjustment.


Add an addition margin to the per band noise floors.

Raise it if the lights flicker in silence, and lower it if they miss quiet sounds.


Set how loud a band must get to reach full strength.
If the lights flash to full on quiet noise between songs, raise it. If the lights never reach full brightness even on loud music, lower it.