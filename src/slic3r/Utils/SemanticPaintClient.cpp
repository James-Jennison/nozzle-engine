#include "SemanticPaintClient.hpp"

#include "Http.hpp"
#include "libslic3r/SemanticPaint.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#include <wx/secretstore.h>
#include <wx/string.h>

namespace fs      = boost::filesystem;
namespace process = boost::process;

namespace Slic3r {

const char *const k_semantic_paint_default_command = "claude -p --output-format json --allowedTools Read --add-dir {dir} --no-session-persistence";

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

std::string quote(const std::string &s)
{
#ifdef _WIN32
    return "\"" + s + "\"";
#else
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
#endif
}

SemanticPaintReply run_command(const SemanticPaintSettings &s, const std::string &prompt, const std::vector<std::string> &png_files,
                               const std::atomic<bool> *cancel)
{
    SemanticPaintReply out;
    if (s.command.find_first_not_of(" \t") == std::string::npos) {
        out.error = "No command set.";
        return out;
    }
    TempDir tmp;
    std::vector<std::string> paths;
    for (size_t i = 0; i < png_files.size(); ++i) {
        const fs::path p = tmp.path / ("view" + std::to_string(i + 1) + ".png");
        std::ofstream(p.string(), std::ios::binary).write(png_files[i].data(), std::streamsize(png_files[i].size()));
        paths.push_back(p.string());
    }
    const fs::path in_file = tmp.path / "prompt.txt", out_file = tmp.path / "reply.txt", err_file = tmp.path / "err.txt";
    {
        std::ofstream in(in_file.string(), std::ios::binary);
        in << SemanticPaint::command_prompt(prompt, paths);
    }

    std::string cmd = s.command;
    // A GUI app often starts without the user's PATH additions; fall back to the usual Claude Code install folders.
    if (cmd.compare(0, 7, "claude ") == 0 && process::search_path("claude").empty()) {
        const std::string found = semantic_paint_find_claude();
        if (!found.empty())
            cmd = quote(found) + cmd.substr(6);
    }
    boost::replace_all(cmd, "{dir}", quote(tmp.path.string()));
    boost::replace_all(cmd, "{model}", s.model);
    if (!s.model.empty() && s.command.find("{model}") == std::string::npos && cmd.compare(0, 7, "claude ") == 0)
        cmd += " --model " + quote(s.model);

    try {
#ifdef _WIN32
        const char *comspec = std::getenv("COMSPEC");
        const std::string shell = comspec ? comspec : "cmd.exe";
        process::child child(shell, process::args({"/c", cmd}), process::std_in < in_file, process::std_out > out_file,
                             process::std_err > err_file, process::start_dir(tmp.path.string()));
#else
        process::child child(process::search_path("sh"), process::args({"-c", cmd}), process::std_in < in_file, process::std_out > out_file,
                             process::std_err > err_file, process::start_dir(tmp.path.string()));
#endif
        const auto start = std::chrono::steady_clock::now();
        while (child.running()) {
            if (cancel && cancel->load()) {
                child.terminate();
                out.error = "Cancelled.";
                return out;
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(kTimeoutSeconds)) {
                child.terminate();
                out.error = "The command did not answer in time.";
                return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        child.wait();
        const std::string body = read_file(out_file);
        out.text = SemanticPaint::command_reply_text(body);
        if (child.exit_code() != 0 || out.text.find_first_not_of(" \t\r\n") == std::string::npos) {
            const std::string err = read_file(err_file);
            out.error = "The command returned no answer (exit " + std::to_string(child.exit_code()) + ")." +
                        (err.empty() ? std::string() : " " + err.substr(0, 400));
            if (err.empty() && !body.empty())
                out.error += " " + body.substr(0, 400);
            out.text.clear();
            return out;
        }
        out.ok = true;
    } catch (const std::exception &e) {
        out.error = std::string("Could not run the command: ") + e.what();
    }
    return out;
}

SemanticPaintReply run_openai(const SemanticPaintSettings &s, const std::string &prompt, const std::vector<std::string> &png_files)
{
    SemanticPaintReply out;
    if (s.model.empty()) {
        out.error = "No model set.";
        return out;
    }
    std::vector<std::string> b64;
    for (const std::string &p : png_files)
        b64.push_back(SemanticPaint::base64_encode(p));
    std::string base = s.endpoint.empty() ? "http://localhost:11434/v1" : s.endpoint;
    while (!base.empty() && base.back() == '/')
        base.pop_back();

    Http http = Http::post(base + "/chat/completions");
    http.header("Content-Type", "application/json").timeout_connect(15).timeout_max(kTimeoutSeconds);
    if (!s.api_key.empty())
        http.header("Authorization", "Bearer " + s.api_key);
    bool        failed = false;
    std::string reply;
    http.set_post_body(SemanticPaint::openai_request_json(s.model, prompt, b64))
        .on_complete([&](std::string resp, unsigned) { reply = std::move(resp); })
        .on_error([&](std::string resp, std::string err, unsigned status) {
            failed    = true;
            out.error = err + " (HTTP " + std::to_string(status) + ")" + (resp.empty() ? std::string() : ": " + resp.substr(0, 400));
        })
        .perform_sync();
    if (failed)
        return out;
    out.text = SemanticPaint::openai_reply_text(reply);
    if (out.text.empty()) {
        out.error = "The server returned no text.";
        return out;
    }
    out.ok = true;
    return out;
}

constexpr const char *kSecretService = "OrcaSlicer/SemanticPaint";
constexpr const char *kSecretUser    = "openai-compatible-api-key";

} // namespace

std::string semantic_paint_find_claude()
{
    std::vector<fs::path> candidates;
    if (const char *home = std::getenv("HOME")) {
        candidates.push_back(fs::path(home) / ".local/bin/claude");
        candidates.push_back(fs::path(home) / ".claude/local/claude");
        candidates.push_back(fs::path(home) / ".npm-global/bin/claude");
    }
    candidates.push_back("/usr/local/bin/claude");
    candidates.push_back("/opt/homebrew/bin/claude");
    candidates.push_back("/usr/bin/claude");
    boost::system::error_code ec;
    for (const fs::path &c : candidates)
        if (fs::exists(c, ec))
            return c.string();
    return {};
}

SemanticPaintReply semantic_paint_query(const SemanticPaintSettings &settings, const std::string &prompt,
                                        const std::vector<std::string> &png_files, const std::atomic<bool> *cancel)
{
    if (settings.provider == "command")
        return run_command(settings, prompt, png_files, cancel);
    if (settings.provider == "openai")
        return run_openai(settings, prompt, png_files);
    SemanticPaintReply r;
    r.error = "Unknown provider: " + settings.provider;
    return r;
}

std::string semantic_paint_load_api_key()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return {};
    wxString      user;
    wxSecretValue value;
    if (!store.Load(kSecretService, user, value))
        return {};
    return std::string(value.GetAsString().utf8_str());
}

bool semantic_paint_save_api_key(const std::string &key)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return false;
    if (key.empty())
        return store.Delete(kSecretService);
    return store.Save(kSecretService, kSecretUser, wxSecretValue(wxString::FromUTF8(key.c_str())));
}

} // namespace Slic3r
