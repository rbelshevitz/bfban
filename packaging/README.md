# RPM packaging for EL10

This directory contains the RPM spec and the reproducible build helper for
RHEL 10 and Rocky Linux 10.

The target is intentionally EL10. EL9 can compile the userspace binaries, but
its stock 5.14 kernel is below bpfilter's 6.6+ runtime requirement. Use EL10
for the package build and runtime validation, or use a separately managed
6.6+ kernel only for experiments.

## Native Rocky Linux 10 build

Enable EPEL and install the build dependencies:

```sh
sudo dnf install -y epel-release
sudo dnf install -y rpm-build gcc make pkgconf-pkg-config \
    systemd-rpm-macros bpfilter-devel
```

Build the RPMs:

```sh
./packaging/build-rpm.sh
```

The script refuses native builds on EL9 or another non-EL10 host, so an
accidental `.el9` package is not produced as a release artifact.

## Clean Mock build

On a non-EL10 host, install Mock and use an EL10 configuration with EPEL
enabled:

```sh
sudo dnf install -y mock
./packaging/build-rpm.sh --mock epel-10-x86_64
```

If the local Mock installation uses another EL10 configuration, pass its name
instead. Mock creates the SRPM first and performs the binary build in the
clean chroot.

## Build products

The `dist/` directory receives:

- `bfban-*.rpm` — daemon, CLI tools, service unit, and configuration;
- `bfban-fail2ban-*.rpm` — optional Fail2ban action package;
- `bfban-*.src.rpm` — source package for rebuilding on another EL10 host.

The Fail2ban action is a separate subpackage so the base package does not
force-install Fail2ban.

## Installation behavior

The spec uses the standard RHEL systemd RPM macros. Installation does not
enable or start `bfband`; service activation remains an explicit administrator
choice:

```sh
sudo dnf install ./bfban-0.1.0-1.el10.x86_64.rpm
# Optional, if Fail2ban is used:
sudo dnf install ./bfban-fail2ban-0.1.0-1.el10.x86_64.rpm
sudo systemctl enable --now bfband.service
```

The package does not manually create `/run` or `/var/lib` state directories.
The unit's `RuntimeDirectory=` and `StateDirectory=` directives let systemd
create and manage them. Configuration files are marked `%config(noreplace)`.

The `%check` phase runs the userspace and fault-injection tests. Real packet
path validation still belongs on an EL10 host with bpfilter, the required BPF
filesystem, and root privileges.
