# Установка awg-proxy на MikroTik

> Раздел документации [awg-proxy](../README.md): контейнер RouterOS, который подключает MikroTik к серверам AmneziaWG. Проще всего ставить [конфигуратором](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html): он сам генерирует все команды. Остальные разделы: [установка](install.md) · [обновление](update.md) · [переменные](env.md) · [AWG 3.x и IPv6](protocol.md) · [маршрутизация и MTU](routing.md) · [серверный режим](server-mode.md) · [неполадки](troubleshooting.md) · [FAQ](faq.md).

## Получение параметров AWG

1. Откройте приложение **AmneziaVPN**
2. Выберите нужное подключение
3. Нажмите **Поделиться** (Share)
4. Выберите: **Протокол**: AmneziaWG, **Формат**: AmneziaWG Format
5. Сохраните `.conf`-файл

Параметры обфускации (`Jc`, `Jmin`, `Jmax`, `S1`, `S2`, `H1`--`H4`) находятся в секции `[Interface]`, а `Endpoint` и `PublicKey` -- в секции `[Peer]`.

**Если каких-то параметров нет** -- например в AWG-конфигах для Cloudflare WARP, где есть только `Jc`/`Jmin`/`Jmax` (иногда ещё `I1`), -- конфигуратор берёт значения AmneziaWG по умолчанию, ровно как приложения AmneziaWG: `S1 = S2 = 0`, `H1`--`H4` = `1`--`4` (обычные типы сообщений WireGuard), отсутствующие `Jc`/`Jmin`/`Jmax` = `0`. Подставленные поля помечены «по умолчанию», под ними пояснение. Для WARP это и есть рабочая схема: Cloudflare говорит на обычном WireGuard, а через DPI рукопожатие проводят junk- и CPS-пакеты, которые прокси отправляет перед ним. Если в конфиге нет вообще никакой обфускации и это не WARP, конфигуратор предупредит, что это обычный WireGuard, и предложит кнопкой добавить junk-пакеты (`Jc = 4`, `Jmin = 40`, `Jmax = 70`) -- обычный WireGuard-сервер их просто отбрасывает.

### Нет AmneziaVPN: конфиг Cloudflare WARP

