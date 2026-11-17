# bfban roadmap and delivery status

This document is intentionally finite. It records what is already delivered
for v1 and the small set of release gates that still need validation. It is
not an open-ended feature wishlist.

## v1 scope

`bfban` v1 is a Linux dynamic L3/L4 enforcement backend for bpfilter. The
daemon is `bfband`; Fail2ban and other agents make decisions, while `bfband`
applies them to the bpfilter chain.

## Delivered

- [x] GPL-2.0-only project and source headers.
- [x] `bfban` CLI for `init`, `add`, `add-many`, `remove`, `list`, and `flush`.
- [x] IPv4 and IPv6 exact-address blocklists.
- [x] IPv4 and IPv6 CIDR support using separate prefix/LPM sets.
- [x] CIDR canonicalization and duplicate elimination.
- [x] Batch feed parsing, validation, grouping, and bounded update chunks.
- [x] Atomic state-file replacement and state-file locking for CLI mutations.
- [x] `bfband` Unix-socket daemon with TTLs and restart recovery.
- [x] Bounded daemon storage and an open-addressing lookup index.
- [x] Expired-entry cleanup with index rebuild.
- [x] Kernel/state reconciliation during daemon initialization.
- [x] Deferred retry for failed TTL removals instead of a busy loop.
- [x] Bounded two-second request reads for sequential Unix-socket clients.
- [x] Preflight capacity checks and best-effort kernel/state rollback.
- [x] State writes with file `fsync` and attempted directory `fsync`.
- [x] Symmetric direct/CLI rollback paths when `libbpfilter` is compiled in but
  runtime direct mode is disabled.
- [x] `FLUSH` clears both the entry table and the daemon hash index.
- [x] Partial batch rollback failures are reported and block further mutations
  until an explicit `INIT` reconciliation succeeds.
- [x] Failed persistence of expired-entry cleanup prevents an unsafe daemon
  startup.
- [x] Post-`rename()` directory-open and `fsync` failures are reported as
  durability warnings, not treated as pre-commit save failures.
- [x] CLI fallback batch rollback removes every address submitted to the kernel,
  including entries after the first failed command.
- [x] Failed restoration of the previous state marks reconciliation as needed.
- [x] Direct `libbpfilter` update path when the development package is present.
- [x] CLI fallback through `bfban` and `bfcli` when direct mode is unavailable.
- [x] Direct batch updates grouped by address family and exact/CIDR set.
- [x] Root-only Unix-socket peer validation and `SIGPIPE` handling.
- [x] `bfbandctl` client with single-address and batch operations.
- [x] Fail2ban action template with custom-prefix support.
- [x] systemd unit and generated runtime configuration.
- [x] `configure --prefix=...` and out-of-tree `build/` output.
- [x] `bfobserve` interface listing, top-N reporting, JSON, and suggestions.
- [x] Ingress-default observation with explicit egress/both modes.
- [x] VLAN handling, bounded IPv6 extension-header parsing, and safe packet reads.
- [x] Rocky Linux 10, Debian 13, and Ubuntu 24.04 validation procedure.
- [x] TCP backlog, buffer, SYN-cookie, and memory guidance.
- [x] English and Russian deployment, Fail2ban, rollback, and troubleshooting docs.
- [x] Test-only fault injection for backend calls and state-save boundaries,
  with explicit root-only `PASS`/`SKIP` reporting in `make test`.

## Release gates

These are validation gates, not new product features:

- [ ] Run the direct profile on a target host with packaged or source-installed
  `bpfilter` and record the exact kernel, bpfilter, and libbpf versions.
- [ ] Execute the full functional lab on Rocky Linux 10, Debian 13, and Ubuntu
  24.04, including exact IPv4/IPv6, CIDR, restart, rollback, and Fail2ban tests.
- [ ] Compare equivalent policies against nftables/iptables and record raw
  benchmark output before making a performance claim.
- [ ] Verify the installed systemd and Fail2ban files on a clean host with both
  `/usr/local` and a non-default prefix.

State changes use a compensating rollback because the kernel update and the
state-file rename cannot be one physical transaction. A failed rollback is
reported and the next daemon initialization reconciles the state with the
kernel chain.

## Explicitly outside v1

- Per-jail bpfilter namespaces.
- Automatic attack classification or automatic banning.
- Per-source XDP/eBPF telemetry maps.
- TPACKET_V3 or AF_XDP collector implementation.
- Upstream DDoS scrubbing or provider coordination.
- Stateful firewall, NAT, conntrack, or rate-limiting replacement.

These may be considered for a separate v2 design after v1 measurements; they
are not prerequisites for the daemon/Fail2ban release.
