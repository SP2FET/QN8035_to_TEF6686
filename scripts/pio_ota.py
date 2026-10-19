# PlatformIO post-script: adds the "ota" target.
#
#   pio run -e lolin_s2_mini -t ota
#
# builds the firmware, then runs scripts/ota_upload.py against the bridge's web UI.
# Host: $OTA_HOST, else custom_ota_host in platformio.ini (the access point address).
# Password (only if BRIDGE_OTA_PASSWORD is set): $OTA_PASSWORD, kept out of the repo.
# The first OTA-capable firmware (and the rollback bootloader) must go over USB once.
import os

Import("env")  # noqa: F821 (SCons global)

script = os.path.join(env.subst("$PROJECT_DIR"), "scripts", "ota_upload.py")  # noqa: F821
host = os.environ.get("OTA_HOST") or env.GetProjectOption(  # noqa: F821
    "custom_ota_host", "192.168.4.1")

env.AddCustomTarget(  # noqa: F821
    name="ota",
    dependencies="$BUILD_DIR/${PROGNAME}.bin",
    actions=f'"$PYTHONEXE" "{script}" --host {host} "$BUILD_DIR/${{PROGNAME}}.bin"',
    title="Upload over Wi-Fi",
    description="Build, then flash over the web OTA endpoint (scripts/ota_upload.py)",
)