В «Режиме установки» есть пункт **«Нет AmneziaVPN — Cloudflare WARP»**. Он показывает, где взять конфиг: откройте [генератор WARP-конфигов](https://lanrat.github.io/wireguard-warp-generator/) (или консольную утилиту [wgcf](https://github.com/ViRb3/wgcf)), нажмите «Generate WARP Config», скопируйте конфиг и вставьте его в поле. Ставится то же, что в режиме «Контейнер». Сам конфигуратор никуда не обращается.

WARP-конфиг, вставленный в режиме «Контейнер», сам включает режим WARP — конфигуратор узнаёт его по ключу сервера Cloudflare или по `engage.cloudflareclient.com`. В режиме WARP форма короткая:

- **лишние настройки спрятаны** -- для WARP подходят значения по умолчанию; кнопка «Показать все настройки» вернёт их;
- **DNS для контейнера спрашивается, только если он нужен**: endpoint -- имя, а публичного DNS в конфиге нет (обычно там `1.1.1.1`, и он подставляется сам);
- **галочка «Случайный порт WARP» включена**: Cloudflare принимает WireGuard на портах 2408, 500, 1701 и 4500, и все они уходят в `AWG_REMOTE` (порт из конфига первым). Прокси начинает со случайного порта, держится за тот, что ответил (и помнит его после перезапуска), а другой пробует, только если этот замолчал на ~15 с. Строка `# AllowedPorts = ...` в конфиге важнее галочки;
- **кнопка «Сделать пакет перед рукопожатием похожим на QUIC Chrome»** записывает в конфиг `I1` с заголовком обычного QUIC v1 Initial, каким его шлёт Chrome к Cloudflare (датаграмма 1250 байт, случайный 8-байтный DCID, пустой SCID), и `Jc = 0`, чтобы перед ним не шли мелкие junk-пакеты. Cloudflare этот пакет игнорирует. Шифрованная часть -- случайные байты: у прокси нет ключей, чтобы собрать настоящий TLS ClientHello, поэтому анализатор, который расшифровывает QUIC Initial, увидит, что внутри не ClientHello. Обычный QUIC ходит на порт 443, а у WARP WireGuard на 2408/500/1701/4500 -- по порту пакет на QUIC не похож, об этом конфигуратор пишет прямо.

## Ручная установка

### 1. Включение контейнеров и fetch

Установите пакет container с [mikrotik.com](https://mikrotik.com/download), загрузите на роутер и перезагрузитесь. Затем:

```routeros
/system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
```

`fetch=yes` нужен для скачивания образа командой `/tool/fetch` прямо на роутере. Если планируете загружать файл вручную через Winbox/SCP, `fetch=yes` не обязателен. `scheduler=yes` нужен для автообновления RU-списка (сценарий «не-РФ трафик в туннель»), `bandwidth-test=yes` — для замера скорости через `/tool/bandwidth-test`.

Роутер попросит подтверждение (кнопка или перезагрузка, зависит от модели).

### 2. Загрузка образа

Скачайте `awg-proxy-{arch}.tar.gz` со страницы [Releases](https://github.com/timbrs/amneziawg-mikrotik-c/releases) и загрузите на роутер через Winbox или SCP. Для RouterOS 7.20 и ниже используйте файлы с суффиксом `-7.20-Docker` (Docker-формат).

> **hEX refresh (E50UG), hEX S 2025 (E60iUGS) и hAP ax S (E62iUGS):** несмотря на `architecture-name: arm`, их процессор исполняет только arm32v5-образы ([ограничение RouterOS](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container)) — используйте `awg-proxy-armv5.tar.gz`, иначе контейнер упадёт с `exited with signal 4 (Illegal instruction)`. Конфигуратор определяет эти устройства автоматически.

Или скачайте прямо на роутер (замените URL на актуальный):

```routeros
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz" dst-path=awg-proxy-arm64.tar.gz
```

**RouterOS 7.22+: из реестра, без скачивания файла.** Образ публикуется в GHCR, архитектура
определяется автоматически, а установленный так контейнер потом обновляется одной командой
(см. [Обновление](update.md#обновление)):

```routeros
/container/add remote-image=ghcr.io/timbrs/awg-proxy:latest ...
```

Правило `/container/config` трогать не нужно: имя реестра берётся прямо из строки. Для
hEX refresh (E50UG), hEX S 2025 (E60iUGS) и hAP ax S (E62iUGS) тег отдельный — `ghcr.io/timbrs/awg-proxy:latest-armv5`:
эти CPU исполняют только arm32v5, и в общий multi-arch индекс такой образ не входит.

> На роутерах с 16 МБ flash задайте `tmpdir` на том же диске, где лежит контейнер, — слои
> скачиваются именно туда: `/container/config set tmpdir=usb1/pull`.

### 3. Настройка сети

```routeros
/interface/veth/add name=veth-awg-proxy address=172.18.0.2/30 gateway=172.18.0.1
/ip/address/add address=172.18.0.1/30 interface=veth-awg-proxy
/ip/firewall/nat/add chain=srcnat action=masquerade src-address=172.18.0.0/30
```

### 4. WireGuard

```routeros
/interface/wireguard/add name=wg-awg-proxy private-key="YOUR_PRIVATE_KEY" listen-port=12429
/interface/wireguard/peers/add interface=wg-awg-proxy public-key="SERVER_PUBLIC_KEY" \
    preshared-key="YOUR_PRESHARED_KEY" endpoint-address=172.18.0.2 endpoint-port=51820 \
    allowed-address=0.0.0.0/0 persistent-keepalive=25
/ip/address/add address=YOUR_TUNNEL_IP interface=wg-awg-proxy
/ip/firewall/nat/add chain=srcnat action=masquerade out-interface=wg-awg-proxy
```

Замените:
- `YOUR_PRIVATE_KEY` -- PrivateKey из `[Interface]`
- `SERVER_PUBLIC_KEY` -- PublicKey из `[Peer]`
- `YOUR_PRESHARED_KEY` -- PresharedKey из `[Peer]` (если есть)
- `YOUR_TUNNEL_IP` -- Address из `[Interface]` (например, `10.8.0.2/32`)

### 5. Переменные окружения

```routeros
/container/envs/add list=awg-proxy-env key=AWG_LISTEN value=":51820"
/container/envs/add list=awg-proxy-env key=AWG_REMOTE value="SERVER_IP:PORT"
/container/envs/add list=awg-proxy-env key=AWG_JC value="5"
/container/envs/add list=awg-proxy-env key=AWG_JMIN value="30"
/container/envs/add list=awg-proxy-env key=AWG_JMAX value="500"
/container/envs/add list=awg-proxy-env key=AWG_S1 value="20"
/container/envs/add list=awg-proxy-env key=AWG_S2 value="20"
/container/envs/add list=awg-proxy-env key=AWG_H1 value="1234567890"
/container/envs/add list=awg-proxy-env key=AWG_H2 value="1234567891"
/container/envs/add list=awg-proxy-env key=AWG_H3 value="1234567892"
/container/envs/add list=awg-proxy-env key=AWG_H4 value="1234567893"
/container/envs/add list=awg-proxy-env key=AWG_SERVER_PUB value="SERVER_PUBLIC_KEY"
/container/envs/add list=awg-proxy-env key=AWG_CLIENT_PUB value=[/interface/wireguard/get [find name=wg-awg-proxy] public-key]
```

Замените все значения на параметры из вашего `.conf`-файла. `AWG_CLIENT_PUB` берется автоматически из WireGuard-интерфейса.

### 6. Создание и запуск контейнера

```routeros
/container/add file=awg-proxy-arm64.tar.gz interface=veth-awg-proxy envlist=awg-proxy-env \
    hostname=awg-proxy root-dir=disk1/awg-proxy logging=yes shm-size=4M start-on-boot=yes
/container/start [find where tag~"awg-proxy"]
```

Проверьте работу:

```routeros
/container/print
/interface/wireguard/peers/print
```

Контейнер должен быть в статусе `running`, а у пира должно появиться значение `last-handshake`.

## Установка без доступа к GitHub

Если роутер не может достучаться до GitHub, включите **офлайн-режим** в конфигураторе. В выводе появится **шаг 0** — скрипт,
который ничего не меняет и только печатает, что именно скачать и куда положить:

```
1. Container image
   file: awg-proxy-arm64.tar.gz
   url:  https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz
   put it here: usb1/awg-proxy-arm64.tar.gz

2. RU address list
   file: ru_ranges_timeout.rsc
   url:  https://github.com/timbrs/ru-ranges/releases/latest/download/ru_ranges_timeout.rsc
   put it here: usb1/ru_ranges_timeout.rsc
```

Имя архива нельзя назвать заранее: оно зависит от архитектуры, версии RouterOS
(до 7.20 нужна сборка `-7.20-Docker`) и — на arm — от модели платы (hEX, E50UG,
E60iUGS и E62iUGS уезжают на `armv5`). Поэтому шаг 0 спрашивает об этом сам роутер.

Порядок:

1. Выполнить шаг 0 на роутере, записать имена файлов.
2. Скачать их там, где GitHub доступен.
3. Загрузить на роутер (Files или перетаскиванием в WinBox) по указанным путям.
4. Выполнить шаг 1 — установку.

В офлайн-режиме установка не обращается к сети вообще: ни `remote-image` из
реестра, ни `/tool/fetch` релизного архива. Если файла на месте не оказалось,
скрипт называет недостающее имя и останавливается, а не падает внутри `fetch`.
Обновление RU-списка тоже перестаёт ждать `github.com` по минуте на каждой
загрузке и при каждом суточном прогоне и принимает любой из двух файлов —
`ru_ranges_timeout.rsc` или `ru_ranges.rsc`.

Файл, положенный вручную, скрипт не удаляет — он ваш. Удаляется только то, что
скрипт скачал сам.

Суточный планировщик обновления в офлайн-режиме не создаётся: он ходит за
списком релизов на `api.github.com`. Скрипт `<prefix>-update` при этом никуда
не девается — запустите его руками, когда у роутера появится выход в интернет.
