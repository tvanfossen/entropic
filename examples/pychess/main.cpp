// SPDX-License-Identifier: LGPL-3.0-or-later
/**
 * @file main.cpp
 * @brief PyChess — play chess against a local LLM via the entropic C API.
 *
 * Demonstrates multi-tier pipeline (thinker + executor), grammar constraints,
 * external MCP server registration, and streaming output. The chess MCP
 * server (chess_server.py) runs as an external process over stdio transport.
 *
 * Board state is managed locally in an 8x8 array. The MCP server is
 * stateless — it acknowledges moves without board tracking. The AI's
 * move is parsed from the streaming output.
 *
 * Usage:
 *   1. Build: cmake -B build && cmake --build build
 *   2. Edit data/default_config.yaml with your model paths
 *   3. Run: ./build/pychess
 *
 * @version 1
 */

#include <entropic/entropic.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/* ── Board constants ─────────────────────────────────── */

static const char INIT_BOARD[8][8] = {
    {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
    {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'.', '.', '.', '.', '.', '.', '.', '.'},
    {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
    {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'},
};

/* ── Game state ──────────────────────────────────────── */

/**
 * @brief Mutable game state for the chess session.
 *
 * Tracks the 8x8 board, move history, and accumulated streaming
 * output for AI move parsing.
 *
 * @internal
 * @version 1
 */
struct GameState {
    char board[8][8];       ///< Piece positions (uppercase=White, lowercase=Black).
    char moves[200][6];     ///< UCI move history (up to 200 half-moves).
    int move_count;         ///< Number of half-moves played.
    char output[16384];     ///< Accumulated streaming output for parsing.
    size_t output_len;      ///< Current length of accumulated output.
};

/* ── Board management ────────────────────────────────── */

/**
 * @brief Initialize board to the standard chess starting position.
 *
 * @param state  Game state to initialize.
 *
 * @utility
 * @version 1
 */
static void init_board(GameState& state)
{
    std::memcpy(state.board, INIT_BOARD, sizeof(INIT_BOARD));
    state.move_count = 0;
    state.output_len = 0;
    state.output[0] = '\0';
}

/**
 * @brief Display the board as ASCII art with file and rank labels.
 *
 * @param board  8x8 board array. Row 0 = rank 8, row 7 = rank 1.
 *
 * @utility
 * @version 1
 */
static void display_board(const char board[8][8])
{
    std::printf("\n    a b c d e f g h\n");
    for (int row = 0; row < 8; row++) {
        std::printf("  %d ", 8 - row);
        for (int col = 0; col < 8; col++) {
            std::printf("%c ", board[row][col]);
        }
        std::printf("\n");
    }
    std::printf("\n");
}

/**
 * @brief Apply a UCI move to the board (simple piece movement).
 *
 * Moves the piece from source to destination. Handles pawn promotion
 * (5th character). No legality validation — format only.
 *
 * @param board  8x8 board to modify.
 * @param uci    UCI move string (4-5 chars, e.g. "e2e4", "e7e8q").
 *
 * @utility
 * @version 1
 */
static void apply_uci(char board[8][8], const char* uci)
{
    int from_row = 7 - (uci[1] - '1');
    int from_col = uci[0] - 'a';
    int to_row = 7 - (uci[3] - '1');
    int to_col = uci[2] - 'a';
    char piece = board[from_row][from_col];
    board[to_row][to_col] = piece;
    board[from_row][from_col] = '.';
    if (uci[4] != '\0') {
        char promo = uci[4];
        board[to_row][to_col] = (piece >= 'A' && piece <= 'Z')
            ? static_cast<char>(promo - 32)
            : promo;
    }
}

/* ── Streaming ───────────────────────────────────────── */

/**
 * @brief Streaming token callback — prints to stderr and accumulates.
 *
 * Tokens are printed to stderr for live output (thinker analysis).
 * Accumulated in GameState::output for AI move parsing afterward.
 *
 * @param token      Token bytes (not null-terminated).
 * @param len        Token byte length.
 * @param user_data  Pointer to GameState.
 *
 * @callback
 * @version 1
 */
static void on_token(const char* token, size_t len, void* user_data)
{
    auto* state = static_cast<GameState*>(user_data);
    std::fwrite(token, 1, len, stderr);
    std::fflush(stderr);
    size_t remaining = sizeof(state->output) - state->output_len - 1;
    size_t to_copy = (len < remaining) ? len : remaining;
    std::memcpy(state->output + state->output_len, token, to_copy);
    state->output_len += to_copy;
    state->output[state->output_len] = '\0';
}

/* ── Move parsing ────────────────────────────────────── */

/**
 * @brief Parse the AI's UCI move from accumulated streaming output.
 *
 * Searches for the "move":"XXXX" pattern emitted by the executor
 * tier's grammar-constrained tool call.
 *
 * @param output    Accumulated streaming text.
 * @param move_out  Output buffer (at least 6 bytes).
 * @return true if a valid UCI move was found.
 *
 * @utility
 * @version 1
 */
static bool parse_ai_move(const char* output, char* move_out)
{
    // Try thinker grammar output: "Best move: d7d5\n"
    const char* best = std::strstr(output, "Best move: ");
    if (best) {
        best += 11; // strlen("Best move: ")
        size_t i = 0;
        while (best[i] != '\n' && best[i] != '\0' && i < 5) {
            move_out[i] = best[i];
            i++;
        }
        move_out[i] = '\0';
        return i >= 4;
    }
    // Fallback: executor tool call JSON: "move":"d7d5"
    const char* pattern = "\"move\":\"";
    const char* pos = std::strstr(output, pattern);
    if (!pos) {
        return false;
    }
    pos += std::strlen(pattern);
    size_t i = 0;
    while (pos[i] != '"' && pos[i] != '\0' && i < 5) {
        move_out[i] = pos[i];
        i++;
    }
    move_out[i] = '\0';
    return i >= 4;
}

/* ── Board context ───────────────────────────────────── */

/**
 * @brief Build a piece list string for one color.
 *
 * Scans the board for pieces of the given color and formats them
 * as "Pc2, Nb1, Ra1" etc.
 *
 * @param board  8x8 board array.
 * @param white  true for White pieces, false for Black.
 * @return Comma-separated piece list string.
 *
 * @utility
 * @version 1
 */
static std::string build_piece_list(const char board[8][8], bool white)
{
    std::string result;
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            char piece = board[row][col];
            if (piece == '.') {
                continue;
            }
            bool is_white = (piece >= 'A' && piece <= 'Z');
            if (is_white != white) {
                continue;
            }
            if (!result.empty()) {
                result += ", ";
            }
            result += piece;
            result += static_cast<char>('a' + col);
            result += static_cast<char>('0' + (8 - row));
        }
    }
    return result;
}

/**
 * @brief Build board context string for the AI prompt.
 *
 * Includes piece positions and move history so the AI can
 * analyze the current position.
 *
 * @param state  Current game state.
 * @return Multi-line context string for the system prompt.
 *
 * @utility
 * @version 1
 */
static std::string build_board_context(const GameState& state)
{
    std::string ctx = "## Current Position\n\n";
    ctx += "White pieces: " + build_piece_list(state.board, true) + "\n";
    ctx += "Black pieces: " + build_piece_list(state.board, false) + "\n\n";
    ctx += "Move " + std::to_string((state.move_count / 2) + 1) + "\n";
    ctx += "Side to move: Black (you)\n";
    if (state.move_count > 0) {
        ctx += "\nMove history:\n";
        for (int i = 0; i < state.move_count; i++) {
            if (i % 2 == 0) {
                ctx += std::to_string((i / 2) + 1) + ". " + state.moves[i];
            } else {
                ctx += " " + std::string(state.moves[i]) + "\n";
            }
        }
        if (state.move_count % 2 == 1) {
            ctx += "\n";
        }
    }
    return ctx;
}

/* ── Human input ─────────────────────────────────────── */

/**
 * @brief Read a UCI move from stdin with format validation.
 *
 * Loops until the user enters a valid 4-5 character UCI string
 * or a quit command.
 *
 * @param buf   Output buffer (at least 8 bytes).
 * @param size  Buffer capacity.
 * @return true if a move was read, false on quit/EOF.
 *
 * @utility
 * @version 1
 */
static bool read_human_move(char* buf, size_t size)
{
    while (true) {
        std::printf("Your move (UCI, e.g. e2e4): ");
        std::fflush(stdout);
        if (!std::fgets(buf, static_cast<int>(size), stdin)) {
            return false;
        }
        size_t len = std::strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') {
            buf[--len] = '\0';
        }
        bool is_quit = (std::strcmp(buf, "quit") == 0 || std::strcmp(buf, "q") == 0);
        if (len == 0 || is_quit) {
            return false;
        }
        if (len >= 4 && len <= 5) {
            return true;
        }
        std::printf("  Invalid UCI format (expected 4-5 chars)\n");
    }
}

