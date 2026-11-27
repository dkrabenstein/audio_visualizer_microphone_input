# audio_visualizer_microphone_input
# Setup

## First-time setup

1. **Flash over USB.** Firmware: `pio run -t upload`. Web UI files: `pio run -t uploadfs`.
2. **Create `include/secrets.h`** from `include/secrets.h.example` (it is gitignored). Set `OTA_PASSWORD` and `API_TOKEN` (a long random value, e.g. `openssl rand -hex 16`).
3. **Put the board on your Wi-Fi.** A board with no saved network opens its own network, `visualizer-esp-32-XXXX` (password `visualizer-esp-32`; change it with `AP_PASSWORD` in `secrets.h`). Join it from a phone or laptop, open `http://192.168.4.1/`, and pick your network under Setup > Wi-Fi network. The board tests the network before saving it, so a typo cannot lock it out. Re-run this any time with "Forget network".
4. **Reserve the board's IP address and name it in the router** (next section).
5. **Pair each browser** so it does not ask for the API token: run `python scripts/pair_link.py` and open the link it prints once in that browser. Treat the link like a password.

## Reserve the board's IP address and give it a name in the router

The board gets its address from the router (DHCP), and the address can change. The wireless upload (`upload_port` in `platformio.ini`) and the router's DNS name both depend on it, so tell the router to always give this board the same one. The board is called `visualizer-esp-32` everywhere.

1. **Find the board's MAC address.** In the router's list of connected devices the board shows up as `visualizer-esp-32`. Or, on a PC on the same network: `arp -a <board IP>` (the IP is printed on the serial monitor as `ESP32 IP: ...`, and shown in the web UI under Setup > Wi-Fi network).
2. **Open the router's admin page.** Its address is the default gateway: `ipconfig` (Windows) or `ip route` (Linux/macOS). Common ones are `192.168.1.1` and `192.168.1.254`. Log in with the router's admin password.
3. **Find the DHCP reservation page.** Routers call it "DHCP reservation", "Static lease", "Address reservation" or "Fixed IP", usually under LAN, Home Network or Advanced.
4. **Add a reservation:** MAC address of the board, the IP to keep (the one it has now is fine), and the name `visualizer-esp-32` if the form asks for one. Save or apply.
5. **Make the board pick it up.** Reboot the board (or wait for its lease to renew) and check that the IP shown in the web UI matches the reserved one.
6. **Name it in the router's DNS.** The board announces its name (`visualizer-esp-32`) when it asks for an address, and most routers then list it and answer DNS for it. Check from a PC: `nslookup visualizer-esp-32 <router IP>` should print the reserved IP. If your router has a "Local DNS", "Host names" or "DNS records" page, you can instead add an entry there: name `visualizer-esp-32`, address = the reserved IP. Some ISP routers have no such page; the announced name and `visualizer-esp-32.local` still work.
7. **If the reserved IP changes,** update `upload_port` in `platformio.ini`.

Once done, the board is reachable at `http://visualizer-esp-32.local/` (works on most phones and computers without the router) and, through the router's DNS, at `http://visualizer-esp-32/`.

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

Are there any tests that should be put in the tests directory as unit tests? Or would I put unit tests somewhere else?

Update main to relect the online version. Make a new branch from main called feature/independent-strips and checkout the branch. Push the branch to online and set it as the upstream branch.

- Per-strip patterns.
Add firmware capability for changing each LED strip independently

basic page: add buttons that say "Set all strips" and "Set single strip" at the bottom of the colour and white cards. The "Set all strips" button should set all strips to the selected colour/shade. The lights should not change until either the "Set all strips" and "Set single strip" button is pressed. Clicking the "Set single strip" button should prompt a pop up that let's you select from one of the 4 strips.

pattern page: add the "Set all strips" and "Set single strip" buttons at the bottom of each patern card. Make the buttons have the same style and functionality of the buttons on the basic page

custom page: Add "Set all strips" and "Set single strip" buttons to replace the upload button

make sure website looks good

- Beat or tempo detection. You already have per-band energy, so a beat pulse for patterns to use is a natural next feature. Explain this
- WebSocket or server-sent events instead of polling. One connection per page would replace repeated requests, which helps the connection limit we just hit. explain this

- ask about current functionallity
Is there any additional functionallity that would be worth adding?

- ask about adding tests
Are there any more tests that should be run?

- ask about refactoring
Is there any refactoring that needs to be done?

- ask about resume portfolio viability
Outside of documentation in the readme file, is there anything I should change before adding this project to my portfolio?
