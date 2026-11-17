# TODO

- Add per-jail namespaces so multiple Fail2ban jails can coexist safely.
- Add CIDR-aware feed import and atomic batch updates.
- Replace fixed-size daemon storage with a bounded heap-backed database.
- Add benchmark results against nftables sets and legacy iptables.
- Replace the current AF_PACKET `bfobserve` collector with per-source eBPF telemetry using bounded LRU maps and top-N export.
- Add safe detection policies that never auto-ban without an allowlist and rate threshold.
- Add integration tests for daemon restart, TTL expiry, IPv4, IPv6, and CIDR.
- Add systemd hardening and a packaged RPM/DEB installation path.
- Add feed provenance, refresh intervals, allowlists, and rollback support.
