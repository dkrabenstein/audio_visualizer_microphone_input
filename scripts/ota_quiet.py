# PlatformIO post-script for the OTA env. espota.py's --debug flag makes it print every parsed option,
# including "'auth': '<password>'", into the build log. Drop that flag so the OTA password is never echoed.
# --progress and error messages are unaffected; only espota's INFO/DEBUG log lines go away.
Import("env")

env.Replace(UPLOADERFLAGS=[f for f in env["UPLOADERFLAGS"] if f != "--debug"])
