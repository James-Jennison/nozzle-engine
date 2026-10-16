#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace Slic3r {

struct SemanticPaintSettings
{
    std::string model;    // empty = the CLI's default model
    std::string cli_path; // empty = look for `claude` on PATH and in the usual install folders
};

struct SemanticPaintReply
{
    bool        ok{false};
    std::string text;  // assistant text
    std::string error; // set when !ok
};

// Runs the Claude Code CLI (`claude -p`, signed in with the user's Claude subscription) on the prompt and the PNG
// images (raw bytes, view order) and blocks until it answers, fails, times out or *cancel becomes true.
SemanticPaintReply semantic_paint_query(const SemanticPaintSettings &settings, const std::string &prompt,
                                        const std::vector<std::string> &png_files, const std::atomic<bool> *cancel = nullptr);

// Path of the Claude Code CLI, empty if it cannot be found.
std::string semantic_paint_find_cli(const std::string &configured);

} // namespace Slic3r
