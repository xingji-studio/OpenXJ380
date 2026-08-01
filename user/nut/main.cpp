#include "../xapi/include/x3api.h"
#include "../xapi/include/xapi_json.h"
#include "../include/httpclient.h"
#include "../include/https_client.h"

typedef struct nut_http_buffer
{
    char   *data;
    size_t  size;
    size_t  capacity;
    int     oom;
} nut_http_buffer_t;

static int nut_http_append_callback(const char *data, int len, void *user_data)
{
    nut_http_buffer_t *buffer = (nut_http_buffer_t *)user_data;
    if (buffer == NULL || data == NULL || len <= 0) {
        return 0;
    }

    if (buffer->size + (size_t)len + 1 > buffer->capacity) {
        size_t new_capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
        while (new_capacity < buffer->size + (size_t)len + 1) {
            new_capacity *= 2;
        }

        char *new_data = (char *)realloc(buffer->data, new_capacity);
        if (new_data == NULL) {
            buffer->oom = 1;
            return 1;
        }

        buffer->data     = new_data;
        buffer->capacity = new_capacity;
    }

    memcpy(buffer->data + buffer->size, data, (size_t)len);
    buffer->size += (size_t)len;
    buffer->data[buffer->size] = '\0';
    return 0;
}

static int nut_download_binary_body(const char *host,
                                    uint16_t port,
                                    bool tls,
                                    const char *path,
                                    char **out_body,
                                    size_t *out_body_size)
{
    if (host == NULL || host[0] == '\0' || path == NULL || path[0] == '\0' || out_body == NULL || out_body_size == NULL) {
        return -1;
    }

    *out_body      = NULL;
    *out_body_size = 0;

    xhttp_request_t request;
    request.method        = "GET";
    request.host          = host;
    request.port          = port;
    request.path          = path;
    request.body          = NULL;
    request.content_type  = NULL;
    request.extra_headers = "Connection: close\r\n";

    nut_http_buffer_t response;
    memset(&response, 0, sizeof(response));

    int status = tls ? xtls_request(&request, nut_http_append_callback, &response)
                     : xhttp_request(&request, nut_http_append_callback, &response);
    if (status != XHTTP_OK || response.oom || response.data == NULL || response.size == 0) {
        free(response.data);
        return -1;
    }

    const char *body_start = NULL;
    size_t      body_offset = 0;

    const char *marker = strstr(response.data, "\r\n\r\n");
    if (marker != NULL) {
        body_start  = marker + 4;
        body_offset = (size_t)(body_start - response.data);
    } else {
        marker = strstr(response.data, "\n\n");
        if (marker != NULL) {
            body_start  = marker + 2;
            body_offset = (size_t)(body_start - response.data);
        }
    }

    if (body_start == NULL || body_offset > response.size) {
        free(response.data);
        return -1;
    }

    size_t body_size = response.size - body_offset;
    if (body_size == 0) {
        free(response.data);
        return -1;
    }

    char *body = (char *)malloc(body_size);
    if (body == NULL) {
        free(response.data);
        return -1;
    }

    memcpy(body, body_start, body_size);
    free(response.data);

    *out_body      = body;
    *out_body_size = body_size;
    return 0;
}

