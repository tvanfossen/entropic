// SPDX-License-Identifier: Apache-2.0
/**
 * @file stream_think_filter_test.cpp
 * @brief v2.3.10 backstop coverage for StreamThinkFilter — drives every
 * branch of the byte-level state machine and the UTF-8 codepoint buffer.
 * @version 2.3.10
 */

#include <catch2/catch_test_macros.hpp>
#include <entropic/core/stream_think_filter.h>
#include <string>

using namespace entropic;

namespace {
struct Sink { std::string buf; int calls = 0; };
static void sink_cb(const char* d, size_t n, void* ud) {
    auto* s = static_cast<Sink*>(ud); s->buf.append(d, n); s->calls++;
}
static std::string filter_once(const std::string& in, Sink& sink) {
    StreamThinkFilter f(sink_cb, &sink);
    f.on_token(in.data(), in.size());
    f.flush();
    return sink.buf;
}
} // namespace

TEST_CASE("StreamThinkFilter: tag-state machine coverage",
          "[v2.3.10][core][stream_think_filter_coverage]") {
    SECTION("plain text passes through") {
        Sink s;
        REQUIRE(filter_once("hello world", s) == "hello world");
    }
    SECTION("single complete think block stripped") {
        Sink s;
        REQUIRE(filter_once("before<think>secret</think>after", s)
                == "beforeafter");
    }
    SECTION("multiple back-to-back think blocks stripped") {
        Sink a;
        REQUIRE(filter_once("A<think>x</think>B<think>y</think>C", a)
                == "ABC");
        Sink b;
        REQUIRE(filter_once("<think>a</think><think>b</think>tail", b)
                == "tail");
    }
    SECTION("tags split across chunk boundaries match") {
        Sink s;
        StreamThinkFilter f(sink_cb, &s);
        for (const char* p : {"pre", "<th", "ink>", "X",
                              "</thi", "nk>", "post"}) {
            f.on_token(p, std::string(p).size());
        }
        f.flush();
        REQUIRE(s.buf == "prepost");
    }
    SECTION("8-byte buffer overflow forwards accumulated bytes") {
        Sink s;
        std::string out = filter_once("<notathing>", s);
        REQUIRE(out.find('<') != std::string::npos);
        REQUIRE(out.find("not") != std::string::npos);
        REQUIRE(out.find('>') != std::string::npos);
    }
    SECTION("unterminated think block holds back trailing content") {
        Sink s;
        REQUIRE(filter_once("visible<think>still thinking when EOF", s)
                == "visible");
    }
}

TEST_CASE("StreamThinkFilter: raw callback + UTF-8 boundary",
          "[v2.3.10][core][stream_think_filter_coverage]") {
    SECTION("raw callback receives every byte unfiltered") {
        Sink filtered, raw;
        StreamThinkFilter f(sink_cb, &filtered);
        f.set_raw_callback(sink_cb, &raw);
        std::string in = "outer<think>secret</think>tail";
        f.on_token(in.data(), in.size());
        f.flush();
        REQUIRE(filtered.buf == "outertail");
        REQUIRE(raw.buf == in);
    }
    SECTION("incomplete UTF-8 codepoint buffered across chunks") {
        Sink s;
        StreamThinkFilter f(sink_cb, &s);
        const char lead = static_cast<char>(0xC3);
        const char cont = static_cast<char>(0xA9);
        f.on_token(&lead, 1);
        REQUIRE(s.buf.empty());
        f.on_token(&cont, 1);
        REQUIRE(s.buf.size() == 2);
        REQUIRE(static_cast<unsigned char>(s.buf[0]) == 0xC3);
        REQUIRE(static_cast<unsigned char>(s.buf[1]) == 0xA9);
    }
    SECTION("flush forwards leftover partial UTF-8 buffer") {
        Sink s;
        StreamThinkFilter f(sink_cb, &s);
        const char lead = static_cast<char>(0xE2);
        f.on_token(&lead, 1);
        REQUIRE(s.buf.empty());
        f.flush();
        REQUIRE(s.buf.size() == 1);
        REQUIRE(static_cast<unsigned char>(s.buf[0]) == 0xE2);
    }
    SECTION("flush is a safe no-op on an empty filter") {
        Sink s;
        StreamThinkFilter f(sink_cb, &s);
        f.flush();
        REQUIRE(s.calls == 0);
        REQUIRE(s.buf.empty());
    }
}
