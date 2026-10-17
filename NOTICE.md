# Notices

The showcase source (`src/showcase.cpp`) is licensed GPL-3.0-or-later.
`LICENSE` contains the GNU General Public License version 3.

## ps5-native-app-boilerplate

The native startup, minimal C++ runtime, linker script, symbol map,
VideoOut setup and bitmap font derive from ps5-native-app-boilerplate:

- Project: https://github.com/BlackBearReloaded/ps5-native-app-boilerplate
- Reference revision: `4f531c4b517f80bcb6b1267135848168d2250047`
- Copyright (C) 2026 BlackBearReloaded
- License: GPL-3.0-or-later

The original headers in `native/` are preserved.

## Presentation assets

The icon, background artwork (`sce_sys/*.png`, `sce_sys/*.dds`) and the
selection track `sce_sys/snd0.at9` ("Night Drive") are original assets by
BlackBearReloaded from the same project, Copyright (C) 2026
BlackBearReloaded, GPL-3.0-or-later. They are separate from the showcase's
in-program synthesized music, which is original to this project.

## External tools

Building uses the public ps5-payload-sdk, LLVM/Clang/lld and
ps5-native-tool; running on macOS uses AnyPS5, Rosetta 2, the Khronos
Vulkan loader and MoltenVK. None of them is included here; their licenses
remain with their projects.