/* ── Game turns ──────────────────────────────────────── */

/**
 * @brief Play the human's turn (White).
 *
 * Reads a move, applies it to the board, records in history.
 *
 * @param state  Game state to update.
 * @return true if a move was played, false if the player quit.
 *
 * @internal
 * @version 1
 */
static bool play_human_turn(GameState& state)
{
    char move[8];
    if (!read_human_move(move, sizeof(move))) {
        return false;
    }
    apply_uci(state.board, move);
    std::strncpy(state.moves[state.move_count], move, 5);
    state.moves[state.move_count][5] = '\0';
    state.move_count++;
    display_board(state.board);
    return true;
}

/**
 * @brief Play the AI's turn (Black).
 *
 * Builds board context, runs streaming generation through the
 * thinker+executor pipeline, parses the AI's move from output.
 *
 * @param handle  Engine handle.
 * @param state   Game state to update.
 * @return true if the AI played a move, false on error.
 *
 * @internal
 * @version 1
 */
static bool play_ai_turn(entropic_handle_t handle, GameState& state)
{
    std::string prompt = build_board_context(state)
        + "\nIt's your turn (Black). Analyze the position and play your move.";
    state.output_len = 0;
    state.output[0] = '\0';
    std::printf("AI is thinking...\n");
    entropic_error_t err = entropic_run_streaming(
        handle, prompt.c_str(), on_token, &state, nullptr);
    if (err != ENTROPIC_OK) {
        std::fprintf(stderr, "\nAI error: %s\n", entropic_last_error(handle));
        return false;
    }
    char ai_move[6];
    if (!parse_ai_move(state.output, ai_move)) {
        std::fprintf(stderr, "\nCould not parse AI move from output\n");
        return false;
    }
    std::printf("\nAI plays: %s\n", ai_move);
    apply_uci(state.board, ai_move);
    std::strncpy(state.moves[state.move_count], ai_move, 5);
    state.moves[state.move_count][5] = '\0';
    state.move_count++;
    display_board(state.board);
    return true;
}

