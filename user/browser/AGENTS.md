# BROWSER KNOWLEDGE BASE

## OVERVIEW
Browser user app and integration layer. Distinct from ordinary apps because it uses custom startup, hosted C/C++ flags, TLS, litehtml/gumbo, and libwebp decoding.

## STRUCTURE
```
user/browser/
├── browser_start.cpp        # Custom xapi startup/init-array/TLS path
├── browser_app.cpp          # Main UI/render/navigation logic
├── browser_fetch.cpp        # Fetch/network integration
├── browser_image_decode.cpp # WebP/SVG/image decode glue
├── browser_platform.*       # XJ380 drawing/input/platform bridge
└── third_party/libwebp/     # Vendored libwebp source; avoid feature edits
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Build rules | `tools/gen_ninja.py` browser section | Pulls litehtml/gumbo/libwebp/mbedtls/std libs. |
| Startup | `browser_start.cpp` | Not the generic xapi startup path. |
| App/UI | `browser_app.cpp` | Browser state, URL/search/render loop. |
| Networking | `browser_fetch.cpp`, `../https_client.cpp`, `../http_tls_compat.cpp` | HTTP/TLS integration. |
| Rendering engine | `third_party/litehtml/` via root `third_party` | Vendored upstream engine. |
| Image decode | `browser_image_decode.cpp`, `third_party/libwebp/` | Project glue + vendored codec. |

## CONVENTIONS
- Browser links with `--gc-sections`, `--wrap=free`, libstdc++/libsupc++/libgcc groups, TLS objects, Mbed TLS, litehtml/gumbo, and libwebp.
- `browser_start.cpp` handles runtime init distinct from `xapi/start.cpp`; do not collapse the paths without checking constructor/TLS behavior.
- Browser hosted flags differ from normal user app freestanding flags; C++ is C++17 and C code is C11 with package include paths.
- Keep browser behavior in `user/browser/*.cpp`; avoid editing vendored litehtml/libwebp/gumbo internals unless syncing upstream.
- UI changes must be verified visually in QEMU, not by build alone.

## ANTI-PATTERNS
- Do not use generic user app link assumptions for browser; it has custom start object and stdlib/builtins requirements.
- Do not assume network availability at startup; netserver/e1000 modules and DHCP/manual config affect fetch behavior.
- Do not treat `user/browser/third_party/libwebp` as first-party style cleanup territory.
- Do not omit `BROWSER_BUILTINS` diagnosis if browser/httpget/nut link fails on compiler builtins.

## VERIFY
Build `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja out/browser.elf`; prefer `ninja -f build.ninja all` for dependency correctness. Runtime verify with QEMU and actual page/navigation/image fetch paths.
