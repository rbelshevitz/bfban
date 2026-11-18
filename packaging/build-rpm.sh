#!/usr/bin/env bash
set -Eeuo pipefail

# Build the release RPM on RHEL 10 or Rocky Linux 10.
#
# The project can be compiled on EL9, but bpfilter needs a 6.6+ running
# kernel.  This script therefore targets EL10 deliberately: the stock EL10
# kernel is new enough for runtime validation and bpfilter is available from
# EPEL 10.
#
# Native build:
#   ./packaging/build-rpm.sh
#
# Clean Mock build (the host does not need to be EL10):
#   ./packaging/build-rpm.sh --mock epel-10-x86_64
#
# Required native build dependencies are installed by the caller, not by this
# script.  On Rocky 10, enable EPEL and install at least:
#   rpm-build gcc make pkgconf-pkg-config systemd-rpm-macros bpfilter-devel

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
spec_file="$script_dir/bfban.spec"
dist_dir="$repo_dir/dist"
build_top="$dist_dir/el10-rpmbuild"
mock_config=

usage() {
    cat <<'EOF'
Usage: packaging/build-rpm.sh [--mock [CONFIG]]

Build bfban RPMs for RHEL 10 / Rocky Linux 10.

Without --mock, the script must run on an EL10 host and uses rpmbuild
directly. With --mock, it first creates an SRPM and then rebuilds it in the
selected Mock configuration (default: epel-10-x86_64).
EOF
}

while (($#)); do
    case "$1" in
        --mock)
            mock_config=${2:-epel-10-x86_64}
            if [[ $# -gt 1 && "${2:-}" != --* ]]; then
                shift
            fi
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "build-rpm.sh: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

[[ -f "$spec_file" ]] || {
    echo "build-rpm.sh: spec file not found: $spec_file" >&2
    exit 1
}

command -v rpmbuild >/dev/null 2>&1 || {
    echo "build-rpm.sh: rpmbuild is required" >&2
    exit 1
}

if [[ -z "$mock_config" ]]; then
    # Do not accidentally produce an EL9 RPM that installs but cannot run
    # bpfilter on the stock EL9 kernel.
    # shellcheck disable=SC1091
    source /etc/os-release
    case "${ID:-}:${VERSION_ID%%.*}" in
        rocky:10|rhel:10|centos:10|almalinux:10) ;;
        *)
            echo "build-rpm.sh: native builds require RHEL/Rocky/compatible EL10" >&2
            echo "Use --mock epel-10-x86_64 from another host, or build on EL10." >&2
            exit 1
            ;;
    esac
else
    command -v mock >/dev/null 2>&1 || {
        echo "build-rpm.sh: mock is required for --mock" >&2
        exit 1
    }
fi

version=$(awk '$1 == "Version:" { print $2; exit }' "$spec_file")
release=$(awk '$1 == "Release:" { print $2; exit }' "$spec_file")
release=${release%%%\{?dist\}*}
[[ -n "$version" && -n "$release" ]] || {
    echo "build-rpm.sh: could not read Version/Release from $spec_file" >&2
    exit 1
}

mkdir -p "$dist_dir" "$build_top"/{BUILD,BUILDROOT,RPMS,SRPMS,SOURCES,TMP}
rm -f "$build_top"/SOURCES/bfban-"$version".tar.gz

# Build the source archive from Git's tracked files. This includes local
# edits to tracked files while excluding build products, AGENTS.md, TODO.md,
# dist/, and other untracked workspace material.
tar_file="$build_top/SOURCES/bfban-$version.tar.gz"
git -C "$repo_dir" ls-files -z \
    | tar -C "$repo_dir" --null --transform="s,^,bfban-$version/," \
        -czf "$tar_file" --files-from=-

if [[ -n "$mock_config" ]]; then
    # Build only the SRPM locally; Mock performs the actual EL10 build in its
    # clean chroot and resolves bpfilter-devel from the configured repositories.
    rpmbuild --define "_topdir $build_top" --define "_tmppath $build_top/TMP" \
        -bs "$spec_file"
    srpm="$build_top/SRPMS/bfban-$version-$release.src.rpm"
    result_dir="$dist_dir/mock-$version"
    rm -rf "$result_dir"
    mkdir -p "$result_dir"
    mock -r "$mock_config" --resultdir "$result_dir" --rebuild "$srpm"
    find "$result_dir" -maxdepth 1 -type f -name '*.rpm' \
        ! -name '*.src.rpm' -exec cp -f {} "$dist_dir"/ \;
    cp -f "$srpm" "$dist_dir"/
else
    # Native EL10 build. The %check section runs the project's basic tests;
    # integration tests requiring a real bpfilter datapath remain a separate
    # runtime validation step.
    rpmbuild --define "_topdir $build_top" --define "_tmppath $build_top/TMP" \
        -ba "$spec_file"
    find "$build_top/RPMS" "$build_top/SRPMS" -maxdepth 2 -type f \
        \( -name '*.rpm' -o -name '*.src.rpm' \) -exec cp -f {} "$dist_dir"/ \;
fi

echo "RPM build completed. Artifacts are in: $dist_dir"
