# MHpower UPS monitor

Projektová přihláška dle Směrnice pro návrh, realizaci a dokumentaci projektů hkfree.org z.s. (v1.0.0).

## Popis

Monitoring zálohovaných zdrojů **MHpower MPU** (300–5000 W) používaných v síti hkfree.
Tyto zdroje nemají žádné management rozhraní — jediný výstup je lokální LED displej.
ESP32 **pasivně odposlouchává sběrnici displeje (TM1640)**, z rámců dekóduje vstupní/výstupní
napětí, stav sítě/baterie, zátěž a alarmy, a publikuje je přes **webové rozhraní** a **SNMP v1**.
Do zdroje se nijak nezasahuje — jen se „čte přes rameno" jeho vlastní displej.

Stav se centrálně sbírá do power monitoru spolku (power.hkfree.org), včetně trvalého logu
událostí (výpadky sítě, poruchy, restarty).

## Cíl projektu

Dohled nad záložními zdroji MHpower v síti bez zásahu do zařízení: včasná informace
o výpadku napájení, stavu a zdraví baterie, přetížení a poruchách — místo zjištění
„až když lokalita spadne".

## Přínos pro komunitu

- monitoring a observabilita páteřní infrastruktury (záložní napájení lokalit),
- náhrada proprietárního řešení — zdroje management nemají, tímto ho získávají,
- open-source přínos: kompletní reverse engineering protokolu TM1640 displeje MPU
  je zdokumentovaný a přenositelný,
- sdílení know-how (pasivní odposlech sběrnice, dekódování 7segmentových glyfů,
  odhad výdrže baterie Peukertovým modelem s učením),
- energetická efektivita: úsporný režim ESP (odběr jednotky minimalizován).

## Rozsah projektu

Etapy (1. etapa odpovídá limitu ~40 člověkohodin dle směrnice):

1. **Proof-of-concept** — odposlech a dekódování TM1640 rámců na jednom kuse,
   ověření logickým analyzátorem, HW zapojení přes Schmittův invertor 74LVC14A.
2. **Firmware pro provoz** — web dashboard, `/api/status`, SNMP v1 (enterprise OID
   1.3.6.1.4.1.53864), OTA aktualizace, odhad výdrže baterie, diagnostika restartů,
   read-only účet guest.
3. **Integrace do power monitoru** — SNMP profil, trvalý log událostí, zobrazení
   odhadu výdrže.
4. **Nasazení na flotilu** — jednotky Kuklenska, Piletice, Divišova, Čeperka 1, Lipky
   (kde chybí ethernet, doplněna WiFi brána MikroTik mAP lite s dst-nat).

## Odhad pracnosti

- Etapa 1 (PoC): ~30–40 čh
- Etapa 2 (firmware): ~80 čh (včetně ladění na živých kusech, ~20 vydaných verzí)
- Etapa 3 (integrace): ~15 čh
- Etapa 4 (rollout, 5 jednotek): ~20 čh + výjezdy na lokality

## Požadované zdroje

Na jednotku: ESP32 DevKit, invertor 74LVC14A, drobné pasivní součástky, kabeláž.
Na lokality bez ethernetu navíc MikroTik mAP lite. Dále přístup do sítě 10.107.0.0/16
(SNMP polling z power monitoru) a montážní přístup ke zdrojům na lokalitách.

## Rozpočet

- Práce ownera: ~150 člověkohodin × 500 Kč/h = **75 000 Kč** (vývoj, ladění,
  integrace, rollout; nad rámec toho výjezdy na lokality)
- ESP32 DevKit + součástky: ~250 Kč / jednotka
- MikroTik mAP lite (jen lokality bez ethernetu): ~700 Kč / kus
- Provozní náklady: zanedbatelné (odběr ESP jednotky v úsporném režimu, sdílený
  polling power monitoru)

## Owner projektu

Pavel Vlček (Pája) — vývoj firmwaru, HW zástavba, integrace do power monitoru.

## Technologický stack

- ESP32 (Arduino core, C++), pasivní vzorkování GPIO, dekódování TM1640
- HTTP server s basic-auth (admin / guest read-only), OTA přes web
- SNMP v1, enterprise MIB 1.3.6.1.4.1.53864.1.1 (idx 1–50)
- MikroTik RouterOS (mAP lite jako AP + NAT brána, export konfigurace v repu)
- Integrace: power monitor (Node.js, power.hkfree.org)

## Dopad na infrastrukturu

Minimální: jednotky jsou pasivní posluchači displeje, do zdrojů se nezasahuje.
Síťový provoz = SNMP polling z power monitoru (~1× za 30 s) a občasný přístup na web.
Žádná nová serverová služba — sběr dat využívá existující power monitor.

## Bezpečnostní dopady

- Web chráněn basic-auth (admin pro zápis, guest jen čtení); SNMP je read-only.
- Reálná hesla a WiFi klíče **nejsou v repozitáři** (placeholdery, sanitizace před pushem).
- mAP brány mají firewall: správa pouze z 10.107.0.0/16, WAN input drop,
  vypnuté nepoužívané služby.
- Jednotky žijí v interní síti 10.107.0.0/16, nejsou vystavené do internetu.

## GitHub

https://github.com/Pajaaaa/Mhpower-UPS (veřejné). Dle směrnice čl. 8.3 plánován
přesun/mirror do GitHub organizace HKFree.

## Dokumentace

V repozitáři: README (princip, zapojení, build, konfigurace, SNMP tabulka OID),
`docs/HISTORIE.md` (changelog verzí), fotogalerie provedení zástavby,
`network/` (export konfigurace mAP). Dle směrnice čl. 8.1 doplnit stránku
na wiki spolku (wiki.hkfree.org).

## Roadmapa

Hotovo: etapy 1–4, firmware v1.21 nasazen na flotile, integrace v power monitoru.

Otevřené:
- oživení jednotky Kuklenska (ESP bez napájení, nutný výjezd),
- HW robustnost napájení: reset supervisor na EN pin (řeší zaseknutí po měkkém
  náběhu 5V větve zdroje),
- dekódování bitů přepětí/podpětí z ikon displeje (`/api/iconscan` čeká na reálnou
  událost mimo pásmo 207–253 V),
- wiki stránka projektu + přesun repa do organizace HKFree.
