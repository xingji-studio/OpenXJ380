#include <cpu/fpu.h>

void save_fpu_context(fpu_context_t *ctx)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(ctx->fxsave_area) : "memory");
}

void restore_fpu_context(fpu_context_t *ctx)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(ctx->fxsave_area) : "memory");
}
