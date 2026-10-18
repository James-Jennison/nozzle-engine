#ifndef slic3r_GUI_U1FilamentSync_hpp_
#define slic3r_GUI_U1FilamentSync_hpp_

#include <string>

class wxWindow;

namespace Slic3r::GUI {

/// True when the edited printer is a Snapmaker U1 with a LAN address configured.
bool u1_filament_sync_available();

/// Reads the loaded filaments from the U1's Moonraker and applies them to the plate's
/// filament slots: count, matching filament preset (by type, preferring the reported
/// vendor) and colour. Shows a summary and returns false when nothing was applied.
bool sync_filaments_from_u1(wxWindow *parent);

} // namespace Slic3r::GUI

#endif
