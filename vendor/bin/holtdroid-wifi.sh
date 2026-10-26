#!/system/bin/sh

L="/vendor/holtdroid-net/lib/ld-musl-x86_64.so.1 --library-path /vendor/holtdroid-net/lib"
NFT="$L /vendor/holtdroid-net/nft"
HOSTAPD="$L /vendor/holtdroid-net/hostapd"
DIR=/data/vendor/wifi

unshare -n sleep 2147483647 &
ns=$!
echo $ns > $DIR/router.pid
R="nsenter --net=/proc/$ns/ns/net --"
until $R true; do sleep 0.1; done

ip link add wifiup type veth peer name wifiup-peer netns $ns
ip link add eth1 type veth peer name mobile0 netns $ns
ip addr add 10.254.0.1/30 dev wifiup
ip link set wifiup up
$R ip link set lo up
$R ip addr add 10.254.0.2/30 dev wifiup-peer
$R ip link set wifiup-peer up
$R ip route add default via 10.254.0.1
$R ip addr add 192.168.78.1/24 dev mobile0
$R sysctl -qw net.ipv4.ip_forward=1
$R $NFT -f - <<EOF
table ip holtdroid {
  chain postrouting {
    type nat hook postrouting priority srcnat;
    oifname "wifiup-peer" masquerade
  }
}
EOF
$NFT -f - <<EOF
table ip holtdroid {
  chain postrouting {
    type nat hook postrouting priority srcnat;
    ip saddr 10.254.0.0/30 oifname "eth0" masquerade
  }
}
EOF

dhcp="--interface=mobile0 --dhcp-range=192.168.78.10,192.168.78.50,1h"
if /vendor/bin/mac80211_create_radios 2 4660 && ap=$(cat /sys/class/net/wlan1/phy80211/name) && iw phy "$ap" set netns $ns; then
  wifi=1
  $R ip addr add 192.168.77.1/24 dev wlan1
  printf 'interface=wlan1\ndriver=nl80211\nssid=HoltdroidWifi\nhw_mode=g\nchannel=6\n' > $DIR/hostapd.conf
  $R $HOSTAPD $DIR/hostapd.conf &
  dhcp="$dhcp --interface=wlan1 --dhcp-range=192.168.77.10,192.168.77.50,1h"
else
  $R ip link set mobile0 up
fi
dns=$(sed -n 's/^nameserver \([0-9.]*\).*/\1/p' /etc/resolv.conf 2>/dev/null | head -1)
sleep 2147483647 | $R dnsmasq -k --no-resolv --port=0 --bind-interfaces $dhcp --dhcp-option=6,${dns:-1.1.1.1} \
  --dhcp-leasefile=$DIR/dnsmasq.leases --pid-file= --user=root &

until [ "$(getprop init.svc.netd)" = running ] && ndc ipfwd enable holtdroid >/dev/null 2>&1; do sleep 1; done
ip rule add iif wifiup lookup main pref 9500
ip rule add to 10.254.0.0/30 lookup main pref 9501
[ -z "$wifi" ] || setprop vendor.holtdroid.wifi 1
wait
