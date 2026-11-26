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

3. After that, look at the FastLED RMT stall itself, for example a newer FastLED or a different RMT setting. That's a firmware experiment, so I'd do it separately.
- Router IP reservation: I recommended it, but I haven't set it up.
- OTA password: it's printed in espota's debug output, so you may want to change it.
- ask about current functionallity
- ask about refactoring
- ask about resume portfolio viability

What's done
- Polling fix: on the Custom tab, the preview now polls /api/audio at 10 Hz. Before, it polled at 20 Hz on every tab. It also pauses when the browser tab is hidden. The Setup-tab meter and tuning pollers pause when hidden too. Requests per second for one open page are now Home 7 (was 25), Basic 0.3 (was 20), Patterns 0 (was 20), Custom 10 (was 20), Setup 8 (was 29).
- Phone layout: the page is now exactly 390 px wide on every tab, and 360 px on a 360 px screen. At 320 px, Basic is 2 px wider than the screen. The five tabs fit, and the Setup slider labels sit above their sliders.
- Re-test: the phone and desktop click-throughs now pass every functional check. The two remaining "failures" are the harmless host.js and favicon 404s.
- On the board: all of this was flashed with uploadfs.

What the polling fix doesn't solve
- The board still crashes with 6 or more real pages open at once. With 6 pages on Custom and 10 on
  Home it hit render stalls, interrupt-watchdog panics, one lwIP assertion failure and a
  task-watchdog abort. It recovered each time, but it reboots in a loop.
- With 1–4 real pages open (200 seconds, including 4 on Setup) there were no failures and no        reboots.                                                                                     Request rate alone doesn't explain it, so I think it's the number of simultaneous connections,    plus every page re-uploading the presets on load. I haven't proven that.                     The panic backtraces point into FastLED's RMT interrupt handler, so the underlying problem is in
  the firmware, not the page. I'd look at that separately.                                                                                                                                       t verified
- Settings after the crash storms: I haven't re-checked that your strips, mic tuning and brightness still match the snapshot since the last six-page test. They did before it, and the tuning is saved in flash. It's a quick check at /api/tuning and /api/strips if you want to confirm.
- Leftover processes: a few headless Chrome, Node or Python processes from my tests may still be  running. Check Task Manager if the laptop feels slow.
- Unexplained reboot: one reboot happened during an earlier desktop test with a single page open. It didn't repeat on the rerun.
- Commit: nothing is committed. data/www/index.html has all the changes, and the test scripts and snapshot are in the scratchpad.