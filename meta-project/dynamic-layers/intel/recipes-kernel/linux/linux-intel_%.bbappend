FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append:intel-corei7-64 = " file://vm-networking.cfg file://nfs-server.cfg file://kvm-amd.cfg file://task-io-accounting.cfg"
