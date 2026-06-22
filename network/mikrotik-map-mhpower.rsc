# 2026-06-22 22:36:09 by RouterOS 7.23.1
# software id = XIH3-S5UJ
#
# model = RBmAPL-2nD
# serial number = HKB0AV1XWR0
/interface wireless
set [ find default-name=wlan1 ] band=2ghz-b/g/n disabled=no mode=ap-bridge \
    radio-name=mhpowerUPS ssid=MH
/interface list
add name=WAN
add name=LAN
/interface wireless security-profiles
set [ find default=yes ] authentication-types=wpa2-psk mode=dynamic-keys \
    supplicant-identity=MikroTik
/ip pool
add name=dhcp_pool0 ranges=192.168.1.2-192.168.1.254
/ip dhcp-server
add address-pool=dhcp_pool0 interface=wlan1 name=dhcp1
/ip neighbor discovery-settings
set discover-interface-list=LAN
/interface list member
add interface=ether1 list=WAN
add interface=wlan1 list=LAN
/ip address
add address=192.168.1.1/24 interface=wlan1 network=192.168.1.0
/ip dhcp-client
add default-route-tables=main interface=ether1 name=client1
/ip dhcp-server lease
add address=192.168.1.254 client-id=1:d8:13:2a:43:1:94 comment=\
    "MHpower ESP32" mac-address=D8:13:2A:43:01:94 server=dhcp1
/ip dhcp-server network
add address=192.168.1.0/24 dns-server=10.107.4.100 gateway=192.168.1.1
/ip firewall address-list
add address=10.107.0.0/16 comment=hkfree-mgmt list=mgmt
/ip firewall filter
add action=accept chain=input comment=est/rel connection-state=\
    established,related
add action=drop chain=input comment="drop invalid" connection-state=invalid
add action=accept chain=input comment=ICMP protocol=icmp
add action=accept chain=input comment="LAN full" in-interface-list=LAN
add action=accept chain=input comment=DHCP-client dst-port=68 \
    in-interface-list=WAN protocol=udp
add action=accept chain=input comment=mgmt-hkfree in-interface-list=WAN \
    src-address-list=mgmt
add action=drop chain=input comment=drop-WAN-input in-interface-list=WAN
add action=accept chain=forward comment=est/rel connection-state=\
    established,related
add action=drop chain=forward comment="drop invalid" connection-state=invalid
add action=accept chain=forward comment=dstnat-web-snmp-ESP \
    connection-nat-state=dstnat
add action=accept chain=forward comment=LAN-to-WAN in-interface-list=LAN \
    out-interface-list=WAN
add action=drop chain=forward comment=drop-forward-rest
/ip firewall nat
add action=masquerade chain=srcnat out-interface-list=WAN
add action=dst-nat chain=dstnat comment=web->MHpower dst-port=80 \
    in-interface-list=WAN protocol=tcp to-addresses=192.168.1.254 to-ports=80
add action=dst-nat chain=dstnat comment=snmp->MHpower dst-port=161 \
    in-interface-list=WAN protocol=udp to-addresses=192.168.1.254 to-ports=\
    161
/ip service
set ftp disabled=yes
set telnet disabled=yes
set reverse-proxy disabled=yes
set api disabled=yes
set api-ssl disabled=yes
/system clock
set time-zone-name=Europe/Prague
/system identity
set name=MHpowerUPS
/tool bandwidth-server
set enabled=no
/tool mac-server
set allowed-interface-list=LAN
/tool mac-server mac-winbox
set allowed-interface-list=LAN
/tool mac-server ping
set enabled=no
