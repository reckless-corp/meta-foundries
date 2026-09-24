#!/usr/bin/env bash
set -euo pipefail

cd $(dirname $(readlink -f $0))

builddir=`pwd`/.build
export KAS_WORK_DIR="${builddir}/work"
export DL_DIR="${builddir}/downloads"
export SSTATE_DIR="${builddir}/sstate-cache"
mkdir -p "${DL_DIR}" "${SSTATE_DIR}" "${KAS_WORK_DIR}"

if (( $# < 1 || $# > 2 )); then
    echo "Usage: $0 <kas-file.yml> [action]"
    echo "Available kas files:" $(ls *.yml | grep -v common)
    exit 2
fi

kc="${builddir}/kas-container"
if [ ! -x ${kc} ] ; then
	wget -O ${kc} https://raw.githubusercontent.com/siemens/kas/refs/tags/5.5/kas-container
	chmod +x "${kc}"
fi

kasfile="$1"
action="${2:-build}"

if [ ! -f ${kasfile} ] ; then
	echo "ERROR: No such file ${kasfile}"
	exit 1
fi

set -x
exec ${kc} "${action}" "${kasfile}"
