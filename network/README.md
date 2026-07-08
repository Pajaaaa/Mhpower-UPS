# Síťová brána před MHpower ESP — MikroTik mAP lite

ESP32 monitor MHpower nemá ethernet a na místě chybí kabel k síti. Místo toho je
před ním **MikroTik mAP lite (RBmAPL‑2nD, RouterOS 7.23.1)**, který dělá WiFi AP
pro ESP a NAT bránu do hkfree sítě.

Soubor [`mikrotik-map-mhpower.rsc`](mikrotik-map-mhpower.rsc) je živý `/export`
z toho zařízení (bez citlivých údajů — RouterOS v exportu skrývá WPA2 klíč, admin
heslo žádné není).

## Topologie

```
hkfree (veřejně 10.107.1.212)
   │  (NAT na nadřazené bráně 192.168.88.1)
   ▼
ether1 = WAN  (DHCP klient → 192.168.88.56/24)
   │
 mAP lite ── NAT (masquerade) ──┐
   │                            │ dst-nat
 wlan1 = LAN  AP "MH" (WPA2)    │  TCP/80  → 192.168.1.254:80   (web)
 192.168.1.1/24, DHCP server    │  UDP/161 → 192.168.1.254:161  (SNMP)
   │
   ▼
ESP32 MHpower  (MAC D8:13:2A:43:01:94)  →  pevná IP 192.168.1.254
```

Web i SNMP ESP jsou tak z hkfree dostupné na **`10.107.1.212:80`** a **`:161`**
jako kdyby byl ESP přímo v síti.

## Co konfig dělá

- **WAN (ether1):** DHCP klient — IP ze sítě automaticky.
- **LAN (wlan1):** AP `ssid=MH` (WPA2‑PSK), `192.168.1.0/24`, DHCP server,
  DNS klientům `10.107.4.100`.
- **Pevná IP pro ESP:** static DHCP lease `192.168.1.254` na MAC ESP.
- **dst‑nat:** web (TCP/80) a SNMP (UDP/161) z WAN na ESP.
- **Firewall:** WAN‑input se dropuje kromě established/related, ICMP, DHCP‑klienta
  a správy z `10.107.0.0/16` (address‑list `mgmt`); forward jen est/rel + dst‑nat
  + LAN→WAN. Vypnuté ftp/telnet/api/api‑ssl/reverse‑proxy; mac‑server a discovery
  jen na LAN, bandwidth‑server vypnutý.

## Reimport

```rsc
/import file=mikrotik-map-mhpower.rsc
```
(na čisté zařízení po `/system reset-configuration no-defaults=yes`).

## TODO / pozor

- **Admin je bez hesla** — nastavit `/user set admin password="…"` (zatím chrání
  jen firewall, který pouští správu pouze z `10.107.0.0/16`).
- WPA2 klíč WiFi je v `.rsc` jen jako placeholder `ZMENIT` (reálné heslo do gitu
  nepatří) — po importu nastavit ručně a musí sedět s ESP:
  `/interface wireless security-profiles set [find default=yes] wpa2-pre-shared-key="…"`
- Pokud dst‑nat pravidla po importu ukážou flag `I` (invalid), je to jen
  re‑evaluace po dávkovém přidání — `/ip firewall nat disable [find chain=dstnat]`
  a hned `enable` to spraví.
