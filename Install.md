# Installationsanleitung / Installation Guide

## Deutsch

### Installation unter AmigaOS 4.1

1. Waehle eine der beiden Device-Dateien:
   - `mv643xx.device` – Build ohne Dateilogger, erstellt mit `make build`
   - `mv643xx_log.device` – Build mit Dateilogger, erstellt mit `make dev`
2. Kopiere die ausgewaehlte Datei nach `SYS:Devs/Networks/`. Das Device muss
   dort `mv643xx.device` heissen. Wenn du `mv643xx_log.device` verwendest,
   benenne sie beim Kopieren in `mv643xx.device` um.
3. Kopiere `MV643XX` und `MV643XX.info` nach
   `SYS:Devs/NetInterfaces/`.

Die Konfigurationsdatei `MV643XX` muss folgenden Inhalt haben:

```text
# DEVS:NetInterfaces/MV643XX
device=mv643xx.device
unit=0
configure=dhcp
```

### Bekannte Einschraenkung

Unterstuetzt wird derzeit nur `configure=dhcp`. Andere Konfigurationen werden
nicht zuverlaessig unterstuetzt, weil die benoetigten Routen nicht korrekt
eingetragen werden.

### Support und Haftung

Support erfolgt im A1K-Forum:
https://www.a1k.org/forum/index.php?threads/98586/

Die Nutzung der Software erfolgt auf eigene Gefahr. Soweit gesetzlich
zulaessig, wird keine Haftung fuer Schaeden an Hardware oder Software
uebernommen.

### Markenhinweis

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

## English

### Installation on AmigaOS 4.1

1. Choose one of the two device files:
   - `mv643xx.device` – build without a file logger, created with `make build`
   - `mv643xx_log.device` – build with a file logger, created with `make dev`
2. Copy the selected file to `SYS:Devs/Networks/`. The installed device must
   be named `mv643xx.device`. If you use `mv643xx_log.device`, rename it to
   `mv643xx.device` while copying it.
3. Copy `MV643XX` and `MV643XX.info` to `SYS:Devs/NetInterfaces/`.

The `MV643XX` configuration file must contain:

```text
# DEVS:NetInterfaces/MV643XX
device=mv643xx.device
unit=0
configure=dhcp
```

### Known limitation

Currently, only `configure=dhcp` is supported. Other configurations are not
reliably supported because the required routes are not added correctly.

### Support and disclaimer

Support is provided in the A1K forum:
https://www.a1k.org/forum/index.php?threads/98586/

Use this software at your own risk. To the extent permitted by law, no
liability is accepted for damage to hardware or software.

### Trademarks

Amiga, the Boing Ball logo, the stylized A logo, and the checkmark logo are
identified as trademarks of [Amiga Corporation](https://amiga.com/). AmigaOS
and other marks mentioned here belong to their respective rights holders.
This independent third-party driver is not affiliated with, endorsed by, or
presented as an official product of Amiga Corporation or Hyperion Entertainment
BV. AmigaOS is not included with this driver and must be obtained separately.

I ask Hyperion Entertainment BV to obtain my prior written consent before
including this driver in an AmigaOS package distributed by Hyperion. This
request does not restrict the redistribution rights of other recipients under
the GPL.
