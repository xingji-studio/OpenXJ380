# Security remediation status

This document records the remediation performed against the supplied host-side PoC bundle. A checked item means the vulnerable first-party path was changed and compiled; it does not replace runtime fuzzing or release qualification.

## Fixed or directly mitigated

- Dynamic linker: validates ELF/program-header/file ranges and module address-space bounds before mapping; validates dynamic termination, symbol/hash/string ranges, relocation tables, symbol indices and relocation write targets; bounds module names.
- GPT/MBR parser: caps global and per-disk partition counts, checks multiplication/LBA arithmetic, disk bounds and allocation results, and removes the duplicate unsafe GPT path.
- SXAH terminal calls: copy through the current process page table and never call `strlen`, `strncpy` or `memcpy` directly on a user pointer.
- Runfile dispatch: rejects oversized registries, uses a zeroed heap object, checks short reads and string termination, closes the VFS node on every path.
- User ELF loader: rejects truncated headers, overflowing program-header tables, kernel/null-page segment addresses, oversized mappings and entry points outside executable load segments.
- VFS symlinks and aliases: limits recursive resolution to 40 links.
- Full-path construction: dynamically sizes the result instead of writing into a fixed 256-byte buffer.
- Authentication flow: boots the shell as a Visitor login session, requires setup/login before the command loop, rate-limits failed logins, suppresses passwords from user-list results, and refuses to overwrite a malformed existing registry during OOBE.
- Kernel random API: replaces per-call xorshift streams with a locked persistent ChaCha20 generator, hardware `RDSEED`/`RDRAND` input when available, periodic reseeding and counter-wrap handling. Both `getrandom` and `/dev/urandom` use the shared generator.
- Image wrappers: reject zero/negative dimensions and checked-size overflow before allocation or resize.
- CLI build cleanup: retains keyboard modifier/input behavior without references to removed GUI-only shortcut routing.

## Still open

- `usereg.dat` still stores passwords in plaintext and has no authenticated on-disk format. It needs a versioned migration to salted password hashes plus an integrity/ownership policy; existing credentials must be migrated without silently resetting accounts.
- Module loading still has no signature/trust policy. Parser hardening reduces memory-corruption exposure, but untrusted kernel modules remain equivalent to arbitrary kernel code.
- The ChaCha20 fallback on CPUs without hardware entropy still mixes timing and address state; a boot entropy readiness model and additional device/interrupt entropy are required before claiming cryptographic guarantees on those machines.
- Userland TLS/chat entropy and peer authentication from PoC item 10 are not in the current CLI build graph and remain to be redesigned before enabling those applications.
- Vendored libvterm and other dependency findings require version inventory, upgrade selection and compatibility testing. No dependency CVE is marked fixed by this patch.
- The in-tree font validator remains the boundary for the mitigated stb_truetype issue. Direct callers must not bypass it.
- ELF and image changes need sustained ASAN/UBSAN/libFuzzer coverage in CI; the supplied PoCs are mirrors of the old logic, not end-to-end kernel tests.

## Verification performed

- `python -m unittest discover -s tests -p 'test_*.py'`: 7/7 passed.
- All changed kernel objects compiled with the generated freestanding Clang rules in an isolated Ubuntu container.
- `user/cli_shell.cpp` compiled with the project userland flags (Clang substituted for unavailable `g++`).
- `graphics/image/image.cpp` compiled with the kernel freestanding flags; only existing macro-redefinition warnings were emitted.
- Supplied ChaCha20 remediation harness passed the RFC 8439 block vector, consecutive-output, non-degenerate 4 KiB, 1 MiB reseed and non-repeating-block tests.
- The full kernel reached the final linker step. Linking remains blocked by pre-existing missing serial/formatting implementations in the reduced CLI graph; no newly added security symbol was reported unresolved.
