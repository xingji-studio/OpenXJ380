// SPDX-License-Identifier: Apache-2.0
#include <uacpi/internal/context.h>
#include <uacpi/internal/mutex.h>
#include <uacpi/uacpi.h>

// uACPI 7's public global-lock API is nonrecursive, even when an AML Lock
// field already owns _GL. Isolate this version-pinned ownership query here;
// never reacquire or release the interpreter's existing ownership in EC I/O.
_Static_assert(UACPI_MAJOR == 7 && UACPI_MINOR == 0 && UACPI_PATCH == 0,
               "review the AML global-lock bridge before updating uACPI");

uacpi_bool acpi_aml_owns_global_lock(void)
{
    return g_uacpi_rt_ctx.global_lock_mutex && uacpi_this_thread_owns_aml_mutex(g_uacpi_rt_ctx.global_lock_mutex);
}
