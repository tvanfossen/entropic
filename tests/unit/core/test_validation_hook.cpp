/**
 * @file test_validation_hook.cpp
 * @brief Validation hook integration tests — registration, dispatch, lifecycle.
 * @version 1.9.8
 */

#include <entropic/core/constitutional_validator.h>
#include <entropic/core/hook_registry.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace entropic;

// ── Mock Inference for Hook Tests ─────────────────────────

namespace {

/**
 * @brief Sequenced mock inference for hook tests.
 * @internal
 * @version 1.9.8
 */
struct HookMockInference {
    std::vector<std::string> responses;
    int call_index = 0;
    int generate_count = 0;
};

/**
 * @brief Allocate a C string copy.
 * @internal
 * @version 1.9.8
 */
char* hook_mock_strdup(const std::string& s) {
    auto* p = new char[s.size() + 1];
    std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

/**
 * @brief Mock free.
 * @internal
 * @version 1.9.8
 */
void hook_mock_free(void* ptr) {
    delete[] static_cast<char*>(ptr);
}

/**
 * @brief Mock generate for hook tests.
 * @callback
 * @version 1.9.8
 */
int hook_mock_generate(
    const char* /*messages_json*/,
    const char* /*params_json*/,
    char** result_json,
    void* user_data) {
    auto* mock = static_cast<HookMockInference*>(user_data);
    mock->generate_count++;

    if (mock->call_index < static_cast<int>(mock->responses.size())) {
        *result_json = hook_mock_strdup(
            mock->responses[mock->call_index]);
        mock->call_index++;
    } else {
        *result_json = hook_mock_strdup(
            R"({"compliant":true,"violations":[],"revised":""})");
    }
    return 0;
}

/**
 * @brief Build InferenceInterface for hook mock.
 * @internal
 * @version 1.9.8
 */
InferenceInterface make_hook_iface(HookMockInference& mock) {
    InferenceInterface iface;
    iface.generate = hook_mock_generate;
    iface.free_fn = hook_mock_free;
    iface.backend_data = &mock;
    return iface;
}

/**
 * @brief Build a HookInterface pointing to a real HookRegistry.
 * @param reg HookRegistry instance.
 * @return Wired HookInterface.
 * @internal
 * @version 1.9.8
 */
HookInterface make_hook_interface(HookRegistry& reg) {
    HookInterface hi;
    hi.fire_pre = [](void* r, entropic_hook_point_t p,
                     const char* ctx, char** out) -> int {
        return static_cast<HookRegistry*>(r)->fire_pre(p, ctx, out);
    };
    hi.fire_post = [](void* r, entropic_hook_point_t p,
                      const char* ctx, char** out) {
        static_cast<HookRegistry*>(r)->fire_post(p, ctx, out);
    };
    hi.fire_info = [](void* r, entropic_hook_point_t p,
                      const char* ctx) {
        static_cast<HookRegistry*>(r)->fire_info(p, ctx);
    };
    hi.registry = &reg;
    return hi;
}

} // anonymous namespace

// ── Tests ────────────────────────────────────────────────

SCENARIO("Hook registered at correct priority",
         "[validation][hooks]") {
    GIVEN("a ConstitutionalValidator with priority 100") {
        HookRegistry reg;
        HookMockInference mock;
        auto iface = make_hook_iface(mock);
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.priority = 100;
        ConstitutionalValidator validator(cfg, "Constitution");

        WHEN("attach is called") {
            auto err = validator.attach(&hi, &iface);

            THEN("it returns ENTROPIC_OK") {
                REQUIRE(err == ENTROPIC_OK);
            }
            THEN("POST_GENERATE has one hook registered") {
                REQUIRE(reg.hook_count(
                    ENTROPIC_HOOK_POST_GENERATE) == 1);
            }
        }
    }
}

SCENARIO("Detach removes hook", "[validation][hooks]") {
    GIVEN("a registered validation hook") {
        HookRegistry reg;
        HookMockInference mock;
        auto iface = make_hook_iface(mock);
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, "Constitution");
        validator.attach(&hi, &iface);
        REQUIRE(reg.hook_count(ENTROPIC_HOOK_POST_GENERATE) == 1);

        WHEN("detach is called") {
            validator.detach(&hi);

            THEN("the hook is deregistered") {
                REQUIRE(reg.hook_count(
                    ENTROPIC_HOOK_POST_GENERATE) == 0);
            }
        }
    }
}

