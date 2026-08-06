#include "./xapi/include/libsys.h"

static void output(const char *text)
{
    enter_syscall(XAPI_OUTPUT, (uint64_t)text, 0, 0, 0, 0, 0);
}

static bool equals(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) { left++; right++; }
    return *left == *right;
}

static bool copy(char *target, size_t capacity, const char *source)
{
    if (target == nullptr || source == nullptr || capacity == 0) return false;

    size_t index = 0;
    while (source[index] != '\0' && index < capacity - 1)
    {
        target[index] = source[index];
        index++;
    }
    target[index] = '\0';
    return source[index] == '\0';
}

static void read_line(char *buffer, size_t capacity, uint64_t flags = 0)
{
    if (buffer == nullptr || capacity == 0) return;
    buffer[0] = '\0';
    enter_syscall(XAPI_INPUT, (uint64_t)buffer, capacity, flags, 0, 0, 0);
}

static bool authenticate()
{
    char username[64] = {};
    char password[64] = {};
    if (enter_syscall(XAPI_USER_OOBE_REQUIRED, 0, 0, 0, 0, 0, 0) != 0)
    {
        output("First boot setup\nUsername: ");
        read_line(username, sizeof(username));
        output("Password: ");
        read_line(password, sizeof(password), XAPI_INPUT_NO_ECHO);
        int64_t result = (int64_t)enter_syscall(XAPI_USER_CREATE_FIRST, (uint64_t)username,
                                                 (uint64_t)password, 0, 0, 0, 0);
        for (char &ch : password) ch = '\0';
        if (result != 0) { output("Account setup failed; registry may be invalid.\n"); return false; }
        return true;
    }

    for (;;)
    {
        output("Username: ");
        read_line(username, sizeof(username));
        output("Password: ");
        read_line(password, sizeof(password), XAPI_INPUT_NO_ECHO);
        int64_t result = (int64_t)enter_syscall(XAPI_USER_LOGIN, (uint64_t)username,
                                                 (uint64_t)password, 0, 0, 0, 0);
        for (char &ch : password) ch = '\0';
        if (result == 0) return true;
        output(result == -11 ? "Too many attempts; wait and retry.\n" : "Login failed.\n");
    }
}

static int split(char *line, char **arguments, int capacity)
{
    int count = 0;
    char *cursor = line;
    while (*cursor != '\0' && count < capacity - 1)
    {
        while (*cursor == ' ') cursor++;
        if (*cursor == '\0') break;
        arguments[count++] = cursor;
        while (*cursor != '\0' && *cursor != ' ') cursor++;
        if (*cursor != '\0') *cursor++ = '\0';
    }
    arguments[count] = nullptr;
    return count;
}

extern "C" int main(int argc, char *argv[], char *envp[])
{
    (void)argc;
    (void)argv;
    (void)envp;

    output("XJ380 CLI\n");
    if (!authenticate()) return 1;
    for (;;)
    {
        output("xj380$ ");
        char line[256] = {};
        read_line(line, sizeof(line));
        if (line[0] == '\0') continue;
        if (equals(line, "exit")) return 0;
        if (equals(line, "clear")) { output("\033[2J\033[H"); continue; }

        char *arguments[16];
        if (split(line, arguments, 16) == 0) continue;
        char path[256];
        copy(path, sizeof(path), "/apps/");
        char *end = path;
        while (*end != '\0') end++;
        if (!copy(end, sizeof(path) - (size_t)(end - path), arguments[0]))
        {
            output("command name too long\n");
            continue;
        }
        int64_t result = (int64_t)enter_syscall(XAPI_RUN_ARGS, (uint64_t)path, (uint64_t)arguments, 0, 0, 0, 0);
        if (result < 0) output("command not found\n");
    }
}
