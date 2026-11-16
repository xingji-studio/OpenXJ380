#include <user/environment.h>
#include <errno.h>
#include <krlibc.h>
#include <mm/heap.h>

bool environment_name_valid(const char *name, size_t length)
{
    if (!name || !length) return false;
    for (size_t i = 0; i < length; ++i)
    {
        char c = name[i];
        if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) continue;
        if (i && c >= '0' && c <= '9') continue;
        return false;
    }
    return true;
}

void environment_free(char **values)
{
    if (!values) return;
    for (size_t i = 0; values[i]; ++i) free(values[i]);
    free(values);
}

int environment_set(EnvironmentTable *table, const char *entry)
{
    if (!table || !entry) return -EINVAL;
    const char *equal = strchr(entry, '=');
    if (!equal || !environment_name_valid(entry, equal - entry)) return -EINVAL;
    size_t length = strlen(entry) + 1;
    if (length > ENVIRONMENT_ENTRY_MAX) return -E2BIG;
    size_t index = 0, old_length = 0;
    for (; index < table->count; ++index)
    {
        if (!strncmp(table->values[index], entry, equal - entry) && table->values[index][equal - entry] == '=')
        {
            old_length = strlen(table->values[index]) + 1;
            break;
        }
    }
    if (table->bytes - old_length + length > ENVIRONMENT_FILE_MAX ||
        (index == table->count && table->count == ENVIRONMENT_COUNT_MAX)) return -E2BIG;
    if (!table->values)
    {
        table->values = (char **)calloc(ENVIRONMENT_COUNT_MAX + 1, sizeof(char *));
        if (!table->values) return -ENOMEM;
    }
    char *copy = strdup(entry);
    if (!copy) return -ENOMEM;
    free(table->values[index]);
    table->values[index] = copy;
    if (index == table->count) ++table->count;
    table->bytes = table->bytes - old_length + length;
    return 0;
}

char **environment_clone(char *const *values, size_t *count)
{
    EnvironmentTable copy = {};
    for (size_t i = 0; values && values[i]; ++i)
    {
        if (i == ENVIRONMENT_COUNT_MAX || environment_set(&copy, values[i]) < 0)
        {
            environment_free(copy.values);
            return NULL;
        }
    }
    if (!copy.values) copy.values = (char **)calloc(1, sizeof(char *));
    if (count) *count = copy.count;
    return copy.values;
}

static bool environment_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

int environment_parse(EnvironmentTable *table, const char *text, size_t length, size_t *error_line)
{
    if (!table || !text) return -EINVAL;
    if (length > ENVIRONMENT_FILE_MAX) return -E2BIG;
    size_t offset = length >= 3 && (unsigned char)text[0] == 0xef &&
                    (unsigned char)text[1] == 0xbb && (unsigned char)text[2] == 0xbf ? 3 : 0;
    size_t line = 0;
    while (offset < length)
    {
        ++line;
        if (error_line) *error_line = line;
        size_t end = offset;
        while (end < length && text[end] != '\n')
        {
            if (!text[end]) return -EINVAL;
            ++end;
        }
        size_t p = offset;
        offset = end < length ? end + 1 : end;
        while (p < end && environment_space(text[p])) ++p;
        if (p == end || text[p] == '#') continue;
        if (end - p >= 7 && !strncmp(text + p, "export ", 7)) p += 7;
        while (p < end && environment_space(text[p])) ++p;
        size_t name = p;
        while (p < end && !environment_space(text[p]) && text[p] != '=') ++p;
        size_t name_length = p - name;
        if (!environment_name_valid(text + name, name_length)) return -EINVAL;
        while (p < end && environment_space(text[p])) ++p;
        if (p == end || text[p++] != '=') return -EINVAL;
        while (p < end && environment_space(text[p])) ++p;
        char entry[ENVIRONMENT_ENTRY_MAX];
        if (name_length + 2 > sizeof(entry)) return -E2BIG;
        memcpy(entry, text + name, name_length);
        size_t used = name_length;
        entry[used++] = '=';
        char quote = 0;
        size_t keep = used;
        for (; p < end; ++p)
        {
            char c = text[p];
            if (!quote && c == '#' && (p == 0 || environment_space(text[p - 1]))) break;
            if (c == '\'' || c == '"')
            {
                if (!quote) { quote = c; continue; }
                if (quote == c) { quote = 0; keep = used; continue; }
            }
            bool protected_space = quote != 0;
            if (c == '\\' && quote != '\'')
            {
                if (++p == end) return -EINVAL;
                c = text[p];
                protected_space = true;
            }
            if (used + 1 >= sizeof(entry)) return -E2BIG;
            entry[used++] = c;
            if (protected_space || !environment_space(c)) keep = used;
        }
        if (quote) return -EINVAL;
        entry[keep] = 0;
        int error = environment_set(table, entry);
        if (error) return error;
    }
    if (error_line) *error_line = 0;
    return 0;
}
