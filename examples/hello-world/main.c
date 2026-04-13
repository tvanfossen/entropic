/**
 * @file main.c
 * @brief Hello World — minimal C API consumer for entropic.
 *
 * Demonstrates:
 *   - Single-tier config with bundled lead identity
 *   - Streaming output via callback
 *   - App context injection (consumer personality)
 *
 * Usage:
 *   1. Build: cmake -B build && cmake --build build
 *   2. Edit config.yaml with your model path
 *   3. Run: ./build/hello-world
 *
 * @version 1
 */

#include <entropic/entropic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Streaming token callback — prints each token to stdout.
 *
 * @param token  Token bytes (not null-terminated).
 * @param len    Token byte length.
 * @param user_data  Unused.
 *
 * @callback
 * @version 1
 */
static void on_token(const char* token, size_t len, void* user_data)
{
    (void)user_data;
    fwrite(token, 1, len, stdout);
    fflush(stdout);
}

/* resolve_config_path removed — examples run from their own directory */

/**
 * @brief Read a line from stdin into a fixed buffer.
 *
 * @param buf   Output buffer.
 * @param size  Buffer capacity.
 * @return 1 on success, 0 on EOF or quit command.
 *
 * @utility
 * @version 1
 */
static int read_prompt(char* buf, size_t size)
{
    printf("You: ");
    fflush(stdout);
    if (!fgets(buf, (int)size, stdin)) {
        return 0;
    }
    /* Strip trailing newline. */
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
        len--;
    }
    int is_quit = (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0 || strcmp(buf, "q") == 0);
    return (len > 0 && !is_quit) ? 1 : 0;
}

/**
 * @brief Interactive streaming chat loop.
 *
 * Creates an engine, configures via layered resolution, then loops:
 * read prompt → run_streaming → print tokens → repeat.
 *
 * @param project_dir  Project config directory (e.g. ".hello-world").
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 *
 * @internal
 * @version 1
 */
static int run_chat(const char* project_dir)
{
    entropic_handle_t handle = NULL;
    entropic_error_t err = entropic_create(&handle);
    if (err != ENTROPIC_OK) {
        fprintf(stderr, "entropic_create failed: %s\n", entropic_error_name(err));
        return EXIT_FAILURE;
    }

    err = entropic_configure_dir(handle, project_dir);
    if (err != ENTROPIC_OK) {
        fprintf(stderr, "configure failed: %s\n", entropic_last_error(handle));
        entropic_destroy(handle);
        return EXIT_FAILURE;
    }

    printf("entropic hello-world (C engine)\n");
    printf("Type 'quit' to exit.\n\n");

    char prompt[4096];
    while (read_prompt(prompt, sizeof(prompt))) {
        err = entropic_run_streaming(handle, prompt, on_token, NULL, NULL);
        if (err != ENTROPIC_OK) {
            fprintf(stderr, "\ngenerate error: %s\n", entropic_last_error(handle));
        }
        printf("\n\n");
    }

    entropic_destroy(handle);
    printf("Bye!\n");
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
    return run_chat(".hello-world");
}
