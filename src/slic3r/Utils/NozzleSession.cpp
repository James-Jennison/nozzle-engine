// Nozzle It All Desktop: session bridge. See NozzleSession.hpp.
#include "NozzleSession.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace Slic3r { namespace NozzleSession {

namespace {

void set_message(std::string *message, const std::string &text)
{
    if (message != nullptr)
        *message = text;
}

} // namespace

std::string sha256_file_hex(const std::string &path)
{
    boost::nowide::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (ctx == nullptr)
        return {};
    std::string result;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) {
        char buffer[64 * 1024];
        bool ok = true;
        while (in) {
            in.read(buffer, sizeof(buffer));
            const std::streamsize n = in.gcount();
            if (n > 0 && EVP_DigestUpdate(ctx, buffer, static_cast<size_t>(n)) != 1) {
                ok = false;
                break;
            }
        }
        if (in.bad())
            ok = false;
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int  digest_len = 0;
        if (ok && EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1) {
            static const char *hex = "0123456789abcdef";
            result.reserve(digest_len * 2);
            for (unsigned int i = 0; i < digest_len; ++i) {
                result.push_back(hex[digest[i] >> 4]);
                result.push_back(hex[digest[i] & 0x0f]);
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    return result;
}

Result update_session_file(const std::string &session_path,
                           const std::string &saved_file,
                           int64_t            now_millis,
                           std::string       *message)
{
    namespace fs = boost::filesystem;
    using json   = nlohmann::ordered_json; // keeps the existing key order

    json session;
    {
        boost::nowide::ifstream in(session_path, std::ios::binary);
        if (!in) {
            set_message(message, "cannot read session file " + session_path);
            return Result::Error;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        session = json::parse(buffer.str(), nullptr, /*allow_exceptions=*/false);
    }
    if (session.is_discarded() || !session.is_object()) {
        set_message(message, "session file is not a JSON object: " + session_path);
        return Result::Error;
    }

    const auto protocol = session.find("protocol");
    if (protocol == session.end() || !protocol->is_string() || protocol->get<std::string>() != PROTOCOL) {
        set_message(message, std::string("session file is not a \"") + PROTOCOL + "\" session; left unchanged");
        return Result::Refused;
    }
    const auto version = session.find("version");
    if (version == session.end() || !version->is_array() || version->empty() || !(*version)[0].is_number_integer()) {
        set_message(message, "session file has no [major, minor] version; left unchanged");
        return Result::Refused;
    }
    const int64_t major = (*version)[0].get<int64_t>();
    if (major != SUPPORTED_MAJOR) {
        set_message(message, "session protocol major version " + std::to_string(major) + " is not supported (this build supports " +
                                 std::to_string(SUPPORTED_MAJOR) + "); left unchanged");
        return Result::Refused;
    }

    const std::string sha = sha256_file_hex(saved_file);
    if (sha.empty()) {
        set_message(message, "cannot hash saved project " + saved_file);
        return Result::Error;
    }

    session["state"]         = "saved";
    session["savedAtMillis"] = now_millis;
    session["savedSha256"]   = sha;

    // Write next to the target and rename, so a reader never sees a partial file.
    const fs::path target(session_path);
    fs::path       tmp = target;
    tmp += ".tmp-" + std::to_string(now_millis);
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out) {
            set_message(message, "cannot write " + tmp.string());
            return Result::Error;
        }
        out << session.dump(2) << "\n";
        out.flush();
        if (!out) {
            set_message(message, "failed writing " + tmp.string());
            boost::system::error_code ec;
            fs::remove(tmp, ec);
            return Result::Error;
        }
    }
    boost::system::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        set_message(message, "cannot replace session file: " + ec.message());
        fs::remove(tmp, ec);
        return Result::Error;
    }
    set_message(message, "session updated: state=saved, savedSha256=" + sha);
    return Result::Updated;
}

Result on_project_saved(const std::string &saved_file)
{
    const char *env = boost::nowide::getenv(ENV_VAR);
    if (env == nullptr || *env == '\0')
        return Result::NoSession;

    const int64_t now = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    std::string message;
    Result      result = Result::Error;
    try {
        result = update_session_file(env, saved_file, now, &message);
    } catch (const std::exception &e) {
        message = e.what();
        result  = Result::Error;
    }
    switch (result) {
    case Result::Updated: BOOST_LOG_TRIVIAL(info) << "[NozzleSession] " << message; break;
    case Result::Refused: BOOST_LOG_TRIVIAL(warning) << "[NozzleSession] " << message; break;
    default: BOOST_LOG_TRIVIAL(error) << "[NozzleSession] " << message; break;
    }
    return result;
}

}} // namespace Slic3r::NozzleSession
