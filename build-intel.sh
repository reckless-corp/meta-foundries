#!/usr/bin/env bash
set -euo pipefail

cd $(dirname $(readlink -f $0))

builddir=`pwd`/.build
export KAS_WORK_DIR="${builddir}/work"
export DL_DIR="${builddir}/downloads"
export SSTATE_DIR="${builddir}/sstate-cache"
mkdir -p "${DL_DIR}" "${SSTATE_DIR}" "${KAS_WORK_DIR}"

action="${1:-build}"
if (( $# > 0 )); then
    shift
fi

exec ./kas-container "${action}" intel.yml "$@"
