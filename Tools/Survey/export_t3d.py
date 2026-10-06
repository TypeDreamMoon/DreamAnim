import os
import unreal

OUT = os.environ.get("DREAMANIM_T3D_OUT") or os.path.join(unreal.Paths.project_saved_dir(), "DreamAnimSurvey", "t3d")
os.makedirs(OUT, exist_ok=True)

reg = unreal.AssetRegistryHelpers.get_asset_registry()
reg.search_all_assets(True)
assets = reg.get_assets_by_path(os.environ.get("DREAMANIM_T3D_ROOT", "/ALS"), recursive=True)

want = {"AnimBlueprint", "ControlRigBlueprint", "RigVMBlueprint"}
summary = []
for ad in assets:
    cls = str(ad.asset_class_path.asset_name)
    if cls not in want:
        continue
    path = str(ad.package_name)
    obj = unreal.load_asset(path)
    if obj is None:
        summary.append(f"LOADFAIL\t{cls}\t{path}")
        continue
    fname = path.strip("/").replace("/", "__") + ".t3d"
    task = unreal.AssetExportTask()
    task.object = obj
    task.filename = OUT + "/" + fname
    task.automated = True
    task.prompt = False
    task.replace_identical = True
    task.exporter = unreal.ObjectExporterT3D()
    ok = unreal.Exporter.run_asset_export_task(task)
    parent = ""
    try:
        pc = obj.get_editor_property("parent_class")
        parent = pc.get_path_name() if pc else ""
    except Exception as e:
        parent = f"?{e}"
    size = os.path.getsize(task.filename) if os.path.exists(task.filename) else -1
    summary.append(f"{'OK' if ok else 'FAIL'}\t{cls}\t{path}\t{parent}\t{size}")

with open(OUT + "/_summary.tsv", "w", encoding="utf-8") as f:
    f.write("\n".join(summary) + "\n")
unreal.log("ALS_T3D_DONE " + str(len(summary)))