SCENARIO("Hook passes through when compliant",
         "[validation][hooks]") {
    GIVEN("a registered hook with compliant critique") {
        HookRegistry reg;
        HookMockInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_hook_iface(mock);
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, "Constitution");
        validator.attach(&hi, &iface);

        WHEN("fire_post is called with content") {
            char* out = nullptr;
            reg.fire_post(
                ENTROPIC_HOOK_POST_GENERATE,
                R"({"content":"Safe text","tier":"eng"})",
                &out);

            THEN("out_json is NULL (no modification)") {
                REQUIRE(out == nullptr);
            }
        }
    }
}

SCENARIO("Hook transforms content when violations found",
         "[validation][hooks]") {
    GIVEN("a hook with non-compliant then compliant critiques") {
        HookRegistry reg;
        HookMockInference mock;
        // First call: critique returns non-compliant with revision
        mock.responses.push_back(
            R"({"compliant":false,"violations":[)"
            R"({"rule":"Privacy","excerpt":"cloud",)"
            R"("explanation":"external"}],)"
            R"("revised":"Fixed text"})");
        // Second call: verify revision is compliant
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_hook_iface(mock);
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        cfg.max_revisions = 2;
        ConstitutionalValidator validator(cfg, "Constitution");
        validator.attach(&hi, &iface);

        WHEN("fire_post is called with violating content") {
            char* out = nullptr;
            reg.fire_post(
                ENTROPIC_HOOK_POST_GENERATE,
                R"({"content":"Upload to cloud","tier":"eng"})",
                &out);

            THEN("out_json contains revised content") {
                REQUIRE(out != nullptr);
                std::string result(out);
                REQUIRE(result.find("Fixed text")
                        != std::string::npos);
                free(out);
            }
        }
    }
}

SCENARIO("Identity opt-out skips validation in hook",
         "[validation][hooks]") {
    GIVEN("validation false for identity eng") {
        HookRegistry reg;
        HookMockInference mock;
        auto iface = make_hook_iface(mock);
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, "Constitution");
        validator.set_identity_validation("eng", false);
        validator.attach(&hi, &iface);

        WHEN("fire_post is called with tier eng") {
            char* out = nullptr;
            reg.fire_post(
                ENTROPIC_HOOK_POST_GENERATE,
                R"({"content":"Any text","tier":"eng"})",
                &out);

            THEN("no critique generation occurs") {
                REQUIRE(mock.generate_count == 0);
            }
            THEN("out_json is NULL (original preserved)") {
                REQUIRE(out == nullptr);
            }
        }
    }
}

SCENARIO("Last result populated after hook validation",
         "[validation][hooks]") {
    GIVEN("a registered validation hook") {
        HookRegistry reg;
        HookMockInference mock;
        mock.responses.push_back(
            R"({"compliant":true,"violations":[],"revised":""})");
        auto iface = make_hook_iface(mock);
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, "Constitution");
        validator.attach(&hi, &iface);

        WHEN("fire_post triggers validation") {
            char* out = nullptr;
            reg.fire_post(
                ENTROPIC_HOOK_POST_GENERATE,
                R"({"content":"Test","tier":"eng"})",
                &out);
            free(out);

            THEN("last_result reflects the validation") {
                auto last = validator.last_result();
                REQUIRE(last.content == "Test");
                REQUIRE(last.final_critique.compliant == true);
            }
        }
    }
}

SCENARIO("Critique failure in hook returns original",
         "[validation][hooks]") {
    GIVEN("a hook with mock inference that fails") {
        HookRegistry reg;
        HookMockInference mock;
        // Empty responses + generate will return compliant default
        // But let's make generate fail by using a failing mock
        auto iface = make_hook_iface(mock);
        iface.generate = [](const char*, const char*,
                            char** result_json,
                            void*) -> int {
            *result_json = nullptr;
            return -1;
        };
        auto hi = make_hook_interface(reg);

        ConstitutionalValidationConfig cfg;
        cfg.enabled = true;
        ConstitutionalValidator validator(cfg, "Constitution");
        validator.attach(&hi, &iface);

        WHEN("fire_post is called") {
            char* out = nullptr;
            reg.fire_post(
                ENTROPIC_HOOK_POST_GENERATE,
                R"({"content":"Original","tier":"eng"})",
                &out);

            THEN("out_json is NULL (original content preserved)") {
                REQUIRE(out == nullptr);
            }
        }
    }
}

SCENARIO("Attach with null hook interface returns error",
         "[validation][hooks]") {
    GIVEN("a validator") {
        HookMockInference mock;
        auto iface = make_hook_iface(mock);

        ConstitutionalValidationConfig cfg;
        ConstitutionalValidator validator(cfg, "Constitution");

        WHEN("attach is called with nullptr") {
            auto err = validator.attach(nullptr, &iface);

            THEN("returns ENTROPIC_ERROR_INVALID_ARGUMENT") {
                REQUIRE(err == ENTROPIC_ERROR_INVALID_ARGUMENT);
            }
        }
    }
}
