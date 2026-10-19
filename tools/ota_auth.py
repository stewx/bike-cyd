# PlatformIO post-script for the cyd2usb_ota environment: passes the
# over-the-air update password from ota.txt (not in git) to espota.
Import("env")  # noqa: F821 (provided by PlatformIO)

from pathlib import Path

password_file = Path(env.subst("$PROJECT_DIR")) / "ota.txt"  # noqa: F821
if password_file.exists():
    env.Append(UPLOADERFLAGS=["--auth=" + password_file.read_text().strip()])  # noqa: F821
else:
    print("ota_auth: ota.txt not found, uploading without a password")
