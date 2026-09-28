// Adapted from the owner's own orcaslicer-android-engine project (local, /mnt/faststorage/orcaslicer-android-engine),
// which cross-compiles upstream OrcaSlicer's libslic3r for Android. Licensed AGPL-3.0-or-later
// (same as OrcaSlicer itself, and the same family already governing this app since the Helix port).
// See THIRD_PARTY_NOTICES.md and docs/WORK_ORDER.md's WO-13 entry.
// libslic3r's NSVGUtils.cpp (Emboss SVG import) calls nsvgParse/
// nsvgParseFromFile/nsvgDelete, but nanosvg.h is a single-header library:
// exactly one translation unit anywhere in the final link must define
// NANOSVG_IMPLEMENTATION before including it, or those symbols stay
// undefined. Upstream, that TU is src/slic3r/GUI/BitmapCache.cpp -- GUI-only,
// so a headless (SLIC3R_GUI=OFF) build never compiles it. Providing it here
// keeps the fix in our own tree rather than patching libslic3r itself.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
