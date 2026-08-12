# [Security] Complete credential migration, trust policy, dependency upgrades, and fuzz regression

## Background

The PoC-driven hardening pass fixed or directly mitigated the reachable memory-safety issues in the ELF/module loader, GPT/MBR parser, SXAH terminal calls, Runfile parser, user ELF mapping, symlink/full-path handling, kernel RNG wrappers and image resize wrappers. It also changed CLI boot to a low-privilege login session and added failed-login backoff.

Several security properties cannot be completed safely as a small compatibility patch. This issue tracks them explicitly so the current hardening is not mistaken for complete closure of the audit.

## Remaining risk

1. `usereg.dat` persists plaintext passwords and is not authenticated. A writable registry can still be forged into an Admin record. The new code fails closed on a malformed existing file and prevents OOBE overwrite, but it does not protect a syntactically valid forged registry.
2. Kernel modules are parser-hardened but unsigned. Loading an attacker-controlled valid `.sys` is still arbitrary ring-0 execution by design.
3. On CPUs without `RDSEED`/`RDRAND`, RNG initialization has no formal entropy-readiness guarantee.
4. TLS/chat code is outside the current CLI build and still needs a proper entropy callback and authenticated peer protocol before re-enablement.
5. libvterm and other vendored components need an inventory-driven upgrade and compatibility pass.
6. The new parser boundaries need continuous fuzz regression, not only host mirrors of the old vulnerable snippets.

## Proposed work

- [ ] Define a versioned, pointer-free credential record containing magic, version, record length, username, role, per-user random salt, password KDF parameters and password verifier.
- [ ] Use a reviewed password KDF; implement constant-time verifier comparison and zero temporary password buffers.
- [ ] Write an atomic migration path for legacy `usereg.dat`: authenticate once against the legacy record, write the new format to a temporary node, sync, then replace; never convert malformed data into OOBE.
- [ ] Enforce registry owner/mode checks and reject Root/System roles from disk records.
- [ ] Add per-account plus global login throttling with monotonic time and auditable failure counters.
- [ ] Define the kernel-module trust root, signature format, signed byte range, rollback/version policy and recovery-mode behavior.
- [ ] Refuse automatic module execution unless signature verification succeeds; keep an explicit developer-mode override visibly tainted.
- [ ] Add an entropy pool/readiness state, mix interrupt/device timing and hardware entropy, and define blocking/non-blocking `getrandom` semantics before readiness.
- [ ] Replace TLS entropy callbacks with the kernel random API and design authenticated chat handshake/replay protection before those apps return to the build.
- [ ] Inventory exact vendored versions and configurations, choose supported upgrades, rebuild artifacts from source, and attach upstream test results.
- [ ] Add ASAN/UBSAN fuzz targets for ELF headers/segments/dynamic tags/relocations, GPT/MBR tables, symlink chains, Runfile records and image dimensions.
- [ ] Add boot tests proving that unauthenticated shell tasks are Visitor, malformed registry cannot trigger account replacement, and successful login changes the effective user.

## Acceptance criteria

- No plaintext password or reusable equivalent exists in the registry or user-list API.
- Legacy credentials migrate without account reset; malformed/forged records fail closed.
- Unsigned or modified modules are rejected before mapping or initializer execution.
- RNG readiness and `getrandom` behavior are documented and tested on hardware-RNG and fallback paths.
- TLS/chat cannot be enabled with deterministic entropy or unauthenticated peers.
- Dependency versions and applicable advisories are recorded in the repository with reproducible build inputs.
- Original PoCs plus new negative/fuzz regression tests pass in CI, and the kernel boots through login in QEMU.

## Already verified in the hardening patch

- 7/7 build-graph unit tests pass.
- Changed kernel, CLI and image translation units compile with project flags.
- ChaCha20 passes the supplied RFC 8439 and state/reseed harness.
- Full kernel compilation succeeds; the reduced CLI graph still has pre-existing serial/formatting linker gaps to resolve separately.
