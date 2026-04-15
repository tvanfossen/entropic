// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file test_profile_registry.cpp
 * @brief Tests for ProfileRegistry -- named GPU resource profile management.
 *
 * Tests cover: load_bundled, register, deregister, get, has, list, size,
 * fallback on unknown name, bundled profile values, thread safety.
 *
 * @version 1.9.7
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/inference/profile_registry.h>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

using entropic::GPUResourceProfile;
using entropic::ProfileRegistry;

// ── Load bundled ─────────────────────────────────────────────

SCENARIO("Load bundled profiles", "[profile_registry]") {
    ProfileRegistry registry;

    WHEN("load_bundled() is called") {
        registry.load_bundled();

        THEN("four bundled profiles are registered") {
            REQUIRE(registry.size() >= 4);
            REQUIRE(registry.has("maximum"));
            REQUIRE(registry.has("balanced"));
            REQUIRE(registry.has("background"));
            REQUIRE(registry.has("minimal"));
        }
    }
}

// ── Bundled profile values ───────────────────────────────────

SCENARIO("Bundled profiles have correct values", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    WHEN("maximum profile is retrieved") {
        auto p = registry.get("maximum");

        THEN("it has expected batch and thread settings") {
            REQUIRE(p.name == "maximum");
            REQUIRE(p.n_batch == 2048);
            REQUIRE(p.n_threads == 0);
            REQUIRE(p.n_threads_batch == 0);
        }
    }

    WHEN("balanced profile is retrieved") {
        auto p = registry.get("balanced");

        THEN("it has expected batch and thread settings") {
            REQUIRE(p.name == "balanced");
            REQUIRE(p.n_batch == 512);
            REQUIRE(p.n_threads == 0);
        }
    }

    WHEN("minimal profile is retrieved") {
        auto p = registry.get("minimal");

        THEN("it has fixed low thread settings") {
            REQUIRE(p.name == "minimal");
            REQUIRE(p.n_batch == 64);
            REQUIRE(p.n_threads == 2);
            REQUIRE(p.n_threads_batch == 2);
        }
    }

    WHEN("background profile is retrieved") {
        auto p = registry.get("background");

        THEN("it has halved thread count") {
            REQUIRE(p.name == "background");
            REQUIRE(p.n_batch == 256);
            REQUIRE(p.n_threads >= 1);
            REQUIRE(p.n_threads_batch >= 1);
        }
    }
}

// ── Register custom profile ─────────────────────────────────

SCENARIO("Register custom profile", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    WHEN("a custom profile is registered") {
        GPUResourceProfile custom;
        custom.name = "game_npc";
        custom.n_batch = 128;
        custom.n_threads = 4;
        custom.n_threads_batch = 4;
        custom.description = "Game NPC dialog";

        bool ok = registry.register_profile(custom);

        THEN("registration succeeds") {
            REQUIRE(ok);
            REQUIRE(registry.has("game_npc"));
            REQUIRE(registry.size() == 5);
        }

        THEN("retrieved profile matches") {
            auto p = registry.get("game_npc");
            REQUIRE(p.n_batch == 128);
            REQUIRE(p.n_threads == 4);
        }
    }
}

// ── Duplicate name rejected ─────────────────────────────────

SCENARIO("Duplicate name rejected", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    WHEN("a profile with existing name is registered") {
        GPUResourceProfile dup;
        dup.name = "balanced";
        dup.n_batch = 999;

        bool ok = registry.register_profile(dup);

        THEN("registration is rejected") {
            REQUIRE_FALSE(ok);
        }

        THEN("original profile is unchanged") {
            auto p = registry.get("balanced");
            REQUIRE(p.n_batch == 512);
        }
    }
}

// ── Deregister ──────────────────────────────────────────────

SCENARIO("Deregister removes profile", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    GPUResourceProfile custom;
    custom.name = "temp";
    custom.n_batch = 100;
    registry.register_profile(custom);

    WHEN("deregister is called") {
        bool ok = registry.deregister("temp");

        THEN("profile is removed") {
            REQUIRE(ok);
            REQUIRE_FALSE(registry.has("temp"));
        }
    }
}

SCENARIO("Deregister nonexistent returns false", "[profile_registry]") {
    ProfileRegistry registry;

    WHEN("deregister is called on nonexistent name") {
        bool ok = registry.deregister("nonexistent");

        THEN("returns false") {
            REQUIRE_FALSE(ok);
        }
    }
}

// ── Get unknown falls back to balanced ──────────────────────

SCENARIO("Get unknown name falls back to balanced", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    WHEN("get is called with unknown name") {
        auto p = registry.get("nonexistent");

        THEN("returns the balanced profile") {
            REQUIRE(p.name == "balanced");
            REQUIRE(p.n_batch == 512);
        }
    }
}

SCENARIO("Get with no profiles returns empty", "[profile_registry]") {
    ProfileRegistry registry;

    WHEN("get is called on empty registry") {
        auto p = registry.get("anything");

        THEN("returns default-constructed profile") {
            REQUIRE(p.name.empty());
        }
    }
}

// ── List ────────────────────────────────────────────────────

SCENARIO("List returns all profile names", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    GPUResourceProfile custom;
    custom.name = "extra";
    registry.register_profile(custom);

    WHEN("list is called") {
        auto names = registry.list();

        THEN("contains all 5 profile names sorted") {
            REQUIRE(names.size() == 5);
            REQUIRE(std::is_sorted(names.begin(), names.end()));
            REQUIRE(std::find(names.begin(), names.end(), "maximum")
                    != names.end());
            REQUIRE(std::find(names.begin(), names.end(), "extra")
                    != names.end());
        }
    }
}

// ── Idempotent load_bundled ─────────────────────────────────

SCENARIO("load_bundled is idempotent", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    GPUResourceProfile custom;
    custom.name = "extra";
    registry.register_profile(custom);
    REQUIRE(registry.size() == 5);

    WHEN("load_bundled is called again") {
        registry.load_bundled();

        THEN("custom profiles are cleared, only bundled remain") {
            REQUIRE(registry.size() == 4);
            REQUIRE_FALSE(registry.has("extra"));
        }
    }
}

// ── Thread safety ───────────────────────────────────────────

SCENARIO("Concurrent register and get are thread-safe", "[profile_registry]") {
    ProfileRegistry registry;
    registry.load_bundled();

    constexpr int kIterations = 200;

    WHEN("threads register and get concurrently") {
        std::thread writer([&] {
            for (int i = 0; i < kIterations; ++i) {
                GPUResourceProfile p;
                p.name = "thread_" + std::to_string(i);
                p.n_batch = i;
                registry.register_profile(p);
            }
        });

        std::thread reader([&] {
            for (int i = 0; i < kIterations; ++i) {
                auto p = registry.get("balanced");
                REQUIRE(p.n_batch == 512);
            }
        });

        writer.join();
        reader.join();

        THEN("no crash and registry is consistent") {
            REQUIRE(registry.size() >= 4);
        }
    }
}
