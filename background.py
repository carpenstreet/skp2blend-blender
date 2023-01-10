import os
import sys
import bpy

argv = sys.argv
argv = argv[argv.index("--") + 1:]  # get all args after "--"
filename = argv[0]
name, ext = os.path.splitext(filename)


def init_load_setting():
    # 기존 오브젝트(Camera, Sun 제외) 지우기
    bpy.ops.object.select_by_type(extend=False, type="EMPTY")
    bpy.ops.object.select_by_type(extend=True, type="MESH")
    bpy.ops.object.delete()


init_load_setting()
bpy.ops.preferences.addon_enable(module="io_skp")
bpy.ops.acon3d.import_skp(filepath=filename)
bpy.ops.wm.save_mainfile(filepath=name + ".blend")
bpy.ops.wm.quit_blender()
