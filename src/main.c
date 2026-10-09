#include "vfs_cli.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define VFS_PROMPT "VFS> "
#define VFS_INPUT_SIZE 4096U

static void trim_input(char *input)
{
    size_t length = strlen(input);

    while (length > 0U &&
           isspace((unsigned char)input[length - 1U])) {
        input[--length] = '\0';
    }

    size_t start = 0U;

    while (input[start] != '\0' &&
           isspace((unsigned char)input[start])) {
        start++;
    }

    if (start > 0U) {
        memmove(input, input + start, strlen(input + start) + 1U);
    }
}

static void print_help(void)
{
    printf("\n");
    printf("Available commands\n");
    printf("==================\n\n");

    printf("Directory commands:\n");
    printf("  makedir, removedir, rename, list\n");
    printf("  changedir, back, where, tree\n\n");

    printf("File commands:\n");
    printf("  create, read, write, append\n");
    printf("  copy, move, delete, resize, fileinfo\n\n");

    printf("Filesystem commands:\n");
    printf("  info, storage, inodes, check\n\n");

    printf("General commands:\n");
    printf("  help, exit\n\n");

    printf("Note: Filesystem commands will be enabled as their\n");
    printf("implementations are integrated into the CLI.\n\n");
}

static int process_command(const char *input)
{
    char command[VFS_INPUT_SIZE];

    size_t length = strcspn(input, " \t");

    if (length >= sizeof(command)) {
        printf("Error: command is too long.\n");
        return 0;
    }

    memcpy(command, input, length);
    command[length] = '\0';

    if (strcmp(command, "help") == 0) {
        if (input[length] != '\0') {
            printf("Usage: help\n");
            return 0;
        }

        print_help();
        return 0;
    }

    if (strcmp(command, "exit") == 0) {
        if (input[length] != '\0') {
            printf("Usage: exit\n");
            return 0;
        }

        printf("Goodbye.\n");
        return 1;
    }

    printf("Unknown command: %s\n", command);
    printf("Type 'help' to see available commands.\n");

    return 0;
}

int vfs_cli_run(void)
{
    char input[VFS_INPUT_SIZE];

    printf("\n");
    printf("Virtual Filesystem\n");
    printf("Type 'help' to see available commands.\n\n");

    for (;;) {
        printf(VFS_PROMPT);
        fflush(stdout);

        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (ferror(stdin)) {
                perror("Error reading command");
                return 1;
            }

            printf("\nGoodbye.\n");
            return 0;
        }

        size_t length = strlen(input);

        if (length > 0U && input[length - 1U] != '\n' &&
            !feof(stdin)) {
            int character;

            while ((character = getchar()) != '\n' &&
                   character != EOF) {
                /* Discard the remainder of the oversized input. */
            }

            printf("Error: command is too long.\n");
            continue;
        }

        trim_input(input);

        if (input[0] == '\0') {
            continue;
        }

        if (process_command(input)) {
            return 0;
        }
    }
}

int main(void)
{
    return vfs_cli_run();
}