static int print_package_metadata(const xapi_JsonValue *item, int index)
{
    if (item == NULL || item->type != XAPI_JSON_VALUE_OBJECT) {
        xapi_Printf((char *)"%d. <无效的软件包对象>\n", index + 1);
        return -1;
    }

    xapi_JsonValue *name = xapi_jsonObjectGet(item, "name");
    if (name == NULL || name->type != XAPI_JSON_VALUE_STRING || name->as.stringValue == NULL) {
        xapi_Printf((char *)"%d. <无效的软件包名称>\n", index + 1);
        return -1;
    }

    xapi_Printf((char *)"%d. %s\n", index + 1, name->as.stringValue);

    xapi_JsonValue *description = xapi_jsonObjectGet(item, "description");
    if (description != NULL && description->type == XAPI_JSON_VALUE_STRING && description->as.stringValue != NULL) {
        xapi_Printf((char *)"  描述: %s\n", description->as.stringValue);
    }

    xapi_JsonValue *author = xapi_jsonObjectGet(item, "author");
    if (author != NULL && author->type == XAPI_JSON_VALUE_STRING && author->as.stringValue != NULL) {
        xapi_Printf((char *)"  作者: %s\n", author->as.stringValue);
    }

    xapi_JsonValue *latest = xapi_jsonObjectGet(item, "latest");
    const char     *latest_version = NULL;
    if (latest != NULL && latest->type == XAPI_JSON_VALUE_STRING && latest->as.stringValue != NULL) {
        latest_version = latest->as.stringValue;
        xapi_Printf((char *)"  最新版本: %s\n", latest_version);
    }

    xapi_JsonValue *versions = xapi_jsonObjectGet(item, "versions");
    if (versions == NULL || versions->type != XAPI_JSON_VALUE_ARRAY) {
        xapi_Printf((char *)"  错误：versions 不是数组。\n");
        return -1;
    }

    int versions_count = xapi_jsonArraySize(versions);
    for (int i = 0; i < versions_count; ++i) {
        xapi_JsonValue *version_item = xapi_jsonArrayAt(versions, i);
        if (version_item == NULL || version_item->type != XAPI_JSON_VALUE_OBJECT) {
            continue;
        }

        xapi_JsonValue *version = xapi_jsonObjectGet(version_item, "version");
        xapi_JsonValue *files   = xapi_jsonObjectGet(version_item, "files");

        if (version == NULL || version->type != XAPI_JSON_VALUE_STRING || version->as.stringValue == NULL ||
            files == NULL || files->type != XAPI_JSON_VALUE_ARRAY) {
            continue;
        }

        if (latest_version != NULL && strcmp(version->as.stringValue, latest_version) != 0) {
            xapi_JsonValue *is_latest = xapi_jsonObjectGet(version_item, "isLatest");
            if (is_latest == NULL || is_latest->type != XAPI_JSON_VALUE_BOOL || is_latest->as.boolValue == 0) {
                continue;
            }
        }

        int file_count = xapi_jsonArraySize(files);
        for (int j = 0; j < file_count; ++j) {
            xapi_JsonValue *file_item = xapi_jsonArrayAt(files, j);
            if (file_item == NULL || file_item->type != XAPI_JSON_VALUE_OBJECT) {
                xapi_Printf((char *)"  错误：第 %d 个文件项无效。\n", j);
                return -1;
            }

            xapi_JsonValue *arch = xapi_jsonObjectGet(file_item, "arch");
            xapi_JsonValue *filename = xapi_jsonObjectGet(file_item, "name");
            xapi_JsonValue *type = xapi_jsonObjectGet(file_item, "type");
            if (arch == NULL || arch->type != XAPI_JSON_VALUE_STRING || arch->as.stringValue == NULL ||
                filename == NULL || filename->type != XAPI_JSON_VALUE_STRING || filename->as.stringValue == NULL) {
                xapi_Printf((char *)"  错误：第 %d 个文件元数据无效。\n", j);
                return -1;
            }

            if (type != NULL && type->type == XAPI_JSON_VALUE_STRING && type->as.stringValue != NULL) {
                xapi_Printf((char *)"  文件: 架构=%s, 类型=%s, 名称=%s\n", arch->as.stringValue,
                            type->as.stringValue, filename->as.stringValue);
            } else {
                xapi_Printf((char *)"  文件: 架构=%s, 名称=%s\n", arch->as.stringValue, filename->as.stringValue);
            }
        }

        return 0;
    }

    xapi_Printf((char *)"  错误：找不到最新版本的文件。\n");
    return -1;
}

static int nut_print_packages_json(const char *json_text)
{
    xapi_JsonDocument document;
    memset(&document, 0, sizeof(document));

    int parse_result = xapi_parseJson(json_text, &document);
    if (parse_result != XAPI_JSON_PARSE_OK) {
        xapi_Printf((char *)"错误：JSON 解析失败：%d\n", parse_result);
        return -1;
    }

    xapi_JsonValue *root = document.root;
    if (root == NULL || root->type != XAPI_JSON_VALUE_OBJECT) {
        xapi_Printf((char *)"错误：根节点不是对象\n");
        xapi_freeJsonDocument(&document);
        return -1;
    }

    xapi_JsonValue *packages = xapi_jsonObjectGet(root, "packages");
    if (packages == NULL || packages->type != XAPI_JSON_VALUE_ARRAY) {
        xapi_Printf((char *)"错误：packages 不是数组\n");
        xapi_freeJsonDocument(&document);
        return -1;
    }

    xapi_Printf((char *)"软件包:\n");
    int count = xapi_jsonArraySize(packages);
    for (int i = 0; i < count; ++i) {
        xapi_JsonValue *item = xapi_jsonArrayAt(packages, i);
        print_package_metadata(item, i);
    }

    xapi_freeJsonDocument(&document);
    return 0;
}

