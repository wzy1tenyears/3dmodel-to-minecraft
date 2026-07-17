# Local atlas tools

These utilities generate the runtime block atlas from Minecraft assets already
available on the local machine. Minecraft assets and generated atlas files are
intentionally not tracked by Git.

Expected default layout:

```text
assets/
  minecraft/
    models/block/*.json
    textures/block/*.png
```

Generate the atlas in `cpp/vendor/ots/res/atlases/`:

```powershell
python .\gen_cpp_vendor_atlas.py
```

Use `--assets-root`, `--vendor-root`, and `--ignore-list` to override the
defaults. `debug_faces.py` accepts the same asset and ignore-list inputs for
diagnosing unsupported model face values.

The generator requires Pillow. Only use assets from a Minecraft installation
or other source that you are entitled to use.
