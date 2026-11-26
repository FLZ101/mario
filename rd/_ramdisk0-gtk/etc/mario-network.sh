# GTK boots default to QEMU_NET=nat, so bring eth0 up with DHCP in the
# background.  Several getty login shells source /etc/profile concurrently;
# the mkdir is atomic and ensures only one shell starts the network service.
if mkdir /tmp/.mario-eth0-lock 2>/dev/null; then
    /sbin/netctl.exe dhcp start >/dev/null 2>&1 &&
        /sbin/netctl.exe dns auto >/dev/null 2>&1 &
fi