static int nut_install_package(const char *package_name)
{
    if (package_name == NULL || package_name[0] == '\0') {
        xapi_Printf((char *)"错误：软件包名称为空\n");
        return -1;
    }

    xapi_Printf((char *)"正在获取 Packages.json\n");
    char *index_body = xapi_httpGet("ftp.xingjisoft.com/Packages.json", 443, true);
    if (index_body == NULL) {
        xapi_Printf((char *)"错误：请求失败\n");
        return -1;
    }

    xapi_JsonDocument document;
    memset(&document, 0, sizeof(document));

    int parse_result = xapi_parseJson(index_body, &document);
    if (parse_result != XAPI_JSON_PARSE_OK) {
        xapi_Printf((char *)"错误：JSON 解析失败：%d\n", parse_result);
        free(index_body);
        return -1;
    }

    xapi_JsonValue *root = document.root;
    if (root == NULL || root->type != XAPI_JSON_VALUE_OBJECT) {
        xapi_Printf((char *)"错误：根节点不是对象\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    xapi_JsonValue *packages = xapi_jsonObjectGet(root, "packages");
    if (packages == NULL || packages->type != XAPI_JSON_VALUE_ARRAY) {
        xapi_Printf((char *)"错误：packages 不是数组\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    xapi_JsonValue *selected_package = NULL;
    int             package_count    = xapi_jsonArraySize(packages);
    for (int i = 0; i < package_count; ++i) {
        xapi_JsonValue *item = xapi_jsonArrayAt(packages, i);
        if (item == NULL || item->type != XAPI_JSON_VALUE_OBJECT) {
            continue;
        }

        xapi_JsonValue *name = xapi_jsonObjectGet(item, "name");
        if (name == NULL || name->type != XAPI_JSON_VALUE_STRING || name->as.stringValue == NULL) {
            continue;
        }

        if (strcmp(name->as.stringValue, package_name) == 0) {
            selected_package = item;
            break;
        }
    }

    if (selected_package == NULL) {
        xapi_Printf((char *)"错误：找不到软件包：%s\n", package_name);
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    xapi_JsonValue *latest = xapi_jsonObjectGet(selected_package, "latest");
    const char     *latest_version = NULL;
    if (latest != NULL && latest->type == XAPI_JSON_VALUE_STRING && latest->as.stringValue != NULL) {
        latest_version = latest->as.stringValue;
    }

    xapi_JsonValue *versions = xapi_jsonObjectGet(selected_package, "versions");
    if (versions == NULL || versions->type != XAPI_JSON_VALUE_ARRAY) {
        xapi_Printf((char *)"错误：versions 不是数组\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    xapi_JsonValue *selected_version = NULL;
    int             versions_count   = xapi_jsonArraySize(versions);
    for (int i = 0; i < versions_count; ++i) {
        xapi_JsonValue *version_item = xapi_jsonArrayAt(versions, i);
        if (version_item == NULL || version_item->type != XAPI_JSON_VALUE_OBJECT) {
            continue;
        }

        xapi_JsonValue *version = xapi_jsonObjectGet(version_item, "version");
        xapi_JsonValue *files   = xapi_jsonObjectGet(version_item, "files");
        if (version == NULL || version->type != XAPI_JSON_VALUE_STRING || version->as.stringValue == NULL ||
            files == NULL || files->type != XAPI_JSON_VALUE_ARRAY) {
            continue;
        }

        if (latest_version != NULL && strcmp(version->as.stringValue, latest_version) == 0) {
            selected_version = version_item;
            break;
        }
    }

    if (selected_version == NULL) {
        for (int i = 0; i < versions_count; ++i) {
            xapi_JsonValue *version_item = xapi_jsonArrayAt(versions, i);
            if (version_item == NULL || version_item->type != XAPI_JSON_VALUE_OBJECT) {
                continue;
            }

            xapi_JsonValue *files = xapi_jsonObjectGet(version_item, "files");
            if (files == NULL || files->type != XAPI_JSON_VALUE_ARRAY) {
                continue;
            }

            xapi_JsonValue *is_latest = xapi_jsonObjectGet(version_item, "isLatest");
            if (is_latest != NULL && is_latest->type == XAPI_JSON_VALUE_BOOL && is_latest->as.boolValue != 0) {
                selected_version = version_item;
                break;
            }
        }
    }

    if (selected_version == NULL) {
        xapi_Printf((char *)"错误：找不到软件包的最新版本：%s\n", package_name);
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    xapi_JsonValue *version_name = xapi_jsonObjectGet(selected_version, "version");
    xapi_JsonValue *files        = xapi_jsonObjectGet(selected_version, "files");
    if (version_name == NULL || version_name->type != XAPI_JSON_VALUE_STRING || version_name->as.stringValue == NULL ||
        files == NULL || files->type != XAPI_JSON_VALUE_ARRAY) {
        xapi_Printf((char *)"错误：选中的版本元数据无效\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    const char *selected_filename = NULL;
    int         files_count        = xapi_jsonArraySize(files);
    for (int i = 0; i < files_count; ++i) {
        xapi_JsonValue *file_item = xapi_jsonArrayAt(files, i);
        if (file_item == NULL || file_item->type != XAPI_JSON_VALUE_OBJECT) {
            continue;
        }

        xapi_JsonValue *file_name = xapi_jsonObjectGet(file_item, "name");
        if (file_name == NULL || file_name->type != XAPI_JSON_VALUE_STRING || file_name->as.stringValue == NULL) {
            continue;
        }

        if (selected_filename == NULL) {
            selected_filename = file_name->as.stringValue;
        }

        xapi_JsonValue *arch = xapi_jsonObjectGet(file_item, "arch");
        if (arch != NULL && arch->type == XAPI_JSON_VALUE_STRING && arch->as.stringValue != NULL &&
            strcmp(arch->as.stringValue, "x86_64") == 0) {
            selected_filename = file_name->as.stringValue;
            break;
        }
    }

    if (selected_filename == NULL) {
        xapi_Printf((char *)"错误：软件包没有可下载文件：%s\n", package_name);
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    char download_path[768];
    int  download_path_len = snprintf(download_path, sizeof(download_path),
                                      "/files/%s/%s/%s",
                                      package_name,
                                      version_name->as.stringValue,
                                      selected_filename);
    if (download_path_len <= 0 || (size_t)download_path_len >= sizeof(download_path)) {
        xapi_Printf((char *)"错误：下载路径过长\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    char target_path[512];
    int  path_len = snprintf(target_path, sizeof(target_path), "/apps/thirdparty/%s", selected_filename);
    if (path_len <= 0 || (size_t)path_len >= sizeof(target_path)) {
        xapi_Printf((char *)"错误：目标路径过长\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    xapi_Printf((char *)"正在下载 https://ftp.xingjisoft.com%s\n", download_path);
    char   *file_body       = NULL;
    size_t  file_size       = 0;
    int     download_result = nut_download_binary_body("ftp.xingjisoft.com", 443, true, download_path, &file_body, &file_size);
    if (download_result != 0 || file_body == NULL || file_size == 0) {
        xapi_Printf((char *)"错误：下载失败\n");
        xapi_freeJsonDocument(&document);
        free(index_body);
        return -1;
    }

    int write_result = (int)xapi_WriteFile(target_path, file_body, (UINT64)file_size, 0);
    free(file_body);
    xapi_freeJsonDocument(&document);
    free(index_body);

    if (write_result < 0) {
        xapi_Printf((char *)"错误：写入失败：%s\n", target_path);
        return -1;
    }

    xapi_Printf((char *)"已安装 %s（%u 字节）-> %s\n", package_name, (unsigned int)file_size, target_path);
    return 0;
}

static int nut_main_impl(int argc, char *argv[], char *envp[])
{
    (void)envp;
    xapi_OutputSerial("nut：已启动");

    if (argc < 2 || argv == NULL || argv[1] == NULL) {
        xapi_Printf((char *)"Nut 包工具已启动。可用命令：fetch、install <软件包名>\n");
        return 0;
    }

    const char *command = argv[1];

    if (strcmp(command, "fetch") == 0) {
        xapi_OutputSerial("nut：开始获取索引");
        xapi_Printf((char *)"正在获取 Packages.json\n");
        char *body = xapi_httpGet("ftp.xingjisoft.com/Packages.json", 443, true);
        if (body == NULL) {
            xapi_Printf((char *)"错误：请求失败\n");
            return 1;
        }

        int result = nut_print_packages_json(body);

        free(body);
        return result == 0 ? 0 : 1;
    }
    else if (strcmp(command, "install") == 0) {
        if (argc < 3 || argv[2] == NULL) {
            xapi_Printf((char *)"用法：nut install <软件包名>\n");
            return 1;
        }

        int result = nut_install_package(argv[2]);
        return result == 0 ? 0 : 1;
    }

    xapi_Printf((char *)"未知命令：%s\n", command);
    return 1;
}

extern "C" int nut_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int nut_main_cpp(int argc, char *argv[], char *envp[])
{
    return nut_main_impl(argc, argv, envp);
}