/* ── Engine setup ────────────────────────────────────── */

/**
 * @brief Create, configure, and wire up the entropic engine.
 *
 * Creates a handle, configures via layered resolution from
 * .pychess/ directory, and registers chess_server.py as an
 * external MCP server over stdio transport.
 *
 * @param project_dir  Project config directory (e.g. ".pychess").
 * @return Configured engine handle, or nullptr on failure.
 *
 * @internal
 * @version 2
 */
static entropic_handle_t setup_engine(const char* project_dir)
{
    entropic_handle_t handle = nullptr;
    entropic_create(&handle);
    if (!handle) {
        std::fprintf(stderr, "entropic_create failed\n");
        return nullptr;
    }
    std::string cmd = "chess_server.py";
    std::string json = R"({"command":"python3","args":[")" + cmd + R"("]})";
    bool ok = (entropic_configure_dir(handle, project_dir) == ENTROPIC_OK)
           && (entropic_grammar_register_file(
                   handle, "chess_thinker",
                   "data/grammars/chess_thinker.gbnf") == ENTROPIC_OK)
           && (entropic_grammar_register_file(
                   handle, "chess_executor",
                   "data/grammars/chess_executor.gbnf") == ENTROPIC_OK)
           && (entropic_register_mcp_server(handle, "chess", json.c_str()) == ENTROPIC_OK);
    if (!ok) {
        std::fprintf(stderr, "Setup failed: %s\n", entropic_last_error(handle));
        entropic_destroy(handle);
        return nullptr;
    }
    return handle;
}

/**
 * @brief Run the interactive chess game loop.
 *
 * Alternates between human (White) and AI (Black) turns until
 * a player quits or an error occurs.
 *
 * @param handle  Configured engine handle.
 *
 * @internal
 * @version 1
 */
static void game_loop(entropic_handle_t handle)
{
    GameState state{};
    init_board(state);
    std::printf("PyChess — You are White, AI is Black.\n");
    std::printf("Enter moves in UCI notation (e.g. e2e4). Type 'quit' to exit.\n");
    display_board(state.board);
    while (play_human_turn(state) && play_ai_turn(handle, state)) {
        /* continue until quit or error */
    }
}

/* ── Entry point ─────────────────────────────────────── */

/**
 * @brief Run the pychess game.
 *
 * Sets up the engine, runs the game loop, and cleans up.
 * Assumes CWD is the example directory.
 *
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 *
 * @internal
 * @version 1
 */
static int run_game()
{
    entropic_handle_t handle = setup_engine(".pychess");
    if (!handle) {
        return EXIT_FAILURE;
    }
    game_loop(handle);
    entropic_destroy(handle);
    std::printf("Goodbye!\n");
    return EXIT_SUCCESS;
}

/**
 * @brief Entry point.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 *
 * @internal
 * @version 1
 */
int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;
    return run_game();
}
