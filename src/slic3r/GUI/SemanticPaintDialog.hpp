#pragma once

namespace Slic3r {
namespace GUI {

// True when exactly one object is selected.
bool can_run_semantic_paint();

// "AI auto-paint": segments the selected object, shows the exact views that would be sent, asks Claude (the user's
// subscription, through the Claude Code CLI) which filament each segment should get, lets the user adjust the answer and applies it as undoable paint.
void run_semantic_paint();

} // namespace GUI
} // namespace Slic3r
