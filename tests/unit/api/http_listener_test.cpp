// SPDX-License-Identifier: Apache-2.0
/**
 * @file http_listener_test.cpp
 * @brief gh#204 (v2.14.0): the Streamable-HTTP front end and its security bar.
 *
 * Live tests bind 127.0.0.1 on a free port and talk to it with an HTTP
 * client, reading the bearer token FROM THE TOKEN FILE — the contract a
 * plugin uses. HOME is pointed at a scratch directory so nothing here
 * touches the developer's real ~/.entropic.
 *
 * @version 2.14.0
 */
#include <catch2/catch_test_macros.hpp>

#include <entropic/entropic.h>
#include <entropic/mcp/external_bridge.h>

#include "http_listener.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using entropic::http::AuthThrottle;
using entropic::http::HttpListener;
using json = nlohmann::json;

namespace {

/// @brief Points HOME at a scratch directory for one scope.
class ScratchHome {
public:
    ScratchHome() {
        dir_ = fs::temp_directory_path()
            / ("entropic_http_home_" + std::to_string(::getpid()));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        const char* old = std::getenv("HOME");
        if (old != nullptr) { saved_ = old; }
        setenv("HOME", dir_.c_str(), 1);
    }
    ~ScratchHome() {
        setenv("HOME", saved_.c_str(), 1);
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    ScratchHome(const ScratchHome&) = delete;
    ScratchHome& operator=(const ScratchHome&) = delete;
    const fs::path& dir() const { return dir_; }

private:
    fs::path dir_;
    std::string saved_;
};

/// @brief The file's full text.
std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// @brief A JSON-RPC request body.
std::string rpc(int id, const std::string& method,
                const json& params = json::object()) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method},
                {"params", params}}.dump();
}

/// @brief Headers carrying a bearer token.
httplib::Headers auth(const std::string& token) {
    return {{"Authorization", "Bearer " + token}};
}

}  // namespace

// ── Pure security helpers ────────────────────────────────────

SCENARIO("gh#204: the bearer token is 256 random bits, compared in constant time",
         "[http][security][gh204][2.14.0]") {
    const auto a = entropic::http::generate_token();
    const auto b = entropic::http::generate_token();
    THEN("each is 64 hex characters and two are never the same") {
        CHECK(a.size() == 64);
        CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
        CHECK(a != b);
    }
    THEN("comparison accepts only an exact match") {
        CHECK(entropic::http::tokens_equal(a, a));
        CHECK_FALSE(entropic::http::tokens_equal(a, b));
        CHECK_FALSE(entropic::http::tokens_equal(a, a.substr(0, 63)));
        CHECK_FALSE(entropic::http::tokens_equal("", ""));
    }
    THEN("only a Bearer scheme yields a token") {
        CHECK(entropic::http::bearer_token("Bearer xyz") == "xyz");
        CHECK(entropic::http::bearer_token("Basic xyz").empty());
        CHECK(entropic::http::bearer_token("").empty());
    }
}

SCENARIO("gh#204: only this loopback endpoint is an acceptable Host",
         "[http][security][gh204][2.14.0]") {
    CHECK(entropic::http::host_allowed("127.0.0.1:7878", 7878));
    CHECK(entropic::http::host_allowed("localhost:7878", 7878));
    CHECK_FALSE(entropic::http::host_allowed("127.0.0.1:7879", 7878));
    CHECK_FALSE(entropic::http::host_allowed("evil.example:7878", 7878));
    CHECK_FALSE(entropic::http::host_allowed("", 7878));
}

SCENARIO("gh#204: repeated authentication failures block for a while",
         "[http][security][gh204][2.14.0]") {
    AuthThrottle t;
    const auto t0 = AuthThrottle::Clock::now();
    for (int i = 0; i < 4; ++i) { t.record_failure(t0); }
    THEN("four failures do not block") { CHECK_FALSE(t.blocked(t0)); }
    t.record_failure(t0);
    THEN("the fifth blocks, and the block lifts after it expires") {
        CHECK(t.blocked(t0 + std::chrono::seconds(1)));
        CHECK_FALSE(t.blocked(t0 + std::chrono::seconds(31)));
    }
}

