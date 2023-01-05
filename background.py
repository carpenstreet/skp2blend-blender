import os
import sys
import bpy

argv = sys.argv
argv = argv[argv.index("--") + 1:]  # get all args after "--"
filename = argv[0]
name, ext = os.path.splitext(filename)

bpy.ops.preferences.addon_enable(module="io_skp")
bpy.ops.acon3d.import_skp(filepath=filename)
bpy.ops.wm.save_mainfile(filepath=name + ".blend")
bpy.ops.wm.quit_blender()
