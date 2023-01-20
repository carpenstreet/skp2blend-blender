import os
import platform
import shutil

SOURCE = "./release/scripts/addons_acon3d/io_skp"
DESTINATION = ""
TARGET = "3.0/scripts/addons_acon3d/io_skp"


def copy_scripts(src, dest):
    shutil.copytree(src, dest, dirs_exist_ok=True)


# Set destination path each OS
if platform.system() == "Windows":
    DESTINATION = "../build_windows_x64_vc17_Release/bin/Release/"
    DESTINATION = os.path.join(DESTINATION, TARGET)
elif platform.system() == "Darwin":
    DESTINATION = "../build_darwin/bin/ABLER.app/Contents/Resources/"
    DESTINATION = os.path.join(DESTINATION, TARGET)
else:
    print("Not supported")
    exit(1)

# Copy scripts
try:
    copy_scripts(SOURCE, DESTINATION)
    print("Successfully copied scripts from addons_acon3d/io_skp")
except Exception as e:
    raise e