SCENARIO("gh#204: the token lives in a private, owned directory",
         "[http][security][gh204][2.14.0]") {
    ScratchHome home;
    GIVEN("a fresh directory") {
        const auto dir = home.dir() / "serve";
        std::string err;
        REQUIRE(entropic::http::prepare_private_dir(dir, err));
        THEN("it is created mode 0700") {
            struct stat st{};
            REQUIRE(::stat(dir.c_str(), &st) == 0);
            CHECK((st.st_mode & 0777) == 0700);
        }
        AND_WHEN("a token is written into it") {
            REQUIRE(entropic::http::write_token_file(dir / "1.token", "abc", err));
            THEN("the file holds exactly the token, mode 0600") {
                struct stat st{};
                REQUIRE(::stat((dir / "1.token").c_str(), &st) == 0);
                CHECK((st.st_mode & 0777) == 0600);
                CHECK(slurp(dir / "1.token") == "abc");
            }
        }
    }
    GIVEN("a symlink where the directory should be") {
        const auto target = home.dir() / "elsewhere";
        fs::create_directories(target);
        const auto link = home.dir() / "serve-link";
        fs::create_directory_symlink(target, link);
        std::string err;
        THEN("it is refused — a token must not be written through a link") {
            CHECK_FALSE(entropic::http::prepare_private_dir(link, err));
            CHECK_FALSE(err.empty());
        }
    }
}

// ── Live listener ────────────────────────────────────────────

