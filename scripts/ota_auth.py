# PlatformIO pre-script for the OTA env: reads OTA_PASSWORD from include/secrets.h
# so the password lives in one (gitignored) place and is not duplicated in platformio.ini.
import re
Import("env")

text = open(env.subst("$PROJECT_DIR/include/secrets.h"), encoding="utf-8").read()
m = re.search(r'#define\s+OTA_PASSWORD\s+"([^"]*)"', text)
if not m:
    raise SystemExit("OTA_PASSWORD not found in include/secrets.h")
env.Append(UPLOAD_FLAGS=["--auth=" + m.group(1)])
