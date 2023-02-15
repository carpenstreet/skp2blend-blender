import os
import sys
import subprocess


argv = sys.argv
argv = argv[argv.index("--") + 1 :]

SKETCHUP_DIR = argv[0]
SKETCHUP_LIST = os.listdir(SKETCHUP_DIR)
BLENDER_DIR = (
    os.getcwd() + "\\..\\build_windows_x64_vc17_Release\\bin\\Release\\blender.exe"
)


for filepath in SKETCHUP_LIST:
    filepath = os.path.join(SKETCHUP_DIR, filepath)
    command = " ".join(
        [BLENDER_DIR, "--background", "--python", "background.py", "--", filepath]
    )
    subprocess.call(command, shell=True)
