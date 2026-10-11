# Pegasos-II-Gigabit-Treiber

Wenn dir dieses Projekt nuetzt, freue ich mich ueber eine freiwillige Spende:
[PayPal](https://paypal.me/cfoltin). Eine Spende ist selbstverstaendlich keine
Voraussetzung fuer die Nutzung oder Weitergabe der Software.

Dieses Projekt enthaelt ein AmigaOS-4.1-SANA-II-Device fuer Ethernet-Port 1
des Marvell MV64360/MV64361 im Pegasos II. Die Hardware-Schicht verwaltet
Sende- und Empfangsringe und kommuniziert im Polling-Betrieb mit Controller
und PHY. Der Unit-Prozess fragt den Port in kurzen Abstaenden ab
(standardmaessig alle 500 Mikrosekunden).

## Voraussetzungen

- Pegasos II mit AmigaOS 4.1 und Marvell MV64360/MV64361
- PowerPC-AmigaOS-Cross-Compiler `ppc-amigaos-gcc` und AmigaOS-SDK/NDK
- GNU Make

Die Hardware-Schicht greift direkt auf die Register des Controllers zu und
benoetigt DMA-faehigen Speicher. Bei der Initialisierung blendet das Device
den Registerbereich mit den erforderlichen Speicherattributen ein. Hardware-
und Netzwerk-Tests sind fuer ein geeignetes Pegasos-II-System gedacht, nicht
fuer einen normalen PC.

## Bauen

Fuehre diese Befehle im Projektverzeichnis aus:

| Befehl | Ergebnis |
| --- | --- |
| `make dev` | Erstellt `mv643xx_log.device` im Projektverzeichnis. Diese Variante schreibt Diagnosemeldungen nach `RAM:mv643xx.log`. |
| `make build` | Erstellt `build/mv643xx.device` ohne Dateilogger. Das Verzeichnis `build/` wird bei Bedarf angelegt. |
| `make s2test` | Erstellt den SANA-II-Testclient `s2test`. |
| `make hwtest` | Erstellt das Hardware-Diagnoseprogramm `pegasos2-gbit-info`. |
| `make all` | Erstellt `hwtest`, `dev` und `s2test`. |
| `make clean` | Entfernt Objektdateien und erzeugte Programme/Devices, einschliesslich `mv643xx_log.device` und `build/mv643xx.device`. |

`make dev` und `make build` erzeugen unterschiedlich benannte Device-Dateien
mit unterschiedlichem Logging. Vor der Installation muss die ausgewaehlte
Datei `mv643xx.device` heissen, da dies der interne Device-Name ist. Die
Logging-Variante muss beim Kopieren entsprechend umbenannt werden.

## Device verwenden

Das Device heisst `mv643xx.device` und stellt Unit 0 (Ethernet-Port 1) bereit.
Installiere es an einem Ort, an dem AmigaOS oder der verwendete TCP/IP-Stack
SANA-II-Devices findet. Waehle in der Netzwerkkonfiguration
`mv643xx.device` mit Unit 0 aus. Die genaue Einrichtung haengt von der
verwendeten AmigaOS-Netzwerksoftware ab. Eine Installationsanleitung fuer
AmigaOS 4.1 steht in [Install.md](./Install.md).

Ein typischer Ablauf fuer einen Client:

1. `OpenDevice("mv643xx.device", 0, ...)` mit einem vollstaendigen
   `IOSana2Req` und den benoetigten Buffer-Management-Callbacks aufrufen.
2. Das Interface mit `S2_CONFIGINTERFACE` konfigurieren.
3. Die Hardware mit `S2_ONLINE` starten.
4. Empfangsanfragen mit `CMD_READ` einreichen und Frames mit `CMD_WRITE`,
   `S2_BROADCAST` oder `S2_MULTICAST` senden.
5. Das Device mit `S2_OFFLINE` stoppen und schliessen.

Das Device registriert den implementierten SANA-II-Befehlssatz. Nicht alle
SANA-II-Funktionen sind vollstaendig umgesetzt: `S2_GETTYPESTATS` liefert
derzeit `S2ERR_NOT_SUPPORTED`; `S2_READORPHAN` und Statistikabfragen sind
ebenfalls eingeschraenkt.

## Tests und Diagnose

### SANA-II-Testclient

`s2test` fuehrt grundlegende Open-/Close-, Abfrage-, Konfigurations-,
Online-/Offline- und Request-Abbruchtests aus. Aufruf:

```text
s2test [Device-Name oder Pfad]
```

Ohne Argument wird `mv643xx.device` verwendet. Die Buffer-Callbacks dieses
Testprogramms sind Platzhalter: Sie melden Erfolg, kopieren aber keine
Nutzdaten. Daher prueft der Client vor allem Device-Kommandos und
Request-Verarbeitung; er ist kein verlaesslicher Test fuer tatsaechlichen
Netzwerkverkehr.

### Hardware-Diagnoseprogramm

`pegasos2-gbit-info` ist ein separates Diagnoseprogramm fuer Register,
Speicherabbildung, PHY und Hardwaretests. Es fuehrt immer einen Register- und
PHY-Diagnoselauf aus und schreibt standardmaessig nach
`RAM:pegasos2-gbit.log`. Optionen:

| Option | Funktion und Wirkung |
| --- | --- |
| `-log <Pfad>` | Schreibt das Log statt in `RAM:pegasos2-gbit.log` in die angegebene Datei. |
| `-rxstoptest` | Stoppt RX-Queue 0 an Port 1, protokolliert Queue-Register und Deskriptorzeiger und versucht anschliessend, die Queue wieder zu starten. Nur ausfuehren, wenn das Device nicht erfolgreich geoeffnet wurde; die Option veraendert kurzzeitig den Hardwarezustand. |
| `-txprep` | Bereitet einen DMA-Puffer und einen Beispiel-TX-Deskriptor vor und protokolliert sie. Laut Programmcode wird dabei kein Ethernet-Port gestartet und kein Frame gesendet. |
| `-txtest JA` | Fuehrt einen TX-Test aus, der ein ARP-Paket sendet. Kann den Ethernet-Port starten und veraendert voruebergehend dessen Register. |
| `-rxtest JA` | Fuehrt einen RX-Test aus, der bis zu vier Frames empfaengt. Greift aktiv auf die RX-Hardware zu. |
| `-echotest JA` | Fuehrt einen 20-sekuendigen Echo-/ARP-Test fuer `192.168.1.80` aus und kann ARP-Antworten senden. |
| `-hwtest JA` | Startet den Hardware-Schicht-Selbsttest. Dieser laeuft bis zu 20 Sekunden, prueft den Link und beantwortet ARP-Anfragen fuer `192.168.1.80`. |
| `-dsoff` | Experiment: aendert PHY-Register 20 (Bit 11) und startet PHY-Reset/Autonegotiation. Aendert den PHY-Zustand. |
| `-g1000` | Experiment: konfiguriert die PHY-Ankuendigung auf 1000BASE-T Vollduplex und startet Autonegotiation neu. Aendert den PHY-Zustand. |
| `-restore` | Schreibt fest codierte Werte in PHY-Register 4, 9, 20 und 0 und startet Autonegotiation neu. Das stellt nicht zwingend individuelle vorherige PHY-Einstellungen wieder her. |
| `-ip <Adresse>` | Wird derzeit zwar eingelesen, aber nicht verwendet. Die Echo-Tests nutzen fest `192.168.1.80`. |

Beispiele:

```text
pegasos2-gbit-info
pegasos2-gbit-info -log RAM:port1-test.log
pegasos2-gbit-info -txtest JA
pegasos2-gbit-info -log RAM:rx-test.log -rxtest JA
```

TX-, RX-, Echo- und Hardwaretests greifen aktiv auf den Ethernet-Port zu.
`-dsoff`, `-g1000` und `-restore` veraendern PHY-Einstellungen. Fuehre solche
Optionen nur einzeln, bewusst und auf geeigneter Hardware aus. Kombiniere
insbesondere PHY-Experimente nicht unbedacht; `-restore` setzt fest codierte
Werte und ist kein allgemeines Zuruecksetzen auf den individuell vorherigen
Zustand.

## Projektstruktur

```text
.
├── device/
│   ├── init.c, dev_unit.c, dev_io.c, dev_cmd.c
│   ├── dev_log.c, dev_log_stub.c
│   ├── dev_hold.h, dev_wait.h
│   └── mv643xx.h, mv643xx.device_rev.h
├── src/
│   ├── mv64361.c, mv64361.h, mv64361_test.c
│   ├── pegasos2-gbit-info.c
│   └── s2test.c
├── build/                 # Ausgabe von make build
├── Makefile
├── LICENSE
├── Install.md
├── Readme_german.md
└── Readme_english.md
```

`obj/` enthaelt die beim Bauen erzeugten Objektdateien. Die Device-Dateien und
Testprogramme werden je nach Make-Ziel im Projektverzeichnis oder unter
`build/` abgelegt.

`make dev` erzeugt `mv643xx_log.device`, die Variante mit Dateilogger. Sie
schreibt Diagnosemeldungen nach `RAM:mv643xx.log`. Vor der Installation muss
die Datei in `mv643xx.device` umbenannt werden, da dies der interne Device-Name
ist.

## Bekannte Einschraenkungen

- Der Controller wird per Polling statt ueber Interrupts bedient.
- Das Neustarten einer angehaltenen RX-Queue, wiederholte Start-/Stopp-Zyklen
  und Linkwechsel waehrend des Betriebs sind noch nicht ausreichend getestet.
- Nach einem AmigaOS-Neustart ohne Computer-Reset versucht das Device eine
  stehengebliebene Port-Konfiguration nur dann wiederherzustellen, wenn die
  SDMA- und Serial-Control-Register der bekannten Startkonfiguration dieses
  Treibers entsprechen. Unbekannte aktive Konfigurationen werden nicht
  uebernommen. Die Wiederherstellung schien nach einem Modus-1-Neustart auf
  Pegasos-II-Hardware zu funktionieren; weitere Konfigurationen muessen noch
  verifiziert werden.
- `S2_GETTYPESTATS` wird nicht unterstuetzt; weitere SANA-II-Statistik- und
  Orphan-Read-Funktionen sind unvollstaendig.
- Derzeit wird nur `configure=dhcp` unterstuetzt. Bei anderen Konfigurationen
  werden die benoetigten Routen nicht zuverlaessig eingetragen.
- Der Build ohne Logging (`make build`) verwirft Diagnosemeldungen. Der
  normale `make dev`-Build schreibt sie nach `RAM:mv643xx.log`.

## Lizenz

Dieses Projekt wird unter der **GNU General Public License, Version 2 oder
einer spaeteren Version (GPL-2.0-or-later)** veroeffentlicht. Der vollstaendige
Text der GPL Version 2 steht in [LICENSE](./LICENSE) und auf der
[GNU-Website](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).

Bei der Weitergabe des Devices oder anderer aus diesem Projekt erzeugter
Programmdateien muessen die Lizenzbedingungen eingehalten werden.
Insbesondere muessen Lizenz- und Copyright-Hinweise erhalten bleiben und der
zugehoerige Quellcode unter der GPL bereitgestellt werden.

## Markenhinweis

Amiga, das Boing-Ball-Logo, das stilisierte A-Logo und das Haekchen-Logo
werden von der [Amiga Corporation](https://amiga.com/) als deren Marken
bezeichnet. AmigaOS und weitere hier genannte Marken gehoeren ihren jeweiligen
Rechteinhabern. Dieser unabhaengige Treiber ist nicht mit der Amiga Corporation
oder Hyperion Entertainment BV verbunden und wird von ihnen weder als
offizielles Produkt ausgegeben noch unterstuetzt. AmigaOS ist nicht Bestandteil
dieses Treibers und muss separat bezogen werden.

Ich bitte Hyperion Entertainment BV, vor einer Aufnahme dieses Treibers in ein
von Hyperion vertriebenes AmigaOS-Paket meine vorherige schriftliche Zustimmung
einzuholen. Diese Bitte schraenkt die Weitergaberechte anderer Empfaenger nach
der GPL nicht ein.

Wenn dir dieses Projekt nuetzt, freue ich mich ueber eine freiwillige Spende:
[PayPal](https://paypal.me/cfoltin). Eine Spende ist selbstverstaendlich keine
Voraussetzung fuer die Nutzung oder Weitergabe der Software.
