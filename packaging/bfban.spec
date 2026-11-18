Name:           bfban
Version:        0.1.0
Release:        1%{?dist}
Summary:        Dynamic IP/CIDR blocklist enforcement through bpfilter
License:        GPL-2.0-only
Packager:       Roman Belshevitz
URL:            https://github.com/rbelshevitz/bfban
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  systemd-rpm-macros
BuildRequires:  bpfilter-devel >= 0.6.0

Requires:       bpfilter >= 0.6.0
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
bfban is a Linux service for dynamic IPv4, IPv6, and CIDR blocklists.
The bfband daemon accepts ban decisions over a Unix socket, keeps persistent
state and optional TTLs, and applies the list through bpfilter/eBPF. It is an
enforcement backend for Fail2ban, CrowdSec, threat feeds, operators, and
custom security agents; it does not replace a stateful host firewall.

%package fail2ban
Summary:        Fail2ban action for bfban
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       fail2ban

%description fail2ban
Fail2ban action configuration for submitting ban and unban decisions to the
bfband Unix-socket daemon.

%prep
%autosetup

%build
./configure --prefix=%{_prefix}
%make_build

%check
%make_build test

%install
%make_install PREFIX=%{_prefix}

install -Dpm0644 build/etc/systemd/bfband.service \
    %{buildroot}%{_unitdir}/bfband.service
rm -f %{buildroot}%{_sysconfdir}/systemd/system/bfband.service

install -Dpm0644 README.md %{buildroot}%{_docdir}/%{name}/README.md
install -Dpm0644 README.ru.md %{buildroot}%{_docdir}/%{name}/README.ru.md

mkdir -p %{buildroot}%{_localstatedir}/lib/bfban

%pre
%systemd_pre bfband.service

%post
%systemd_post bfband.service

%preun
%systemd_preun bfband.service

%postun
%systemd_postun_with_restart bfband.service

%files
%license LICENSE COPYING
%doc README.md README.ru.md
%{_sbindir}/bfban
%{_sbindir}/bfband
%{_sbindir}/bfbandctl
%{_sbindir}/bfobserve
%config(noreplace) %{_sysconfdir}/bfban/bfban.env
%{_unitdir}/bfband.service
%dir %{_localstatedir}/lib/bfban

%files fail2ban
%config(noreplace) %{_sysconfdir}/fail2ban/action.d/bfban.conf

%changelog
* Wed Nov 18 2026 Roman Belshevitz - 0.1.0-1
- Add initial RHEL 10 and Rocky Linux 10 RPM packaging.
