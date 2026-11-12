// SPDX-License-Identifier: Apache-2.0
// Link one copy into each C++ native executable/DSO. Identity is image-local and
// stays valid until process exit; this ABI does not provide dlclose.
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;
