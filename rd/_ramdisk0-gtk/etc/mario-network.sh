# GTK boots default to QEMU_NET=nat, so bring eth0 up with DHCP in the
# background.  init runs /etc/initrc after mounting /usr; the initrc sources
# this file.  Keep the atomic mkdir guard so a future initrc restart cannot
# accidentally start a second DHCP service.
if mkdir /tmp/.mario-eth0-lock 2>/dev/null; then
    /sbin/netctl.exe dhcp start >/dev/null 2>&1 &&
        /sbin/netctl.exe dns auto >/dev/null 2>&1 &
fi