SCENARIO("gh#204: the HTTP listener serves the bridge to authenticated clients only",
         "[http][gh204][2.14.0]") {
    ScratchHome home;
    entropic::ExternalMCPConfig cfg;
    entropic::ExternalBridge bridge(nullptr, cfg, home.dir() / "unused.sock");
    HttpListener listener(&bridge, 0);
    std::string err;
    REQUIRE(listener.start(err));
    INFO("start: " << err);
    const int port = listener.port();
    REQUIRE(port > 0);
    const std::string token = slurp(listener.token_file());
    httplib::Client cli("127.0.0.1", port);
    cli.set_read_timeout(10, 0);

    THEN("the token file is where a plugin looks for it") {
        CHECK(listener.token_file() == entropic::http::token_path_for(port));
        CHECK(token.size() == 64);
    }
    WHEN("a request carries no token") {
        auto r = cli.Post("/mcp", rpc(1, "tools/list"), "application/json");
        THEN("it is refused 401") { REQUIRE(r); CHECK(r->status == 401); }
    }
    WHEN("a request carries the wrong token") {
        auto r = cli.Post("/mcp", auth(std::string(64, '0')),
                          rpc(1, "tools/list"), "application/json");
        THEN("it is refused 401") { REQUIRE(r); CHECK(r->status == 401); }
    }
    WHEN("a request carries an Origin header, even with the right token") {
        auto h = auth(token);
        h.emplace("Origin", "http://evil.example");
        auto r = cli.Post("/mcp", h, rpc(1, "tools/list"), "application/json");
        THEN("it is refused 403 — a browser page never drives the engine") {
            REQUIRE(r);
            CHECK(r->status == 403);
        }
    }
    WHEN("a request names a foreign Host") {
        auto h = auth(token);
        h.emplace("Host", "evil.example:" + std::to_string(port));
        auto r = cli.Post("/mcp", h, rpc(1, "tools/list"), "application/json");
        THEN("it is refused 403 (DNS rebinding)") {
            REQUIRE(r);
            CHECK(r->status == 403);
        }
    }
    WHEN("an authenticated client lists tools") {
        auto r = cli.Post("/mcp", auth(token), rpc(1, "tools/list"),
                          "application/json");
        THEN("it gets the same tools the unix socket serves, as JSON") {
            REQUIRE(r);
            REQUIRE(r->status == 200);
            auto j = json::parse(r->body);
            bool has_ask = false;
            for (const auto& t : j["result"]["tools"]) {
                has_ask = has_ask || t["name"] == "entropic.ask";
            }
            CHECK(has_ask);
        }
    }
    WHEN("an authenticated client sends a notification") {
        auto r = cli.Post("/mcp", auth(token),
                          R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                          "application/json");
        THEN("it is accepted with 202 and no body") {
            REQUIRE(r);
            CHECK(r->status == 202);
        }
    }
    WHEN("an authenticated client calls a tool") {
        auto r = cli.Post(
            "/mcp", auth(token),
            rpc(7, "tools/call",
                {{"name", "entropic.ask_status"},
                 {"arguments", {{"task_id", "no-such-task"}}}}),
            "application/json");
        THEN("the result arrives as an SSE event carrying the JSON-RPC reply") {
            REQUIRE(r);
            REQUIRE(r->status == 200);
            CHECK(r->get_header_value("Content-Type")
                      .find("text/event-stream") != std::string::npos);
            const auto at = r->body.find("data: ");
            REQUIRE(at != std::string::npos);
            const auto line = r->body.substr(at + 6, r->body.find('\n', at) - at - 6);
            auto j = json::parse(line);
            CHECK(j["id"] == 7);
            CHECK(j.contains("result"));
        }
    }
    WHEN("a client tries to open a server stream") {
        auto r = cli.Get("/mcp", auth(token));
        THEN("it is told the method is not allowed") {
            REQUIRE(r);
            CHECK(r->status == 405);
        }
    }
    WHEN("five bad tokens arrive in a row") {
        for (int i = 0; i < 5; ++i) {
            (void)cli.Post("/mcp", auth(std::string(64, '1')),
                           rpc(1, "tools/list"), "application/json");
        }
        auto r = cli.Post("/mcp", auth(token), rpc(1, "tools/list"),
                          "application/json");
        THEN("even the right token is refused 429 until the block lifts") {
            REQUIRE(r);
            CHECK(r->status == 429);
        }
    }
    const auto token_file = listener.token_file();
    listener.stop();
    THEN("stopping removes the token file") {
        CHECK_FALSE(fs::exists(token_file));
    }
}

SCENARIO("gh#204: a listener stopped the instant it started still stops",
         "[http][gh204][2.14.0]") {
    // httplib's stop() is a no-op until its listen loop is running. A stop()
    // landing in that window left the loop running and the join hung — seen
    // as this file's live test hanging only under ctest's parallel load.
    // start() now returns only once the server is accepting.
    ScratchHome home;
    entropic::ExternalMCPConfig cfg;
    entropic::ExternalBridge bridge(nullptr, cfg, home.dir() / "unused.sock");
    THEN("two hundred immediate start/stop cycles all complete") {
        int completed = 0;
        for (int i = 0; i < 200; ++i) {
            HttpListener l(&bridge, 0);
            std::string err;
            REQUIRE(l.start(err));
            l.stop();
            ++completed;
        }
        CHECK(completed == 200);
    }
}

SCENARIO("gh#204: an unservable http_port fails configure loudly",
         "[http][api][gh204][2.14.0]") {
    ScratchHome home;
    entropic_handle_t h = nullptr;
    REQUIRE(entropic_create(&h) == ENTROPIC_OK);
    const auto rc = entropic_configure(h,
        R"({"log_level":"WARN","mcp":{"external":{"enabled":true,"http_port":70000}}})");
    THEN("configure reports an I/O error naming the listener") {
        CHECK(rc == ENTROPIC_ERROR_IO);
        const char* e = entropic_last_error(h);
        REQUIRE(e != nullptr);
        CHECK(std::string(e).find("HTTP listener") != std::string::npos);
    }
    entropic_destroy(h);
}
