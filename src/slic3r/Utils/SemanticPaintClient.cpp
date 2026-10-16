#include "SemanticPaintClient.hpp"

#include "libslic3r/SemanticPaint.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>

namespace fs      = boost::filesystem;
namespace process = boost::process;

namespace Slic3r {

namespace {

constexpr int kTimeoutSeconds = 240;

std::string read_file(const fs::path &p)
{
    std::ifstream in(p.string(), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("semantic-paint-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

SemanticPaintReply run_cli(const SemanticPaintSettings &s, const std::string &prompt, const std::vector<std::string> &png_files,
                           const std::atomic<bool> *cancel)
{
    SemanticPaintReply out;
    const std::string cli = semantic_paint_find_cli(s.cli_path);
    if (cli.empty()) {
        out.error = "The Claude Code CLI (`claude`) was not found. Install Claude Code and sign in with your Claude account, or set its path.";
        return out;
    }
    TempDir tmp;
    std::vector<std::string> paths;
    for (size_t i = 0; i < png_files.size(); ++i) {
        const fs::path p = tmp.path / ("view" + std::to_string(i + 1) + ".png");
        std::ofstream(p.string(), std::ios::binary).write(png_files[i].data(), std::streamsize(png_files[i].size()));
        paths.push_back(p.string());
    }
    const fs::path in_file = tmp.path / "prompt.txt", out_file = tmp.path / "reply.json", err_file = tmp.path / "err.txt";
    {
        std::ofstream in(in_file.string(), std::ios::binary);
        in << SemanticPaint::claude_cli_prompt(prompt, paths);
    }
    std::vector<std::string> args{"-p", "--output-format", "json", "--allowedTools", "Read", "--add-dir", tmp.path.string(), "--no-session-persistence"};
    if (!s.model.empty()) {
        args.push_back("--model");
        args.push_back(s.model);
    }
    try {
        process::child child(cli, process::args(args), process::std_in < in_file, process::std_out > out_file, process::std_err > err_file,
                             process::start_dir(tmp.path.string()));
        const auto start = std::chrono::steady_clock::now();
        while (child.running()) {
            if (cancel && cancel->load()) {
                child.terminate();
                out.error = "Cancelled.";
                return out;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(kTimeoutSeconds)) {
                child.terminate();
                out.error = "The Claude Code CLI did not answer in time.";
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        child.wait();
        const std::string body = read_file(out_file);
        out.text = SemanticPaint::claude_cli_reply_text(body);
        if (out.text.empty()) {
            std::string err = read_file(err_file);
            out.error       = "The Claude Code CLI returned no answer (exit " + std::to_string(child.exit_code()) + ")." +
                        (err.empty() ? std::string() : " " + err.substr(0, 400));
            if (!body.empty() && err.empty())
                out.error += " " + body.substr(0, 400);
            return out;
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.error = std::string("Could not run the Claude Code CLI: ") + e.what();
    }
    return out;
}

} // namespace

std::string semantic_paint_find_cli(const std::string &configured)
{
    if (!configured.empty())
        return fs::exists(configured) ? configured : std::string();
    boost::system::error_code ec;
    const fs::path            on_path = process::search_path("claude");
    if (!on_path.empty())
        return on_path.string();
    std::vector<fs::path> candidates;
    if (const char *home = std::getenv("HOME")) {
        candidates.push_back(fs::path(home) / ".local/bin/claude");
        candidates.push_back(fs::path(home) / ".claude/local/claude");
        candidates.push_back(fs::path(home) / ".npm-global/bin/claude");
    }
    candidates.push_back("/usr/local/bin/claude");
    candidates.push_back("/opt/homebrew/bin/claude");
    candidates.push_back("/usr/bin/claude");
    for (const fs::path &c : candidates)
        if (fs::exists(c, ec))
            return c.string();
    return {};
}

SemanticPaintReply semantic_paint_query(const SemanticPaintSettings &settings, const std::string &prompt,
                                        const std::vector<std::string> &png_files, const std::atomic<bool> *cancel)
{
    return run_cli(settings, prompt, png_files, cancel);
}

} // namespace Slic3r
