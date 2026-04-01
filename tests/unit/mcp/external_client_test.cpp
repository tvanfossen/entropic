/**
 * @file test_external_client.cpp
 * @brief ExternalMCPClient unit tests with mock transport.
 * @version 1.8.7
 */

#include <entropic/mcp/external_client.h>
#include <entropic/mcp/transport.h>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace entropic;
using json = nlohmann::json;

/**
 * @brief Mock transport for unit testing ExternalMCPClient.
 * @version 1.8.7
 */
class MockTransport : public Transport {
public:
    bool open_result = true;             ///< What open() returns
    bool connected = false;              ///< is_connected state
    std::vector<std::string> requests;   ///< Captured requests
    std::vector<std::string> responses;  ///< Queued responses
    int response_idx = 0;               ///< Next response index

    /**
     * @brief Open: set connected based on open_result.
     * @return open_result.
     * @version 1.8.7
     */
    bool open() override {
        connected = open_result;
        return open_result;
    }

    /**
     * @brief Close: set disconnected.
     * @version 1.8.7
     */
    void close() override { connected = false; }

    /**
     * @brief Send request, return queued response.
     * @param request_json Request.
     * @param timeout_ms Timeout (unused).
     * @return Next queued response.
     * @version 1.8.7
     */
    std::string send_request(const std::string& request_json,
                             uint32_t /*timeout_ms*/) override {
        requests.push_back(request_json);
        if (response_idx < static_cast<int>(responses.size())) {
            return responses[response_idx++];
        }
        return "";
    }

    /**
     * @brief Check connected state.
     * @return connected.
     * @version 1.8.7
     */
    bool is_connected() const override { return connected; }
};

/**
 * @brief Build a mock initialize response.
 * @param id Request ID.
 * @return JSON string.
 * @utility
 * @version 1.8.7
 */
static std::string mock_init_response(int id) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["protocolVersion"] = "2024-11-05";
    resp["result"]["serverInfo"]["name"] = "mock";
    resp["result"]["capabilities"] = json::object();
    return resp.dump();
}

/**
 * @brief Build a mock tools/list response.
 * @param id Request ID.
 * @param tools Tool names.
 * @return JSON string.
 * @utility
 * @version 1.8.7
 */
static std::string mock_tools_response(
    int id, const std::vector<std::string>& tools) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    auto arr = json::array();
    for (const auto& t : tools) {
        arr.push_back({{"name", t}, {"description", "mock"}});
    }
    resp["result"]["tools"] = arr;
    return resp.dump();
}

/**
 * @brief Build a mock tools/call response.
 * @param id Request ID.
 * @param text Result text.
 * @return JSON string.
 * @utility
 * @version 1.8.7
 */
static std::string mock_call_response(int id,
                                      const std::string& text) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"]["content"] = json::array({
        {{"type", "text"}, {"text", text}}
    });
    return resp.dump();
}

TEST_CASE("Connect sends initialize and tools/list",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    auto* mock = transport.get();

    // Queue: initialize response, then tools/list response
    mock->responses.push_back(mock_init_response(1));
    mock->responses.push_back(
        mock_tools_response(2, {"echo", "reverse"}));

    ExternalMCPClient client("mock", std::move(transport));
    REQUIRE(client.connect());
    REQUIRE(mock->requests.size() == 2);

    // Verify tools are prefixed
    auto tools = json::parse(client.list_tools());
    REQUIRE(tools.size() == 2);
    REQUIRE(tools[0]["name"] == "mock.echo");
    REQUIRE(tools[1]["name"] == "mock.reverse");
}

TEST_CASE("Execute sends tools/call and returns result",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    auto* mock = transport.get();

    mock->responses.push_back(mock_init_response(1));
    mock->responses.push_back(
        mock_tools_response(2, {"echo"}));
    mock->responses.push_back(
        mock_call_response(3, "hello world"));

    ExternalMCPClient client("mock", std::move(transport));
    client.connect();

    auto result = client.execute("echo", R"({"text":"hello"})");
    auto j = json::parse(result);
    REQUIRE(j["result"] == "hello world");
    // Security: directives always empty for external servers
    REQUIRE(j["directives"].empty());
}

TEST_CASE("Execute when disconnected returns error",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    transport->open_result = false;

    ExternalMCPClient client("mock", std::move(transport));
    // Don't connect — transport is disconnected

    auto result = client.execute("echo", "{}");
    auto j = json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("disconnected") != std::string::npos);
}

TEST_CASE("Connect fails when transport open fails",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    transport->open_result = false;

    ExternalMCPClient client("mock", std::move(transport));
    REQUIRE_FALSE(client.connect());
    REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("Disconnect clears cached tools",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    auto* mock = transport.get();

    mock->responses.push_back(mock_init_response(1));
    mock->responses.push_back(
        mock_tools_response(2, {"echo"}));

    ExternalMCPClient client("mock", std::move(transport));
    client.connect();

    auto tools_before = json::parse(client.list_tools());
    REQUIRE(tools_before.size() == 1);

    client.disconnect();

    auto tools_after = json::parse(client.list_tools());
    REQUIRE(tools_after.empty());
}

TEST_CASE("Refresh tools detects additions",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    auto* mock = transport.get();

    mock->responses.push_back(mock_init_response(1));
    mock->responses.push_back(
        mock_tools_response(2, {"echo"}));
    // Refresh response with additional tool
    mock->responses.push_back(
        mock_tools_response(3, {"echo", "reverse"}));

    ExternalMCPClient client("mock", std::move(transport));
    client.connect();

    auto [added, removed] = client.refresh_tools();
    REQUIRE(added.size() == 1);
    REQUIRE(added[0] == "mock.reverse");
    REQUIRE(removed.empty());
}

TEST_CASE("Refresh tools detects removals",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    auto* mock = transport.get();

    mock->responses.push_back(mock_init_response(1));
    mock->responses.push_back(
        mock_tools_response(2, {"echo", "reverse"}));
    // Refresh with one tool removed
    mock->responses.push_back(
        mock_tools_response(3, {"echo"}));

    ExternalMCPClient client("mock", std::move(transport));
    client.connect();

    auto [added, removed] = client.refresh_tools();
    REQUIRE(added.empty());
    REQUIRE(removed.size() == 1);
    REQUIRE(removed[0] == "mock.reverse");
}

TEST_CASE("Execute timeout returns error",
          "[external_client]") {
    auto transport = std::make_unique<MockTransport>();
    auto* mock = transport.get();

    mock->responses.push_back(mock_init_response(1));
    mock->responses.push_back(
        mock_tools_response(2, {"echo"}));
    // No response queued for tools/call → empty string (timeout)

    ExternalMCPClient client("mock", std::move(transport));
    client.connect();

    auto result = client.execute("echo", "{}");
    auto j = json::parse(result);
    std::string r = j["result"];
    REQUIRE(r.find("timed out") != std::string::npos);
}
