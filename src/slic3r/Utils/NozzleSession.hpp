// Nozzle It All Desktop: session bridge.
//
// When the Nozzle It All app launches the workspace it may set the environment
// variable NOZZLE_WORKSPACE_SESSION to the path of a session.json file:
//
//   {"protocol":"nozzle-workspace","version":[1,0], ...}
//
// After every successful project save the workspace rewrites that file, keeping
// every existing field, and sets "state":"saved", "savedAtMillis" (Unix epoch
// milliseconds) and "savedSha256" (lowercase hex SHA-256 of the saved file).
// A file with another protocol or a major version other than 1 is left alone
// and the refusal is logged.
#ifndef slic3r_Utils_NozzleSession_hpp_
#define slic3r_Utils_NozzleSession_hpp_

#include <cstdint>
#include <string>

namespace Slic3r { namespace NozzleSession {

constexpr const char *ENV_VAR        = "NOZZLE_WORKSPACE_SESSION";
constexpr const char *PROTOCOL       = "nozzle-workspace";
constexpr int         SUPPORTED_MAJOR = 1;

enum class Result {
    NoSession, // environment variable not set or empty
    Updated,   // session file rewritten
    Refused,   // not a nozzle-workspace session, or unsupported major version
    Error      // I/O or parse failure
};

// Lowercase hex SHA-256 of a file; empty string on failure.
std::string sha256_file_hex(const std::string &path);

// Update `session_path` for a save of `saved_file` at `now_millis`.
// `message` (optional) receives a human-readable explanation.
Result update_session_file(const std::string &session_path,
                           const std::string &saved_file,
                           int64_t            now_millis,
                           std::string       *message = nullptr);

// Reads NOZZLE_WORKSPACE_SESSION and calls update_session_file() with the
// current time. Never throws; all outcomes are logged.
Result on_project_saved(const std::string &saved_file);

}} // namespace Slic3r::NozzleSession

#endif
