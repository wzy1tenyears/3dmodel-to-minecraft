# 3dmodel-to-minecraft

Native Windows C++ tooling for importing 3D models into Minecraft worlds, with
both a graphical interface and a command-line interface.

- [English documentation](cpp/README.md)
- [中文文档](cpp/README.zh-CN.md)
- [Build instructions](cpp/BUILDING.md)
- [Current engineering TODO](TODO.md)

The active source tree is under `cpp/`. Build output, local worlds, generated
Minecraft texture atlases, and installer archives are intentionally excluded
from this source repository.

## Minecraft assets

This repository does not redistribute Minecraft textures. The atlas utilities
under `cpp/tools/atlas-26.2/` work with assets extracted locally from a copy of
Minecraft that you are entitled to use. See the tool README for the expected
directory layout.

This is an unofficial community project and is not approved by or associated
with Mojang or Microsoft.

## License

Project source is licensed under GPL-3.0. Third-party components retain their
own licenses; see `cpp/THIRD_PARTY_NOTICES.md`.
