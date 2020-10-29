#!/usr/bin/env bash

set -euo pipefail
IFS=$'\n\t'

cat <<EOF
language: c
dist: xenial
cache: ccache
services:
  - docker

matrix:
  include:
EOF

function emit
{
    local travis_arch="$1"
    local kernel_arch="$2"
    local sub_arch="$3"
    local target="$4"

    cat <<EOF
    - os:   linux
      arch: $travis_arch
      env:  TARGET=selftests ARCH=$kernel_arch SUBARCH=$sub_arch IMAGE=ubuntu-20.04 INSTALL=1 TARGETS=$target
      name: "selftests/$target: $sub_arch"
EOF
}

function skip
{
    local kernel_arch="$1"
    local target="$2"

    cat <<EOF
    # skipping $target on $kernel_arch
EOF
}

function header
{
    cat <<EOF
    ########################################
    # $1
    ########################################
EOF
}

travis_arch="ppc64le"
sub_arch="ppc64le"
kernel_arch="powerpc"

header "$sub_arch"

for target in $(awk '/TARGETS \+?=/ {print $3}' tools/testing/selftests/Makefile)
do
    case "$target" in
	"arm64")	;&
	"android")	;&
	"intel_pstate")	;&
	"kexec")	;&
	"filesystems/binderfs") ;&
	"sparc64")	;&
	"x86")
	    skip "$sub_arch" "$target"
	    continue ;;
    esac

    emit "$travis_arch" "$kernel_arch" "$sub_arch" "$target"
done

travis_arch="amd64"
kernel_arch="x86"
sub_arch="x86_64"

header "$sub_arch"

for target in $(awk '/TARGETS \+?=/ {print $3}' tools/testing/selftests/Makefile)
do
    case "$target" in
	"arm64")	;&
	"powerpc")	;&
	"sparc64")
	    skip "$sub_arch" "$target"
	    continue ;;
    esac

    emit "$travis_arch" "$kernel_arch" "$sub_arch" "$target"
done


cat <<EOF

install:
  - docker pull linuxppc/build:\$IMAGE-\$(uname -m)

script:
  - mkdir -p \$HOME/.ccache
  - travis_wait 50 ./arch/powerpc/tools/ci-build.sh
  - find \$HOME/output
EOF
