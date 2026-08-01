# NETSERVER KMOD KNOWLEDGE BASE

## OVERVIEW
Loadable networking module that integrates first-party socket/netdev glue with vendored lwIP sources.

## STRUCTURE
```
kmod/netserver/
├── netserver.cpp   # Module orchestration, sockets, DHCP/manual IPv4, DNS
├── arch/           # lwIP OS-port glue for scheduler/mutex/time
└── lwip/           # Vendored lwIP stack; avoid local feature edits
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Module build | `tools/gen_ninja.py` kmod section | Produces `out/netserver.sys`. |
| Socket/provider glue | `netserver.cpp` | Main first-party networking behavior. |
| lwIP OS port | `arch/sys_arch.cpp`, `arch/utils.cpp` | Scheduler/mutex/timer bridge. |
| Network ABI | `include/net/`, `include/netdev.h` | Shared with kernel/user callers. |
| lwIP config | `lwip/include/`, local includes | Treat as vendored/config integration. |

## CONVENTIONS
- C lwIP sources compile with `gnu11`; first-party glue compiles as freestanding GNU++17 PIC.
- `netserver.cpp` owns module-level state: provider registration, DHCP readiness, manual IPv4, DNS override, log counters.
- Default DNS is `223.5.5.5`; image staging also writes this to `/etc/resolv.conf`.
- The network device path depends on `netdev_t` callbacks and E1000/module registration order.
- `arch/sys_arch.cpp` is the correct layer for lwIP mutex/protect/scheduler semantics; do not scatter OS-port behavior through lwIP core files.
- Document lwIP integration boundaries here, not inside `lwip/` child docs.

## ANTI-PATTERNS
- Do not edit `lwip/**` for project policy when `netserver.cpp`, `arch/`, or config can carry the change.
- Do not enable lwIP PPP options marked unsupported (`CBCP_SUPPORT`, `ECP_SUPPORT`, `DEMAND_SUPPORT`).
- Do not call lwIP internal PPP entry points from user code.
- Do not assume chained pbuf support where lwIP ZEP code asserts `p->next == NULL`.

## VERIFY
Build `python3 tools/gen_ninja.py --out build.ninja && ninja -f build.ninja out/netserver.sys` or `ninja -f build.ninja all`. Runtime changes require booting with modules enabled and exercising `netcfg`, `httpget`, `browser`, or socket-using apps.
