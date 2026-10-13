# Source build of Wine Mono WPF replacements

Wine Mono 11.3.0's prebuilt `wpfgfx_cor3.dll` files contain static compiler
runtime assertion paths and an absolute PDB path. Rebuild these two DLLs at
source; preserve the rest of the Mono distribution.

Use Wine Mono tag `wine-mono-11.3.0`, commit
`17dd911a3ea04f21d7e4a8dcbff580ad520607eb`, with its `wpf` submodule initialized
at `9178bf7cb4d5d33dfd36f0611153c9f95a337595`. The helper exports immutable
source and uses the native WpfGfx Makefile for both x86 and x86_64.

First create a private compiler installation from the pinned llvm-mingw
`20260922-ucrt` archive documented in `build_mesa_zink.md`, and rebuild its
static C++ runtimes with `build_mingw_runtimes.sh`. Then run:

```sh
bash tools/build_wpf_ps5.sh --source /path/to/wine-mono \
  --work /path/to/new/wpf-build --toolchain /path/to/private/compiler --jobs 3
```

The output contains `lib/{x86,x86_64}/wpfgfx_cor3.dll`, licenses and a manifest
with source pins, compiler archive/runtime hashes, PE architecture and imports.
The license tree includes WPF and Wine texts, LLVM notices, and MinGW's
`share/mingw32` notices for the statically linked CRT archives.
Use a new work directory when changing the recipe. The script preserves
upstream's default optimization, maps diagnostic source paths and omits the
PDB flags. MinGW's CRT archives carry DWARF build paths even when WPF omits
debug flags, so `-Wl,--strip-debug` tells the linker to omit debug sections
when creating the DLL. No completed DLL is postprocessed. Runtime assertion
paths remain executable data and must be fixed by the source runtime rebuild.
The script rejects host paths in the completed DLLs. Copy these DLLs into a
private copy of the original Wine Mono input tree and carry the replacement
manifest and license files into the kit. Never rewrite binary strings.

Wine Mono's upstream default compiler is `20260616-ucrt`; this source rebuild
uses the already pinned `20260922-ucrt` compiler shared with Mesa. Compare
exports/ordinals and imports to the original DLLs, run a host load test when
available, and include an existing .NET title in console acceptance. A path
scan verifies release hygiene; it does not prove WPF rendering behavior.
