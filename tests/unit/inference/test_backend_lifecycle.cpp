/**
 * @file test_backend_lifecycle.cpp
 * @brief Tests for InferenceBackend state machine.
 *
 * Uses a mock subclass that tracks do_* calls without loading a real model.
 *
 * @version 1.8.2
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/backend.h>

namespace {

/**
 * @brief Mock backend that tracks lifecycle calls.
 * @version 1.8.2
 */
class MockBackend : public entropic::InferenceBackend {
public:
    int load_calls = 0;
    int activate_calls = 0;
    int deactivate_calls = 0;
    int unload_calls = 0;
    int generate_calls = 0;
    int complete_calls = 0;
    bool fail_load = false;
    bool fail_activate = false;

protected:
    bool do_load(const entropic::ModelConfig& /*config*/) override {
        ++load_calls;
        if (fail_load) {
            last_error_ = "mock load failure";
            return false;
        }
        return true;
    }

    bool do_activate() override {
        ++activate_calls;
        if (fail_activate) {
            last_error_ = "mock activate failure";
            return false;
        }
        return true;
    }

    void do_deactivate() override { ++deactivate_calls; }
    void do_unload() override { ++unload_calls; }

    entropic::GenerationResult do_generate(
        const std::vector<entropic::Message>& /*messages*/,
        const entropic::GenerationParams& /*params*/) override
    {
        ++generate_calls;
        entropic::GenerationResult r;
        r.content = "mock response";
        r.token_count = 2;
        return r;
    }

    entropic::GenerationResult do_generate_streaming(
        const std::vector<entropic::Message>& /*messages*/,
        const entropic::GenerationParams& /*params*/,
        std::function<void(std::string_view)> on_token,
        std::atomic<bool>& /*cancel*/) override
    {
        entropic::GenerationResult r;
        r.content = "streamed";
        if (on_token) on_token("streamed");
        r.token_count = 1;
        return r;
    }

    entropic::GenerationResult do_complete(
        const std::string& /*prompt*/,
        const entropic::GenerationParams& /*params*/) override
    {
        ++complete_calls;
        entropic::GenerationResult r;
        r.content = "2";
        r.token_count = 1;
        return r;
    }

    int do_count_tokens(const std::string& text) const override {
        return static_cast<int>(text.size()) / 4;
    }
};

entropic::ModelConfig make_config() {
    entropic::ModelConfig cfg;
    cfg.path = "/tmp/test.gguf";
    cfg.context_length = 4096;
    cfg.gpu_layers = -1;
    return cfg;
}

} // anonymous namespace

// ── Lifecycle transitions ──────────────────────────────────

SCENARIO("Backend lifecycle transitions", "[backend][lifecycle]") {
    GIVEN("a COLD backend") {
        MockBackend backend;
        REQUIRE(backend.state() == entropic::ModelState::COLD);

        WHEN("load() is called") {
            bool ok = backend.load(make_config());
            THEN("state is WARM and do_load was called") {
                REQUIRE(ok);
                REQUIRE(backend.state() == entropic::ModelState::WARM);
                REQUIRE(backend.load_calls == 1);
            }
        }
    }

    GIVEN("a WARM backend") {
        MockBackend backend;
        backend.load(make_config());

        WHEN("activate() is called") {
            bool ok = backend.activate();
            THEN("state is ACTIVE") {
                REQUIRE(ok);
                REQUIRE(backend.state() == entropic::ModelState::ACTIVE);
                REQUIRE(backend.activate_calls == 1);
            }
        }
    }

    GIVEN("an ACTIVE backend") {
        MockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("deactivate() is called") {
            backend.deactivate();
            THEN("state is WARM") {
                REQUIRE(backend.state() == entropic::ModelState::WARM);
                REQUIRE(backend.deactivate_calls == 1);
            }
        }

        WHEN("unload() is called") {
            backend.unload();
            THEN("state is COLD") {
                REQUIRE(backend.state() == entropic::ModelState::COLD);
                REQUIRE(backend.unload_calls == 1);
            }
        }
    }
}

// ── Idempotent transitions ─────────────────────────────────

SCENARIO("Idempotent transitions", "[backend][lifecycle]") {
    GIVEN("a WARM backend") {
        MockBackend backend;
        backend.load(make_config());

        WHEN("load() is called again") {
            backend.load(make_config());
            THEN("do_load is NOT called again") {
                REQUIRE(backend.load_calls == 1);
                REQUIRE(backend.state() == entropic::ModelState::WARM);
            }
        }
    }

    GIVEN("an ACTIVE backend") {
        MockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("activate() is called again") {
            backend.activate();
            THEN("do_activate is NOT called again") {
                REQUIRE(backend.activate_calls == 1);
            }
        }
    }

    GIVEN("a COLD backend") {
        MockBackend backend;

        WHEN("deactivate() is called") {
            backend.deactivate();
            THEN("do_deactivate is NOT called") {
                REQUIRE(backend.deactivate_calls == 0);
                REQUIRE(backend.state() == entropic::ModelState::COLD);
            }
        }
    }
}

// ── Generation requires ACTIVE ─────────────────────────────

SCENARIO("Generation requires ACTIVE state", "[backend][generate]") {
    GIVEN("a WARM backend") {
        MockBackend backend;
        backend.load(make_config());

        WHEN("generate() is called") {
            auto result = backend.generate({}, {});
            THEN("error result is returned") {
                REQUIRE_FALSE(result.ok());
                REQUIRE(result.error_code == ENTROPIC_ERROR_INVALID_STATE);
                REQUIRE(backend.generate_calls == 0);
            }
        }
    }

    GIVEN("an ACTIVE backend") {
        MockBackend backend;
        backend.load_and_activate(make_config());

        WHEN("generate() is called") {
            auto result = backend.generate({}, {});
            THEN("do_generate is called") {
                REQUIRE(result.ok());
                REQUIRE(result.content == "mock response");
                REQUIRE(backend.generate_calls == 1);
            }
        }

        WHEN("complete() is called") {
            auto result = backend.complete("test prompt", {});
            THEN("do_complete is called") {
                REQUIRE(result.ok());
                REQUIRE(result.content == "2");
                REQUIRE(backend.complete_calls == 1);
            }
        }
    }
}

// ── Convenience: load_and_activate from COLD ───────────────

SCENARIO("load_and_activate from COLD", "[backend][lifecycle]") {
    GIVEN("a COLD backend") {
        MockBackend backend;

        WHEN("load_and_activate() is called") {
            bool ok = backend.load_and_activate(make_config());
            THEN("both load and activate are called") {
                REQUIRE(ok);
                REQUIRE(backend.state() == entropic::ModelState::ACTIVE);
                REQUIRE(backend.load_calls == 1);
                REQUIRE(backend.activate_calls == 1);
            }
        }
    }
}

// ── Load failure ───────────────────────────────────────────

SCENARIO("Load failure keeps COLD state", "[backend][lifecycle]") {
    GIVEN("a backend that fails to load") {
        MockBackend backend;
        backend.fail_load = true;

        WHEN("load() is called") {
            bool ok = backend.load(make_config());
            THEN("returns false and stays COLD") {
                REQUIRE_FALSE(ok);
                REQUIRE(backend.state() == entropic::ModelState::COLD);
            }
        }
    }
}
