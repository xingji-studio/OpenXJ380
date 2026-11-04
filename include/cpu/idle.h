#pragma once

// Runtime idle is reached only after the CPU's scheduler state is published.
// Preserve the boot/fatal IRQ gate instead of enabling interrupts through it.
static inline void cpu_idle_once(bool interrupts_forbidden)
{
    if (interrupts_forbidden)
    {
        __asm__ volatile("pause" ::: "memory");
        return;
    }

    // Start with IF clear even if idle resumed with IF set. STI's interrupt
    // shadow then covers the adjacent HLT, so a pending IRQ cannot be handled
    // just before the halt and leave this CPU asleep after its wakeup.
    __asm__ volatile("cli; sti; hlt" ::: "memory");
}
