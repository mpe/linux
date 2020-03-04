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
    local arch="$2"
    local sub_arch="$3"
    local target="$4"

    cat <<EOF
    - os:   linux
      arch: $travis_arch
      env:  TARGET=selftests ARCH=$arch SUBARCH=$sub_arch IMAGE=ubuntu-20.04 INSTALL=1 TARGETS=$target
      name: "selftests/$target: $sub_arch"
EOF
}

function skip
{
    local arch="$1"
    local target="$2"

    cat <<EOF
    # skipping $target on $arch
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
arch="ppc64le"

header "$arch"

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
	    skip "$arch" "$target"
	    continue ;;
    esac

    emit "$travis_arch" "$arch" "$arch" "$target"
done

travis_arch="amd64"
arch="x86_64"

header "$arch"

for target in $(awk '/TARGETS \+?=/ {print $3}' tools/testing/selftests/Makefile)
do
    case "$target" in
	"arm64")	;&
	"powerpc")	;&
	"sparc64")
	    skip "$arch" "$target"
	    continue ;;
    esac

    emit "$travis_arch" "$arch" "$arch" "$target"
done


cat <<EOF

install:
  - docker pull linuxppc/build:\$IMAGE-\$(uname -m)

script:
  - mkdir -p \$HOME/.ccache
  - travis_wait 50 ./arch/powerpc/tools/ci-build.sh
  - find \$HOME/output
EOF
