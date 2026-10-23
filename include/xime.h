#pragma once
// Fixed-width ABI types avoid mixing the hosted and freestanding libc headers.
typedef unsigned int       XImeU32;
typedef unsigned long long XImeU64;
typedef int                XImeI32;
typedef long long          XImeResult;

#define XIME_ABI_VERSION     1U
#define XIME_MAX_INSTALLED   16U
#define XIME_ID_BYTES        48U
#define XIME_NAME_BYTES      64U
#define XIME_PATH_BYTES      256U
#define XIME_TEXT_BYTES      256U
#define XIME_MAX_CANDIDATES  9U
#define XIME_CANDIDATE_BYTES 64U

#define XIME_MODE_ENGLISH      0U
#define XIME_MODE_CHINESE      1U
#define XIME_CONTEXT_ENABLED   1U
#define XIME_CONTEXT_PASSWORD  2U
#define XIME_MOD_SHIFT         1U
#define XIME_MOD_CTRL          2U
#define XIME_MOD_ALT           4U
#define XIME_MOD_WIN           8U
#define XIME_MOD_CAPS          16U
#define XIME_EVENT_KEY         1U
#define XIME_EVENT_RESET       2U
#define XIME_EVENT_CANDIDATE   3U
#define XIME_REPLY_PASS        0U
#define XIME_REPLY_CONSUMED    1U
#define XIME_REPLY_COMMIT_UTF8 2U

// All requests are fixed-size v1 records. Reserved fields must be zero.
// A registration has no path/active fields: the kernel derives the executable.
typedef struct XImeRegistration
{
    XImeU32 size, version;
    char    id[XIME_ID_BYTES];
    char    name[XIME_NAME_BYTES];
} XImeRegistration;

typedef struct XImeInfo
{
    char id[XIME_ID_BYTES];
    char name[XIME_NAME_BYTES];
    char executable[XIME_PATH_BYTES];
} XImeInfo;

typedef struct XImeList
{
    XImeU32  size, version, count, reserved;
    XImeInfo entries[XIME_MAX_INSTALLED];
} XImeList;

typedef struct XImeState
{
    XImeU32 size, version, mode, attached;
    char    active[XIME_ID_BYTES]; // Empty means direct input.
} XImeState;

typedef struct XImeSelect
{
    XImeU32 size, version;
    char    id[XIME_ID_BYTES];
} XImeSelect;

typedef struct XImeSession
{
    XImeU32 size, version;
    XImeU64 session;
} XImeSession;

typedef struct XImeEvent
{
    XImeU32 size, version;
    XImeU64 session, sequence, context;
    XImeU32 kind, modifiers, mode, reserved;
    XImeU64 message, value;
} XImeEvent;

typedef struct XImeReply
{
    XImeU32 size, version;
    XImeU64 session, sequence, context;
    XImeU32 action, reserved;
    char    text[XIME_TEXT_BYTES];
} XImeReply;

typedef struct XImeUi
{
    XImeU32 size, version;
    XImeU64 session, context, sequence;
    XImeU32 visible, selected, count, page;
    char    preedit[XIME_TEXT_BYTES];
    char    candidates[XIME_MAX_CANDIDATES][XIME_CANDIDATE_BYTES];
} XImeUi;

typedef struct XImeMode
{
    XImeU32 size, version;
    XImeU64 session;
    XImeU32 mode, reserved;
} XImeMode;

typedef struct XImeContext
{
    XImeU32 size, version;
    XImeU64 window, control;
    XImeU32 flags, reserved;
    XImeI32 caret_x, caret_y; // Window content coordinates.
} XImeContext;

#ifdef __cplusplus
extern "C" {
#endif
XImeResult xapi_ImeRegister(const XImeRegistration *request);
XImeResult xapi_ImeEnum(XImeList *list);
XImeResult xapi_ImeGetState(XImeState *state);
XImeResult xapi_ImeSetActive(const XImeSelect *request);
XImeResult xapi_ImeUnregister(const XImeSelect *request);
XImeResult xapi_ImeAttach(XImeSession *session);
// Nonblocking: -EAGAIN when empty. Applications should sleep before retrying.
XImeResult xapi_ImeGetEvent(XImeEvent *event);
XImeResult xapi_ImeReplyEvent(const XImeReply *reply);
XImeResult xapi_ImeUpdateUi(const XImeUi *ui);
XImeResult xapi_ImeSetMode(const XImeMode *mode);
XImeResult xapi_ImeSetContext(const XImeContext *context);
#ifdef __cplusplus
}
static_assert(sizeof(XImeU32) == 4 && sizeof(XImeU64) == 8, "Unsupported IME ABI integer widths");
static_assert(sizeof(XImeRegistration) == 120 && sizeof(XImeInfo) == 368 && sizeof(XImeList) == 5904,
              "IME registry ABI layout changed");
static_assert(sizeof(XImeState) == 64 && sizeof(XImeSelect) == 56 && sizeof(XImeSession) == 16,
              "IME session ABI layout changed");
static_assert(sizeof(XImeEvent) == 64 && sizeof(XImeReply) == 296 && sizeof(XImeUi) == 880,
              "IME event ABI layout changed");
static_assert(sizeof(XImeMode) == 24 && sizeof(XImeContext) == 40, "IME context ABI layout changed");
#endif
