> This project is not affiliated with or endorsed by MikroTik / SIA Mikrotikls

# awg-proxy -- AmneziaWG для MikroTik

[![C11](https://img.shields.io/badge/C-11-blue)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[English version](README_en.md) | [GitHub](https://github.com/timbrs/amneziawg-mikrotik-c)

Легковесный Docker-контейнер, который позволяет MikroTik подключаться к серверам AmneziaWG. Весь трафик шифруется нативным WireGuard-клиентом роутера, а контейнер преобразует формат пакетов.
Полностью поддерживает протокол AWG 3.0 (в том числе AWG 1.0, 1.5, 2.0).

[Конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html)

## Содержание

- [Как это работает](#как-это-работает)
- [Быстрый старт (конфигуратор)](#быстрый-старт-конфигуратор)
- [Установка без доступа к GitHub](#установка-без-доступа-к-github)
- [Требования](#требования)
- [Ручная установка](#ручная-установка)
- [Получение параметров AWG](#получение-параметров-awg)
- [Серверный режим (1:N) — подробная настройка](#серверный-режим-1n--подробная-настройка)
  - [Маршрутизация между клиентами](#маршрутизация-между-клиентами)
- [Дополнительные настройки](#дополнительные-настройки)
- [Обновление](#обновление)
- [Удаление](#удаление)
- [Устранение неполадок](#устранение-неполадок)
  - [execvpe /awg-proxy: No such file or directory](#execvpe-awg-proxy-no-such-file-or-directory)
  - [Site-to-site: handshake did not complete](#site-to-site-handshake-did-not-complete)
  - [Storage device not found](#storage-device-not-found)
  - [Insufficient disk space](#insufficient-disk-space)
  - [not allowed by device-mode](#not-allowed-by-device-mode)
  - [child spawn failed / could not load next layer](#child-spawn-failed--could-not-load-next-layer)
  - [exited with signal 4 (Illegal instruction)](#exited-with-signal-4-illegal-instruction)
  - [Список RU-адресов не загружается после перезагрузки](#список-ru-адресов-не-загружается-после-перезагрузки)
  - [Handshake не проходит после восстановления из бэкапа](#handshake-не-проходит-после-восстановления-из-бэкапа)
- [Сборка из исходников](#сборка-из-исходников)
- [Лицензия](#лицензия)

## Как это работает

### Стандартный режим (normal, по умолчанию)

```
MikroTik WG-клиент ──UDP──> [awg-proxy] ──UDP──> сервер AmneziaWG
   (шифрование)          (преобразование)          (обфускация)
```

Прокси заменяет заголовки пакетов, добавляет паддинг и мусорные пакеты так, чтобы сервер AmneziaWG принял трафик. Ключи и данные не затрагиваются.

### Режим reverse (mikrotik-to-mikrotik)

```
MikroTik1 WG ↔ [proxy1 normal] ──AWG──> [proxy2 reverse] ↔ MikroTik2 WG
```

Принимает AWG-трафик от normal-прокси, преобразует обратно в стандартный WireGuard и пересылает локальному WG-серверу. Позволяет соединить два MikroTik через AWG без поднятия отдельного AWG-сервера.
Это соединение вида точка-точка, не поддерживается мультисоединения.

### Режим awg-server (1:N)

```
proxy1a (normal) ──AWG──┐
proxy1b (normal) ──AWG──┤──> [reverse-hub] ──WG──> WG-сервер
proxy1c (normal) ──AWG──┘
```

Множественный обратный прокси: принимает AWG-подключения от нескольких normal-прокси и маршрутизирует ответы от WG-сервера к правильному клиенту через встроенную таблицу сессий. Для каждого пира используется ~16 байт в hash-таблице.

Подробная настройка описана в разделе [Серверный режим (1:N) — подробная настройка](#серверный-режим-1n--подробная-настройка).

Совместим с AWG v1 и v2 -- версия определяется автоматически по переменным окружения.

## Быстрый старт (конфигуратор)

0. Подготовьте роутер:
   - Установите пакет **container** с [mikrotik.com](https://mikrotik.com/download) (System → Packages), загрузите на роутер и перезагрузите
   - Включите device-mode:
     ```routeros
     /system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
     ```
     Роутер попросит подтверждение (кнопка Reset/Mode или перезагрузка)
1. Экспортируйте `.conf`-файл из AmneziaVPN (см. [Получение параметров AWG](#получение-параметров-awg))
2. Откройте [конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html)
3. Вставьте содержимое `.conf`-файла
4. Скопируйте сгенерированные команды и выполните их в терминале MikroTik

Готово. Конфигуратор работает оффлайн, данные не отправляются на сервер.

<video src="https://github.com/user-attachments/assets/f0100789-0a23-42f8-a67f-085e5f8d13a3" controls width="100%"></video>

![Замеры скорости на MikroTik AX3](https://github.com/user-attachments/assets/9fb34444-681b-4f34-8306-8f202f1b121d)

*Замеры скорости на устройстве MikroTik AX3*

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
(до 7.20 нужна сборка `-7.20-Docker`) и — на arm — от модели платы (hEX, E50UG и
E60iUGS уезжают на `armv5`). Поэтому шаг 0 спрашивает об этом сам роутер.

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

## Требования

- Сервер AmneziaWG с известными параметрами обфускации
- Файл конфигурации `.conf`, экспортированный из AmneziaVPN
- MikroTik RouterOS 7.4+ с пакетом **container**
  - **RouterOS 7.21+**: стандартные образы `awg-proxy-{arch}.tar.gz` (OCI-формат)
  - **RouterOS 7.20 и ниже**: образы `awg-proxy-{arch}-7.20-Docker.tar.gz` (Docker-формат)
  - Конфигуратор определяет версию автоматически
- Архитектура: ARM64, ARM (v7), ARM (v5: hEX refresh / hEX S 2025) или x86_64 ([проверить устройство](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container))
- Минимум 256 КБ свободного места на диске (или USB-накопитель)
- Минимум 16 МБ свободной оперативной памяти (RAM)

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

> **hEX refresh (E50UG) и hEX S 2025 (E60iUGS):** несмотря на `architecture-name: arm`, CPU EN7562CT исполняет только arm32v5-образы ([ограничение RouterOS](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container)) — используйте `awg-proxy-armv5.tar.gz`, иначе контейнер упадёт с `exited with signal 4 (Illegal instruction)`. Конфигуратор определяет эти устройства автоматически.

Или скачайте прямо на роутер (замените URL на актуальный):

```routeros
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz" dst-path=awg-proxy-arm64.tar.gz
```

**RouterOS 7.22+: из реестра, без скачивания файла.** Образ публикуется в GHCR, архитектура
определяется автоматически, а установленный так контейнер потом обновляется одной командой
(см. [Обновление](#обновление)):

```routeros
/container/add remote-image=ghcr.io/timbrs/awg-proxy:latest ...
```

Правило `/container/config` трогать не нужно: имя реестра берётся прямо из строки. Для
hEX refresh (E50UG) и hEX S 2025 (E60iUGS) тег отдельный — `ghcr.io/timbrs/awg-proxy:latest-armv5`:
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

## Получение параметров AWG

1. Откройте приложение **AmneziaVPN**
2. Выберите нужное подключение
3. Нажмите **Поделиться** (Share)
4. Выберите: **Протокол**: AmneziaWG, **Формат**: AmneziaWG Format
5. Сохраните `.conf`-файл

Параметры обфускации (`Jc`, `Jmin`, `Jmax`, `S1`, `S2`, `H1`--`H4`) находятся в секции `[Interface]`, а `Endpoint` и `PublicKey` -- в секции `[Peer]`.

## Серверный режим (1:N) — подробная настройка

Серверный режим позволяет одному awg-proxy обслуживать множество MikroTik-клиентов одновременно. Это аналог полноценного AmneziaWG-сервера, но реализованный через связку WireGuard-сервер + awg-proxy.

### Архитектура

```
MikroTik1 + awg-proxy(normal) ──AWG──┐
MikroTik2 + awg-proxy(normal) ──AWG──┤──> VPS: [awg-proxy server :443] ──WG──> [WG-сервер :51820]
MikroTik3 + awg-proxy(normal) ──AWG──┘
```

- **WireGuard-сервер** — обычный WG-сервер, принимает стандартный WG-трафик от awg-proxy. Каждый MikroTik-клиент — отдельный peer.
- **awg-proxy в режиме `server`** — слушает публичный порт (например, 443/udp), принимает AWG-трафик от клиентских normal-прокси, преобразует в стандартный WG и пересылает локальному WG-серверу. Таблица сессий маршрутизирует ответы к правильному клиенту.

### 1. Настройка серверной стороны (VPS)

#### WireGuard-сервер

Установите WireGuard на VPS и создайте конфигурацию. Пример `/etc/wireguard/wg0.conf`:

```ini
[Interface]
PrivateKey = <приватный_ключ_сервера>
Address = 10.0.0.1/24
ListenPort = 51820

# Клиент 1 (MikroTik1)
[Peer]
PublicKey = <публичный_ключ_клиента_1>
AllowedIPs = 10.0.0.2/32

# Клиент 2 (MikroTik2)
[Peer]
PublicKey = <публичный_ключ_клиента_2>
AllowedIPs = 10.0.0.3/32
```

```bash
wg-quick up wg0
```

#### awg-proxy в режиме server

**Вариант A: Docker Compose** (рекомендуется)

Создайте `docker-compose.yml`:

```yaml
services:
  awg-proxy:
    image: ghcr.io/timbrs/awg-proxy:latest
    container_name: awg-proxy-server
    restart: unless-stopped
    network_mode: host
    environment:
      AWG_MODE: server
      AWG_LISTEN: ":443"              # публичный порт для AWG-клиентов
      AWG_REMOTE: "127.0.0.1:51820"   # локальный WireGuard-сервер
      AWG_JC: "4"
      AWG_JMIN: "50"
      AWG_JMAX: "1000"
      AWG_S1: "84"
      AWG_S2: "40"
      AWG_H1: "1263070671"
      AWG_H2: "1883150219"
      AWG_H3: "1505218884"
      AWG_H4: "1343091225"
      AWG_SERVER_PUB: "<публичный_ключ_WG_сервера>"
      AWG_CLIENT_PUB: "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
      AWG_CLIENT_PUBS: "<публичный_ключ_клиента_1>,<публичный_ключ_клиента_2>"
      # или вместо строки выше:
      # AWG_CLIENT_PUBS_FILE: "/etc/awg-proxy/client-pubs.txt"
      AWG_LOG_LEVEL: info
```

> Замените параметры AWG_JC, AWG_S1, AWG_H1--H4 и т.д. на свои. Эти параметры должны совпадать на сервере и на всех клиентах.

> В server mode `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE` — это новый явный список **реальных публичных ключей клиентов**, который нужен для прямых AmneziaWG 2.0 клиентов. `AWG_CLIENT_PUB` остаётся как legacy single-peer / proxy-only fallback. Для старого сценария `proxy → server → WG` placeholder по-прежнему работает, потому что normal-клиентский прокси пересчитает входящий MAC1 ещё раз. Для прямого клиента этого пересчёта нет, поэтому одного placeholder уже недостаточно.

> Если скачать образ из ghcr.io не получается, соберите его локально: склонируйте репозиторий, выполните `docker build -t awg-proxy .` и укажите в compose `image: awg-proxy`.

```bash
docker compose up -d
```

**Вариант B: бинарник + systemd**

```bash
# Скачайте бинарник для вашей платформы
wget https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-linux-amd64
chmod +x awg-proxy-linux-amd64

# Создайте systemd-сервис
cat > /etc/systemd/system/awg-proxy.service << 'EOF'
[Unit]
Description=AWG Proxy Server
After=network.target wg-quick@wg0.service

[Service]
Type=simple
Environment=AWG_MODE=server
Environment=AWG_LISTEN=:443
Environment=AWG_REMOTE=127.0.0.1:51820
Environment=AWG_JC=4
Environment=AWG_JMIN=50
Environment=AWG_JMAX=1000
Environment=AWG_S1=84
Environment=AWG_S2=40
Environment=AWG_H1=1263070671
Environment=AWG_H2=1883150219
Environment=AWG_H3=1505218884
Environment=AWG_H4=1343091225
Environment=AWG_SERVER_PUB=<публичный_ключ_WG_сервера>
Environment=AWG_CLIENT_PUB=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
Environment=AWG_CLIENT_PUBS=<публичный_ключ_клиента_1>,<публичный_ключ_клиента_2>
# Или вместо AWG_CLIENT_PUBS:
# Environment=AWG_CLIENT_PUBS_FILE=/etc/awg-proxy/client-pubs.txt
Environment=AWG_LOG_LEVEL=info
ExecStart=/usr/local/bin/awg-proxy-linux-amd64
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

cp awg-proxy-linux-amd64 /usr/local/bin/
systemctl daemon-reload
systemctl enable --now awg-proxy
```

### 2. Настройка клиентской стороны (MikroTik)

Каждый MikroTik-клиент использует стандартный awg-proxy в режиме `normal` (по умолчанию).

1. Создайте файл `.conf` для каждого клиента. В секции `[Peer]`:
   - `Endpoint` = IP_вашего_VPS:443 (порт AWG_LISTEN сервера)
   - `PublicKey` = публичный ключ WG-сервера

2. Откройте [конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), вставьте `.conf` и выполните команды на MikroTik.

3. Параметры обфускации (Jc, S1, S2, H1--H4) должны **точно совпадать** с параметрами на сервере.

### 3. Добавление нового клиента

1. Сгенерируйте ключевую пару WireGuard:
   ```bash
   wg genkey | tee client_private.key | wg pubkey > client_public.key
   ```

2. Добавьте peer на WG-сервере:
   ```bash
   wg set wg0 peer $(cat client_public.key) allowed-ips 10.0.0.X/32
   ```

3. Создайте `.conf`-файл для нового клиента (на основе шаблона, с уникальным PrivateKey и Address).

4. Настройте MikroTik через конфигуратор.

5. Если этот клиент должен подключаться **напрямую как AmneziaWG 2.0**, добавьте его реальный публичный ключ в `AWG_CLIENT_PUBS` (или в файл `AWG_CLIENT_PUBS_FILE`) на сервере. Для proxy-only клиентов старое placeholder-поведение через `AWG_CLIENT_PUB` остаётся рабочим.

> **Важно:** перезапускать awg-proxy на сервере при добавлении нового клиента **обычно не нужно** — таблица сессий обновляется автоматически. Но если вы меняете сам список `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE`, перезапустите awg-proxy, чтобы он перечитал peer list.

### 4. Проверка работы

На VPS:
```bash
# Логи awg-proxy
docker logs awg-proxy-server
# или
journalctl -u awg-proxy -f

# Проверка WireGuard peers
wg show wg0
```

На MikroTik:
```routeros
# Проверка handshake
/interface/wireguard/peers/print where interface~"awg-proxy"
# Должен быть recent handshake
```

### Маршрутизация между клиентами

В топологии "звезда" каждый клиент по умолчанию видит только сеть сервера. Клиент 1 (192.168.8.0/24) **не знает** о сети клиента 2 (192.168.11.0/24) и наоборот. Трафик между клиентами идёт через сервер, но маршруты нужно прописать вручную.

#### Схема

```
Клиент 1 (192.168.8.0/24)                    Клиент 2 (192.168.11.0/24)
   WG: 10.10.10.2                               WG: 10.10.10.3
         \                                            /
          \_________ Сервер (192.168.1.0/24) ________/
                      WG: 10.10.10.1
```

По умолчанию настроены маршруты:
- Клиент 1 → сеть сервера (192.168.1.0/24)
- Клиент 2 → сеть сервера (192.168.1.0/24)
- Сервер → сеть клиента 1 (192.168.8.0/24)
- Сервер → сеть клиента 2 (192.168.11.0/24)

Маршрутов клиент 1 ↔ клиент 2 **нет**.

#### Как добавить

**Шаг 1. На сервере** — добавить подсети обоих клиентов в `allowed-address` каждого WG-пира:

```routeros
# Посмотреть текущих пиров
/interface/wireguard/peers/print where interface=wg-awg-server-1

# Добавить подсеть клиента 2 в allowed-address пира клиента 1
/interface/wireguard/peers/set [find where comment=awg-server-1-client-1] \
    allowed-address=10.10.10.2/32,192.168.8.0/24,192.168.11.0/24

# Добавить подсеть клиента 1 в allowed-address пира клиента 2
/interface/wireguard/peers/set [find where comment=awg-server-1-client-2] \
    allowed-address=10.10.10.3/32,192.168.11.0/24,192.168.8.0/24
```

**Шаг 2. На клиенте 1** — добавить маршрут к сети клиента 2:

```routeros
/ip/route/add dst-address=192.168.11.0/24 gateway=wg-awg-server-1
```

**Шаг 3. На клиенте 2** — добавить маршрут к сети клиента 1:

```routeros
/ip/route/add dst-address=192.168.8.0/24 gateway=wg-awg-server-1
```

#### Проверка

С устройства в сети клиента 1 пингуйте устройство в сети клиента 2:

```
ping 192.168.11.X
```

Трафик пойдёт: клиент 1 → WG → сервер → WG → клиент 2. Это двойной hop через сервер — задержка будет суммой двух туннелей.

#### Если клиентов много

Для N клиентов нужно:
- На сервере: в `allowed-address` каждого пира добавить подсети **всех остальных** клиентов
- На каждом клиенте: добавить маршруты ко **всем остальным** клиентским подсетям

При 3+ клиентах это проще автоматизировать скриптом на сервере.

## Дополнительные настройки

### Все переменные окружения

| Переменная | Обязательная | По умолчанию | Описание |
|------------|:---:|:---:|-------------|
| `AWG_LISTEN` | Да | -- | Адрес прослушивания |
| `AWG_REMOTE` | Да | -- | Адрес AWG-сервера; порт может быть списком или диапазоном |
| `AWG_JC` | Да | -- | Количество мусорных пакетов |
| `AWG_JMIN` | Да | -- | Мин. размер мусорного пакета |
| `AWG_JMAX` | Да | -- | Макс. размер мусорного пакета |
| `AWG_S1` | Да | -- | Паддинг handshake init |
| `AWG_S2` | Да | -- | Паддинг handshake response |
| `AWG_H1`--`AWG_H4` | Да | -- | Типы сообщений |
| `AWG_SERVER_PUB` | Да | -- | Публичный ключ сервера |
| `AWG_CLIENT_PUB` | Да* | -- | Публичный ключ клиента; в `server` режиме — legacy single-peer / proxy-only fallback |
| `AWG_CLIENT_PUBS` | Нет | -- | В `server` режиме: список реальных публичных ключей direct-клиентов |
| `AWG_CLIENT_PUBS_FILE` | Нет | -- | В `server` режиме: путь к файлу со списком реальных публичных ключей direct-клиентов |
| `AWG_S3` | Нет | `0` | Паддинг cookie reply (v2) |
| `AWG_S4` | Нет | `0` | Паддинг transport data (v2) |
| `AWG_I1`--`AWG_I5` | Нет | -- | CPS-шаблоны (v1.5/v2/v3) |
| `AWG_HEADER_PROTECTION_KEY` | Нет | -- | Ключ шифрования заголовка (v3), base64 (44 симв.) или hex (64 симв.). Задан — включается AmneziaWG 3.0 |
| `AWG_RANDOM_TRAILERS` | Нет | `off` | v3.1: случайный хвост у рукопожатий (`on`/`off`). Должен совпадать с сервером |
| `AWG_DISABLE_COOKIES` | Нет | `off` | v3.1: не выпускать наружу cookie reply (`on`/`off`) |
| `AWG_MODE` | Нет | `normal` | Режим работы: `normal`, `reverse`, `server` |
| `AWG_FB_H1`--`AWG_FB_H4` | Нет | -- | Ступень 1 цепочки отката: типы сообщений. Включает цепочку |
| `AWG_FB_S1`, `AWG_FB_S2` | Да** | -- | Паддинг init/response для ступени 1 |
| `AWG_FB_S3`, `AWG_FB_S4` | Нет | `0` | Паддинг cookie reply / transport для ступени 1 |
| `AWG_FB_I1`--`AWG_FB_I5` | Нет | -- | CPS-шаблоны ступени 1 |
| `AWG_FB_HP` | Нет | `0` | `1` = ступень 1 тоже шифрует заголовок (нужен `AWG_HEADER_PROTECTION_KEY`) |
| `AWG_FB_RANDOM_TRAILERS` | Нет | `off` | `on` = ступень 1 тоже добавляет случайный хвост (v3.1) |
| `AWG_FB2_*`, `AWG_FB3_*` | Нет | -- | Ступени 2 и 3 цепочки, тот же набор ключей |
| `AWG_FB_AFTER` | Нет | `20` | Секунд тишины от сервера до пробы следующей ступени (инициатор) |
| `AWG_SRC_PORT` | Нет | random | Исходящий порт к серверу: `random` / `auto` / число |
| `AWG_TIMEOUT` | Нет | `60` | Молчание сервера при неотвеченном рукопожатии, после которого переподключаемся (сек) |
| `AWG_DNS_REFRESH` | Нет | `60` | Период фоновой проверки DNS для hostname в AWG_REMOTE (сек, `0` = выкл) |
| `AWG_HE_DELAY` | Нет | `250` | Happy Eyeballs: мс форы у IPv4 до пробы IPv6 (только если у имени есть и A, и AAAA) |
| `AWG_STATE_FILE` | Нет | `/etc/awg-proxy.state` | Файл выученного предпочтения семьи адресов; пустое значение — не сохранять |
| `AWG_LOG_LEVEL` | Нет | `info` | Уровень логирования |
| `AWG_NO_GRO` | Нет | `0` | Отключить UDP GRO |
| `AWG_NO_GSO` | Нет | `0` | Отключить UDP GSO на отправке |
| `AWG_NO_DF` | Нет | `0` | Снять DF-бит с UDP-пакетов (обход DPI, режущих DF=1) |
| `AWG_SOCKET_BUF` | Нет | `16777216` | Размер буфера сокета |
| `AWG_CPU_C2S` | Нет | `-1` | CPU для потока client→server |
| `AWG_CPU_S2C` | Нет | `-1` | CPU для потока server→client |
| `AWG_RT` | Нет | `0` | Приоритет `SCHED_RR` для потоков ввода-вывода (1..50, `0` = выкл). Требует привилегий |
| `AWG_RPS` | Нет | — | Маска ядер для RPS на интерфейсе контейнера (hex, например `9`). Требует привилегий |
| `AWG_BUSY_POLL` | Нет | `0` | SO_BUSY_POLL таймаут (мкс). На RouterOS не работает — ядро собрано без `CONFIG_NET_RX_BUSY_POLL` |
| `AWG_SPIN` | Нет | `0` | Спин в userspace вместо busy poll: мкс неблокирующих перечитываний сокета перед сном. `auto` — самонастройка |
| `AWG_STATS` | Нет | `0` | Период строки статистики (пропускная способность, наши дропы, дропы ядра, по-сокетные), сек |
| `AWG_DNS` | Нет | -- | DNS-сервер для резолва hostname в AWG_REMOTE |
| `AWG_REJECT_AFTER` | Нет | -- | v3: принимается, не эмулируется. При нижней границе < 150 с прокси печатает WARN |
| `AWG_CONTENT_PADDING` | Нет | -- | v3: принимается и игнорируется (см. ниже) |
| `AWG_REKEY_AFTER`, `AWG_REKEY_TIMEOUT`, `AWG_KEEPALIVE_TIMEOUT`, `AWG_MAX_HANDSHAKE_ATTEMPTS` | Нет | -- | v3: принимаются и игнорируются (см. ниже) |

`*` В `server` режиме должен быть задан либо legacy `AWG_CLIENT_PUB`, либо явный direct-peer список `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE`.

`**` Обязательны только если включён резервный профиль (задан `AWG_FB_H1`).

Версия протокола определяется автоматически: **v3.1** если включён `AWG_RANDOM_TRAILERS` или `AWG_DISABLE_COOKIES`, иначе **v3** если задан `AWG_HEADER_PROTECTION_KEY`, иначе **v2** если заданы S3/S4 или H в виде диапазонов, иначе **v1.5** если заданы CPS-шаблоны (I1-I5), иначе **v1**.

### AmneziaWG 3.0

На проводе 3.0 отличается от 2.0 ровно одним: заголовок пакета шифруется ChaCha20 (RFC 8439) на ключе `AWG_HEADER_PROTECTION_KEY`, а nonce — первые 12 байт S-паддинга. У transport-пакета шифруются только 16 байт заголовка (`Type` + `Receiver` + `Counter`), у handshake init/response/cookie reply — сообщение целиком. Сам паддинг не шифруется. Всё остальное в 3.0 (junk, `I1`-`I5`, `S1`-`S4`, диапазоны `H1`-`H4`) есть у нас с версии 2.

Отсюда два практических правила:

- **`AWG_S1`--`AWG_S4` должны быть >= 12**, иначе nonce не помещается в паддинг. Прокси откажется стартовать; `amneziawg-go` проверяет то же самое (в его README написано 8 — это опечатка, в коде `HeaderCipherNonceSize = 12`).
- **`AWG_S4` держите в диапазоне 12--16.** Внешняя датаграмма = IP-заголовок + 8 (UDP) + S4 + 16 + `round_up(MTU,16)` + 16. По IPv4 заголовок 20 байт, то есть `1484 + S4` при штатном MTU 1420: при S4 = 16 это ровно 1500, при S4 = 20 начнётся фрагментация. **По IPv6 заголовок 40 байт**, те же цифры дают `1504 + S4` -- предел пробит уже при S4 = 0, поэтому там MTU нужно понижать всегда. Предельный MTU:

  ```
  IPv4:  floor((1440 - S4) / 16) * 16      # S4=0 -> 1440,  S4=16 -> 1424
  IPv6:  floor((1420 - S4) / 16) * 16      # S4=0 -> 1408,  S4=16 -> 1392
  ```

  Это та же причина, по которой `wg-quick` ставит 1420 для IPv4 и 1400 для IPv6. Конфигуратор генерирует S4 из 12--16 и при IPv4 MTU не трогает; при IPv6-эндпоинте или чужом `.conf` с большим S4 он понижает `mtu=` у WireGuard-интерфейса. Сам прокси MTU не меняет -- он только печатает WARN с предельным значением, когда соединение фактически установилось по IPv6.

Ключ не задан — прокси работает как v2 **байт в байт** (`hp_off_matches_v2` в юнит-тестах, отдельная ветка ChaCha20 не вызывается ни разу). Так же устроен и сам `amneziawg-go`: нулевой ключ означает `cipher == nil`.

**Что из 3.0 прокси не воспроизводит и почему.** Прокси видит только UDP-датаграммы и не владеет сессионными ключами — Noise идёт между WG-стеком MikroTik и сервером. Отсюда:

- `ContentPaddingAddition` кладётся в plaintext **до** AEAD; дописать байты снаружи нельзя — сломается тег. Параметр объявлен опциональным, и без него AWG 3.0 откатывается на штатное выравнивание до 16 — ровно то, что делает WG-стек MikroTik. То есть наш профиль на проводе = «AWG 3.0 с выключенным ContentPaddingAddition», это законная конфигурация, а не деградация.
- Тайминги (`RekeyAfterTime`, `RekeyTimeout`, `KeepaliveTimeout`, `MaxHandshakeAttempts`) — прокси пересылает пакеты сразу, а не выдерживает паузы. Все эти параметры проверяет только отправитель; сервер их не валидирует, поэтому туннель работает.
- `RejectAfterTime` — политика приёмной стороны, её нельзя эмулировать в принципе. Если на сервере нижняя граница меньше ~150 с, он начнёт отвергать пакеты от ключа, который MikroTik ещё считает живым (ре-кей стартует на 120 с, старый keypair живёт до 180 с). Прокси печатает WARN при старте.

Все перечисленные переменные принимаются, чтобы `.conf` от провайдера переносился без потерь, и пишутся в лог как неприменённые. Что остаётся заметным для DPI: размеры пакетов кратны 16 и ритм WireGuard (ретраи 5 с, keepalive 10 с, ре-кей 120 с) не маскируется.

**Цепочка отката (обратная совместимость / устойчивость к DPI).** Основной профиль (`AWG_S*`, `AWG_H*`, `AWG_I*`, `AWG_HEADER_PROTECTION_KEY`) дополняется до четырёх ступеней: `AWG_FB_*`, `AWG_FB2_*`, `AWG_FB3_*`. Типовая цепочка — v3 → v2 → v1.5 → v1. Инициатор (`normal`) работает на основном профиле и, если сервер молчит дольше `AWG_FB_AFTER` секунд, по кругу пробует следующие ступени, пока не найдёт рабочую. Отвечающая сторона (`reverse`) принимает любую ступень и отвечает той, которой пришёл handshake. В режиме `server` профиль запоминается **на каждого клиента** отдельно (кэш по адресу источника + поле в таблице сессий), поэтому клиенты на разных версиях не перебивают друг друга. Ненулевой `AWG_S4` теперь разрешён: headroom буфера считается как максимум S4 по всем ступеням.

### AmneziaWG 3.1

3.1 добавляет к 3.0 две вещи, обе поддержаны прокси.

**`RandomTrailers` (`AWG_RANDOM_TRAILERS=on`)** — к каждому рукопожатию (init, response, cookie reply) дописывается случайный хвост, поэтому их размеры перестают быть отпечатком: раньше init всегда весил ровно `S1 + 148` байт, и этого одного числа хватало, чтобы узнать AmneziaWG в потоке UDP. Длина хвоста берётся из диапазона `[0, окно − размер пакета)`, где окно — самая большая датаграмма, увиденная на этом соединении (не меньше 500 байт и не больше 1500). Так дополненное рукопожатие попадает в тот же диапазон размеров, что и обычный трафик туннеля. На приёме прокси принимает рукопожатие любой длины начиная с ожидаемой и отбрасывает хвост.

Transport-пакеты прокси не трогает: там хвост кладётся **внутрь** шифрования (как обычный content padding) и снимается WireGuard'ом на той стороне — сессионных ключей у прокси нет, дописать байты снаружи нельзя, сломается AEAD-тег. Это не потеря: у transport-пакетов размер и так переменный, отпечатком были именно рукопожатия.

Параметр обязан совпадать с сервером: собеседник на 3.0 меряет размеры точно и дополненное рукопожатие просто отбросит. В цепочке отката хвост задаётся на каждой ступени отдельно (`AWG_FB_RANDOM_TRAILERS`), поэтому ступень, описывающая старый сервер, остаётся байт в байт прежней.

**`DisableCookies` (`AWG_DISABLE_COOKIES=on`)** — cookie reply (тип 3) наружу не уходит вообще. Cookie — это ответ на рукопожатие под нагрузкой, и по нему сервер легко зондировать: послал мусорный init — получил пакет известного размера. С включённым параметром прокси такой ответ отбрасывает. Осмысленно в режимах `reverse` и `server`, где cookie выпускает наш WireGuard; в обычном клиентском режиме их и так почти не бывает.

Оба параметра переносятся из `.conf` конфигуратором как есть (`RandomTrailers = on` → `AWG_RANDOM_TRAILERS=on`); принимаются и `on`/`off`, и `1`/`0`.

### IPv6

IPv6 работает на обоих плечах. **Исходящее** (`AWG_REMOTE`, контейнер → AWG-сервер) описано ниже. **Входящее** (`AWG_LISTEN`) нужно в режимах `server` и `reverse`, где сокет смотрит в интернет: хаб на роутере без публичного IPv4 доступен клиентам только по IPv6. Указание IPv6-адреса в `AWG_LISTEN` переключает семью сокета:

```
AWG_LISTEN=:51820             # IPv4, как раньше — обычный клиентский туннель на veth
AWG_LISTEN=[::]:443           # обе семьи: IPv4-клиент приходит v4-mapped
AWG_LISTEN=[2001:db8::1]:443  # только этот адрес
```

Семья выбирается один раз, при старте, и дальше адрес клиента везде — и в таблице сессий, и в кэше профилей, и в батчах recv/send — хранится в общем виде фиксированной длины, так что горячий путь не ветвится по семье. У `[::]` выключается `IPV6_V6ONLY`, поэтому один хаб обслуживает клиентов обеих семей одним сокетом.

На MikroTik контейнер сам по себе IPv6 не получает: veth нужно выдать адрес (обычно ULA) и `gateway6`, а входящий трафик завернуть на него через `dst-nat` в `/ipv6 firewall nat`. Конфигуратор делает это сам, когда отмечена галочка «сервер доступен по IPv6».

**Когда используется.** Прокси резолвит `AWG_REMOTE` на `AF_UNSPEC` и берёт первую A- и первую AAAA-запись. Дальше:

- **только AAAA** -- работает по IPv6, вопросов нет;
- **только A** -- как раньше, ничего не меняется;
- **обе** -- запускается проба Happy Eyeballs (RFC 8305), см. ниже;
- **IPv6-литерал** в `AWG_REMOTE` (`[2001:db8::1]:443`) -- сразу IPv6, никакого резолва и никакой пробы.

**Как выбирается семья.** У UDP нет рукопожатия, поэтому единственный признак живости -- первый пакет, пришедший от сервера. При наличии обеих записей прокси поднимает два `connect()`-нутых сокета и:

1. первый исходящий пакет уходит по IPv4 (та же семья, что и во всех прошлых версиях -- IPv6 забирает соединение только когда докажет, что работает);
2. если за `AWG_HE_DELAY` (250 мс) ответа нет, **тот же самый** пакет дублируется по IPv6. Дубль безопасен: обе копии несут один TAI64N, и сервер, получивший обе, отвергает вторую как реплей и отвечает ровно один раз;
3. побеждает та семья, чей пакет пришёл первым; проигравший сокет закрывается, и дальше поведение ровно такое же, как у одностековой конфигурации -- накладных расходов в установившемся режиме нет.

Проба ждёт на `poll()` по двум дескрипторам и не вычитывает данные, поэтому выигравшая датаграмма достаётся обычному пути приёма. `POLLERR` (ICMP unreachable) на одном сокете сразу отдаёт соединение другой семье. Каждый реконнект начинает пробу заново -- `dial_remote()` при этом заново резолвит имя, так что это же и есть повторный резолв при ошибках.

**Выученное предпочтение (`AWG_STATE_FILE`).** Если IPv4 у площадки мёртв (типично для CGNAT), плата за фору в 250 мс повторяется при каждом реконнекте. Поэтому исход пробы запоминается: в файл пишется один байт -- `6` или `4`, и следующий запуск сразу набирает выученную семью первой. Запись происходит **не чаще одного раза за запуск процесса** и только когда байт на диске устарел: флеш роутера маленький, и мигающая связь не должна превращаться в цикл записи.

Важно, что означают значения. `6` -- это только оптимизация: пропустить заведомо дохлую фору IPv4. `4` -- это **не** «IPv6 выключен», а возврат к штатному порядку, в котором IPv6-сокет по-прежнему поднимается и пробуется при каждом коннекте и реконнекте. Поэтому временная поломка IPv6 не запоминается навсегда: как только IPv4 снова замолчит, IPv6 получит новый шанс и сможет отыграть предпочтение обратно. Если у имени вообще нет AAAA (или в `AWG_REMOTE` стоит IPv4-литерал), IPv6 не используется вовсе и никакая метка этого не меняет.

Файл лежит в корне контейнера и переживает его перезапуск. `AWG_STATE_FILE=""` полностью отключает сохранение -- поведение возвращается к чистому RFC 8305 без памяти.

**MTU.** Заголовок IPv6 на 20 байт длиннее, и полноразмерный пакет перестаёт помещаться в 1500 -- формулы и предельные значения выше, в разделе про AmneziaWG 3.0. Прокси MTU не меняет (он принадлежит WireGuard-интерфейсу роутера), а печатает при подключении:

```
WARN: remote is IPv6: set the WireGuard interface MTU to 1408 or lower — ...
```

Конфигуратор ставит `mtu=` сам: галочка «Сервер доступен по IPv6» включается автоматически, как только в endpoint распознан IPv6-литерал. Для DNS-имени галочка по умолчанию снята (браузер намеренно не резолвит имя вашего сервера -- оно ушло бы третьей стороне), но сгенерированный скрипт дополнительно спрашивает сам роутер:

```
:do {
  :local a6 [:resolve vpn.example.com type=ipv6]
  :if ([:len $a6] > 0) do={ /interface/wireguard/set [find name=wg-awg-proxy-1] mtu=1392 }
} on-error={}
```

Параметр `type=` у `:resolve` есть не во всех сборках RouterOS 7.x; `on-error={}` гарантирует, что при его отсутствии установка не падает, а MTU просто остаётся IPv4-шным. В этом случае поставьте галочку вручную или задайте `mtu=` сами.

**Чего нет.** Остальная генерация RouterOS (`/ip/address`, `/ip/route`, `/ip/firewall`, veth, policy-routing) остаётся IPv4. Для сценария «весь не-РФ трафик» это не проблема: защита от петли маршрутизации нужна только IPv4-трафику, а при IPv6-сервере она просто не создаётся -- policy-routing до него всё равно не дотягивается.

**`AWG_NO_DF` и IPv6.** У IPv6 нет DF-бита -- промежуточный маршрутизатор там не фрагментирует в принципе. На IPv6-сокете опция (`IPV6_MTU_DISCOVER=IPV6_PMTUDISC_DONT`) влияет только на поведение локального стека: он перестаёт учитывать PMTU-ответы. Симметрия сохранена, чтобы `AWG_NO_DF=1` означало одно и то же на обеих семьях.

### Подробное описание переменных

#### Обязательные -- параметры обфускации

Все значения берутся из `.conf`-файла AmneziaVPN (секция `[Interface]` и `[Peer]`). Должны **точно** совпадать с параметрами сервера, иначе handshake не пройдёт.

**`AWG_LISTEN`** -- адрес и порт, на котором прокси принимает UDP-пакеты: от WireGuard-клиента роутера в режиме `normal`, от AWG-клиентов из интернета в режимах `reverse` и `server`. Формат: `адрес:порт` или `:порт` (слушать на всех интерфейсах). IPv6-литерал -- в квадратных скобках, как и в `AWG_REMOTE`.

```
AWG_LISTEN=:51820             # все интерфейсы, порт 51820 (стандартный)
AWG_LISTEN=172.18.0.2:9000    # конкретный адрес и порт
AWG_LISTEN=[::]:443           # обе семьи сразу (хаб за IPv6)
AWG_LISTEN=[2001:db8::1]:443  # только этот IPv6-адрес
```

**`AWG_REMOTE`** -- адрес и порт AWG-сервера (`Endpoint` из `[Peer]`). Поддерживаются IPv4, IPv6 и доменные имена. IPv6-литерал **обязан** быть в квадратных скобках: порт отделяется двоеточием, а в самом адресе двоеточий сколько угодно, так что без скобок форма неоднозначна и отвергается.

```
AWG_REMOTE=1.2.3.4:443            # IPv4 + порт
AWG_REMOTE=[2001:db8::1]:443      # IPv6 + порт (скобки обязательны)
AWG_REMOTE=vpn.example.com:51820  # домен + порт
```

Вместо одного порта можно перечислить несколько -- через запятую, диапазоны через дефис (до 32 элементов):

```
AWG_REMOTE=1.2.3.4:443,8080                    # два порта
AWG_REMOTE=1.2.3.4:20150-20299                 # диапазон
AWG_REMOTE=1.2.3.4:20150-20299,21500-21649     # несколько диапазонов
AWG_REMOTE=[2001:db8::1]:6000-6100             # то же для IPv6
```

Порт берётся из списка **случайно и заново при каждом подключении** -- и при старте, и при любом переподключении (таймаут, смена IP по DNS, переключение ступени `AWG_FB_*`). Смысл двойной. Во-первых, постоянный порт -- приметная деталь для DPI, особенно когда все клиенты сервера сидят на одном и том же; случайный порт из широкого диапазона такой зацепки не даёт. Во-вторых, заблокированный порт перестаёт быть приговором: если сервер молчит, прокси через **15 секунд** уходит на следующий порт, не дожидаясь полного `AWG_TIMEOUT` (по умолчанию 60 секунд). В логе это видно строкой `no answer on this port, trying another one`, а выбранный порт -- строкой `connected to <адрес> port <порт>`.

Работает это только если **сервер действительно принимает весь перечисленный диапазон** -- то есть на нём стоит DNAT/REDIRECT всех этих UDP-портов на порт AmneziaWG. Список портов, которых сервер не слушает, обернётся молчанием и бесконечными хопами. Диапазоны в `AWG_REMOTE` подставляет веб-конфигуратор, если в `.conf` есть строка `# AllowedPorts = ...` (её пишет бот, выдавший ключ).


Приём по IPv6 настраивается отдельно, через `AWG_LISTEN` (см. выше): в `normal` он не нужен -- это локальный veth внутри роутера, -- а в `server`/`reverse` именно он делает хаб доступным клиентам.

**`AWG_JC`**, **`AWG_JMIN`**, **`AWG_JMAX`** -- параметры мусорных (junk) пакетов. Перед каждым handshake init отправляется `JC` случайных UDP-пакетов размером от `JMIN` до `JMAX` байт. Сервер их отбрасывает, но для DPI они выглядят как обычный трафик. Значения из `.conf` (`Jc`, `Jmin`, `Jmax`).

```
AWG_JC=5      # 5 мусорных пакетов перед handshake
AWG_JMIN=30   # минимум 30 байт
AWG_JMAX=500  # максимум 500 байт

AWG_JC=0      # мусорные пакеты отключены
```

**`AWG_S1`**, **`AWG_S2`** -- количество байт паддинга, добавляемых к handshake init (S1) и handshake response (S2). Изменяет размер пакетов, чтобы DPI не мог определить WireGuard handshake по характерным размерам 148 и 92 байта. Значения из `.conf` (`S1`, `S2`).

```
AWG_S1=20   # +20 байт к handshake init (148 → 168)
AWG_S2=20   # +20 байт к handshake response (92 → 112)

AWG_S1=0    # паддинг отключен
AWG_S2=0
```

**`AWG_H1`**, **`AWG_H2`**, **`AWG_H3`**, **`AWG_H4`** -- подмена типов сообщений WireGuard. Стандартные типы (1, 2, 3, 4) заменяются на указанные значения, чтобы DPI не распознал протокол. В v1 -- фиксированные числа, в v2 -- могут быть диапазонами `min-max`. Значения из `.conf` (`H1`--`H4`).

```
# v1: фиксированные значения
AWG_H1=1234567890
AWG_H2=1234567891
AWG_H3=1234567892
AWG_H4=1234567893

# v2: диапазоны (случайное значение из диапазона для каждого пакета)
AWG_H1=100-200
AWG_H4=1000-2000
```

**`AWG_SERVER_PUB`**, **`AWG_CLIENT_PUB`** -- публичные ключи сервера и клиента в формате base64 (44 символа). Используются для пересчёта MAC1 в handshake-пакетах после подмены заголовков. Без корректных ключей MAC-проверка на сервере не пройдёт.

```
AWG_SERVER_PUB=kB3VpJIEGVTW2D4GR0cC/c3bOEG3jNIm5MjHJkSIj2I=
AWG_CLIENT_PUB=aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789+/ABCD=

# Автоматическое получение из WireGuard-интерфейса роутера:
AWG_CLIENT_PUB=[/interface/wireguard/get [find name=wg-awg-proxy] public-key]
```

- В режимах `normal` и `reverse` `AWG_CLIENT_PUB` — обычный обязательный ключ удалённого peer.
- В режиме `server` `AWG_CLIENT_PUB` — **legacy fallback**: single-peer совместимость и старый placeholder-сценарий для proxy-only клиентов.
- Для прямых AmneziaWG 2.0 клиентов в режиме `server` теперь нужно перечислить реальные клиентские ключи через **`AWG_CLIENT_PUBS`** или **`AWG_CLIENT_PUBS_FILE`**.

**`AWG_CLIENT_PUBS`** -- список реальных публичных ключей direct-клиентов для режима `server`. Разделители: запятая, пробел или перевод строки.

```
AWG_CLIENT_PUBS=base64key1,base64key2
AWG_CLIENT_PUBS="base64key1 base64key2"
```

**`AWG_CLIENT_PUBS_FILE`** -- путь к файлу со списком реальных публичных ключей direct-клиентов для режима `server`.

```
AWG_CLIENT_PUBS_FILE=/etc/awg-proxy/client-pubs.txt
```

Каждая строка файла — один base64 WireGuard public key. При исходящем WG handshake response серверный прокси сравнивает исходный стандартный MAC1 с ключами из этого списка, находит нужного peer и затем переписывает ответ уже с его peer-specific MAC1. Если совпадения нет, используется legacy fallback из `AWG_CLIENT_PUB`.

#### Необязательные -- протокол v2

**`AWG_S3`**, **`AWG_S4`** -- паддинг для cookie reply (S3) и transport data (S4). Появились в AWG v2. Если заданы S3 > 0 или S4 > 0, прокси автоматически переключается в режим v2.

```
AWG_S3=0    # по умолчанию, нет паддинга
AWG_S4=16   # +16 байт к каждому пакету transport data
```

**`AWG_I1`--`AWG_I5`** -- CPS-шаблоны (Constant Packet Size). До 5 шаблонов для генерации пакетов фиксированного формата перед handshake. Если заданы без S3/S4/диапазонов H, прокси работает в режиме v1.5.

Поддерживаемые теги (совпадают с `device/obf.go` эталона): `<b 0xHEX>` -- статические байты, `<r N>` -- N случайных байт, `<rc N>` -- N случайных букв (52 буквы, без цифр), `<rd N>` -- N случайных цифр, `<t>` -- unix-время, 4 байта big-endian, `<dz N>` -- N нулевых байт, `<d>` и `<ds>` -- принимаются и не дают байт (у эталона в I-пакетах пустой источник). Тег `<c>` (счётчик) -- наше расширение, эталон его не примет.

```
AWG_I1=b:48656c6c6f,r:10,t:4,c:4
```

#### Необязательные -- режим работы

**`AWG_MODE`** -- режим работы прокси. Определяет направление преобразования пакетов.

- `normal` (по умолчанию) -- стандартный прокси: принимает WireGuard от роутера, преобразует в AWG и отправляет на AWG-сервер.
- `reverse` -- обратный прокси (1:1 site-to-site): принимает AWG от другого normal-прокси, преобразует обратно в WireGuard и отправляет локальному WG-серверу. Используется в паре с normal-прокси на другой стороне.
- `server` -- обратный прокси-хаб (1:N): по сути аналог AmneziaWG Server. Поддерживает подключения от нескольких normal-прокси одновременно. Маршрутизация ответов от WG-сервера к правильному клиенту осуществляется через таблицу сессий по `sender_index`/`receiver_index` из WireGuard-пакетов.

```
AWG_MODE=normal    # по умолчанию
AWG_MODE=reverse   # обратный прокси, 1:1
AWG_MODE=server    # AmneziaWG-server, 1:N
```

В режимах `reverse` и `server` `AWG_REMOTE` указывает на WireGuard-сервер (а не на AWG-сервер), а `AWG_LISTEN` принимает AWG-трафик от normal-прокси. Параметры обфускации (H1--H4, S1--S4, JC и т.д.) должны совпадать с параметрами normal-прокси на другой стороне.

#### Необязательные -- сеть и диагностика

**`AWG_SRC_PORT`** -- исходящий UDP-порт для соединения с AWG-сервером. По умолчанию (`random`) порт выбирает ядро: без bind, новый эфемерный порт при каждом реконнекте. Значение `auto` заставляет прокси копировать порт клиента WireGuard, а число -- взять фиксированный порт.

**Почему `random` стал умолчанием (с v1.2.7).** Копирование порта клиента намертво фиксирует исходящую 5-tuple на всё время жизни контейнера, а запись NAT, построенную по ней, потом невозможно стряхнуть. Masquerade выбирает адрес источника один раз -- в момент создания записи. Если запись родилась, когда у WAN-интерфейса не было адреса (типично сразу после переполучения аренды DHCP), в неё попадёт адрес какого-нибудь другого интерфейса роутера, нередко приватный -- такие пакеты провайдер выбрасывает. Обычно conntrack сам бы её выбросил, но keepalive прокси обновляет запись быстрее, чем истекает 30-секундный UDP-таймаут. Реконнект тоже не спасает: та же 5-tuple попадает в ту же испорченную запись. Канал остаётся мёртвым при полностью живом сервере на другом конце, пока запись не удалят руками. Эфемерный порт снимает проблему целиком: новый порт -- новая запись -- NAT пересчитан.

В режимах `server` и `reverse` переменная ни на что не влияет при значениях `random` и `auto`: ветка, копирующая порт клиента, живёт только в обработчике обычного режима, поэтому bind там не делается в любом случае. Фиксированный номер порта работает во всех режимах.

```
AWG_SRC_PORT=random  # по умолчанию, эфемерный порт ядра, новый при каждом реконнекте
AWG_SRC_PORT=auto    # копирует порт WG-клиента (умолчание до v1.2.7)
AWG_SRC_PORT=0       # то же что auto
AWG_SRC_PORT=12345   # фиксированный порт 12345
```

**`AWG_TIMEOUT`** -- сколько секунд сервер может молчать в ответ на рукопожатие, прежде чем прокси переподключится (re-resolve DNS + новый сокет + повторная проверка обеих семей адресов).

Считается не бездействие, а именно молчание сервера при живом клиенте: установленный туннель без трафика законно тих, а keepalive ответа не требует, поэтому сами по себе они переподключение не вызывают. Сигналом служит неотвеченный handshake init -- на него WireGuard отвечает всегда, когда может, так что молчание в ответ означает, что пропал путь, а не трафик. Поэтому короткое значение безопасно и ложных переподключений не даёт.

```
AWG_TIMEOUT=60    # по умолчанию, 1 минута
AWG_TIMEOUT=30    # быстрее замечать обрыв на нестабильных каналах
AWG_TIMEOUT=300   # 5 минут, если переподключение дороже простоя
```

**`AWG_DNS_REFRESH`** -- период фоновой проверки DNS в секундах, когда `AWG_REMOTE` задан hostname'ом (для literal IP -- как IPv4, так и IPv6 -- проверка выключена). Прокси периодически заново резолвит hostname и, если текущий адрес сервера исчез из записей две проверки подряд (защита от round-robin DNS), переподключается на новый адрес, не дожидаясь `AWG_TIMEOUT`. Резолв идёт на `AF_UNSPEC`, а совпадением считается только запись той же семьи и с теми же байтами адреса -- иначе у dual-stack имени A-запись «прикрывала» бы исчезнувшую AAAA. Гранулярность -- 5 секунд. Реконнект сбрасывает сессию клиента (как и при таймауте) -- WireGuard сам выполнит новый handshake.

```
AWG_DNS_REFRESH=60   # по умолчанию, проверка раз в минуту
AWG_DNS_REFRESH=0    # выключить фоновую проверку DNS
```

**`AWG_HE_DELAY`** -- фора IPv4 в миллисекундах перед пробой IPv6 (Happy Eyeballs, RFC 8305). Читается только когда `AWG_REMOTE` -- имя с обеими записями, A и AAAA; в любой одностековой конфигурации переменная не используется вовсе. Подробности -- в разделе [IPv6](#ipv6) ниже.

```
AWG_HE_DELAY=250   # по умолчанию, как в RFC 8305
AWG_HE_DELAY=0     # дублировать первый пакет по IPv6 сразу
```

**`AWG_STATE_FILE`** -- где хранить выученное предпочтение семьи адресов (один байт, `6` или `4`). Запись -- максимум один раз за запуск процесса и только при смене значения, чтобы не изнашивать флеш роутера. Значение `4` не отключает IPv6, а лишь возвращает штатный порядок с пробой; подробности -- в разделе [IPv6](#ipv6).

```
AWG_STATE_FILE=/etc/awg-proxy.state   # по умолчанию
AWG_STATE_FILE=                       # не запоминать вовсе
```

**`AWG_LOG_LEVEL`** -- уровень логирования. Определяет подробность вывода в `/container/print` и syslog роутера.

- `none` -- ничего не выводить (для production на слабых устройствах)
- `error` -- только ошибки (bind/connect failed, reconnect)
- `info` -- стартовая конфигурация, подключения клиентов, реконнекты (по умолчанию)
- `debug` -- трассировка пакетов: handshake init, junk-отправка, GRO-сегменты, ошибки send. Нужен для диагностики проблем с handshake

```
AWG_LOG_LEVEL=info    # по умолчанию
AWG_LOG_LEVEL=debug   # полная трассировка для отладки
AWG_LOG_LEVEL=error   # только ошибки
AWG_LOG_LEVEL=none    # тишина
```

**`AWG_NO_GRO`** -- отключает UDP GRO (Generic Receive Offload). GRO объединяет несколько входящих UDP-пакетов в один буфер, уменьшая количество системных вызовов; прокси разбирает такой буфер обратно по размеру сегмента из cmsg `UDP_GRO`. Включается по умолчанию только в режиме `normal` и сразу на обоих сокетах (приём от клиента и приём от сервера); в режимах `reverse` и `server` не используется. Если ядро восемь раз подряд ничего не склеило, прокси сам снимает опцию сокета и переходит на `recvmmsg`. Установите `AWG_NO_GRO=1` для принудительного отключения.

```
AWG_NO_GRO=0   # по умолчанию, GRO включён (если ядро поддерживает)
AWG_NO_GRO=1   # принудительно отключить GRO, использовать recvmmsg
```

**`AWG_NO_GSO`** -- отключает UDP GSO (Generic Segmentation Offload) на отправке. Это зеркало GRO: прокси отдаёт ядру подряд идущие пакеты одинакового размера одним `sendmsg` с cmsg `UDP_SEGMENT`, а ядро само режет буфер на датаграммы. Один системный вызов вместо десятков; на реальном трафике серия выходит 15--22 пакета. Работает на обоих направлениях и во всех режимах, включая `server` (серия рвётся при смене адресата). Если ядро не умеет `UDP_SEGMENT`, прокси однократно пишет в лог `GSO disabled by kernel: ...` и дальше шлёт обычным `sendmmsg`. Отключать имеет смысл только для замеров «с офлоадом и без» на одной и той же сборке.

Насколько офлоад сработал, видно в строке `gso:` периодической статистики (`AWG_STATS`): `run` -- средняя длина серии, `1.0` означает, что склеивать было нечего.

```
AWG_NO_GSO=0   # по умолчанию, GSO включён (если ядро поддерживает)
AWG_NO_GSO=1   # принудительно отключить, каждый батч уходит sendmmsg
```

**`AWG_NO_DF`** -- снимает DF-бит (Don't Fragment) с исходящих UDP-пакетов прокси (`IP_MTU_DISCOVER=IP_PMTUDISC_DONT` на обоих сокетах). Linux по умолчанию шлёт UDP с DF=1 (Path MTU Discovery); есть сообщения, что некоторые DPI-узлы (ТСПУ) на отдельных сетях хуже пропускают UDP с DF=1. Опция меняет заголовок IP на проводе, поэтому выключена по умолчанию -- включайте только при проблемах со связью: с DF=0 крупные пакеты могут фрагментироваться по пути. На IPv6-сокете применяется `IPV6_MTU_DISCOVER=IPV6_PMTUDISC_DONT`, но DF-бита у IPv6 нет -- там это влияет только на реакцию локального стека на PMTU (см. раздел [IPv6](#ipv6)).

```
AWG_NO_DF=0   # по умолчанию, DF-бит как в системе (обычно DF=1)
AWG_NO_DF=1   # снять DF-бит с UDP-пакетов прокси
```

**`AWG_RT` и `AWG_RPS`** -- два рычага против потерь, вызванных не переполнением буфера, а планировщиком. Оба работают **только в привилегированном контейнере** (на RouterOS -- `/container set ... privileged=yes`, свойство появилось в 7.24): ядро проверяет права против init-namespace, поэтому обычному контейнеру `sched_setscheduler` отвечает `EPERM`, а `/sys/class/net` недоступен на запись.

`AWG_RT` ставит потокам ввода-вывода класс `SCHED_RR` с указанным приоритетом (1..50). Нужен, когда потери приходят пачкой в одно окно, а не размазаны: это подпись того, что поток-читатель не получал процессор десятки миллисекунд, и размер буфера тут уже ни при чём. Намеренно несовместим с `AWG_SPIN` -- спин под real-time отобрал бы ядро у softirq, который считает крипто для WG; если заданы оба, `AWG_RT` игнорируется с записью в лог.

`AWG_RPS` пишет маску ядер в `rps_cpus` приёмной очереди интерфейса контейнера -- ядро раскладывает обработку приёма по указанным процессорам вместо одного. Маска шестнадцатеричная, как её ждёт ядро: `9` = ядра 0 и 3, `e` = ядра 1--3.

Разводить их по разным ядрам обязательно. На hAP ax(2) при пинах потоков `AWG_CPU_C2S=1`, `AWG_CPU_S2C=2` маска `e` (ядра 1--3) накладывается на сами потоки: real-time вытесняет softirq ровно там, куда RPS его раскладывает, и выигрыш съедается. Маска `9` (ядра 0 и 3) разносит их и даёт лучший результат из всех проверенных сочетаний.

```
AWG_RT=0            # по умолчанию, обычный класс планировщика
AWG_RT=10           # SCHED_RR приоритет 10
AWG_RPS=9           # RPS на ядрах 0 и 3
AWG_CPU_C2S=1       # потоки на ядрах 1 и 2 -- не пересекаются с маской RPS
AWG_CPU_S2C=2
```

При старте прокси пишет, что получилось: `c2s: realtime SCHED_RR prio 10` либо `c2s: realtime refused (...)`, и `rps: <интерфейс> mask applied`. Если строк нет -- проверьте, что контейнер привилегированный и что уровень логирования пропускает INFO.

**`AWG_SOCKET_BUF`** -- размер буферов приёма/отправки (SO_RCVBUF/SO_SNDBUF) для UDP-сокетов в байтах. Ядро обычно удваивает запрошенное значение. Большие буферы снижают потерю пакетов при нагрузке, но потребляют RAM.

```
AWG_SOCKET_BUF=16777216  # по умолчанию, 16 МБ
AWG_SOCKET_BUF=4194304   # 4 МБ, для устройств с ограниченной RAM
AWG_SOCKET_BUF=1048576   # 1 МБ, минимальный рекомендуемый
```

#### Необязательные -- производительность

Эти параметры имеют смысл только на мощных устройствах с несколькими CPU-ядрами. На типичных MikroTik (1-2 ядра) оставьте значения по умолчанию.

**`AWG_CPU_C2S`**, **`AWG_CPU_S2C`** -- привязка потоков к конкретным ядрам CPU (CPU affinity). Прокси использует два потока: c2s (client→server, обработка исходящих пакетов) и s2c (server→client, обработка входящих). Привязка к разным ядрам исключает миграцию потоков и повышает эффективность кэша.

```
AWG_CPU_C2S=-1   # по умолчанию, ОС выбирает ядро
AWG_CPU_S2C=-1

AWG_CPU_C2S=0    # c2s на ядре 0
AWG_CPU_S2C=1    # s2c на ядре 1
```

**`AWG_BUSY_POLL`** -- включает SO_BUSY_POLL на сокетах. Ядро будет активно опрашивать сетевой драйвер в течение указанного времени (в микросекундах) вместо перехода в сон. Снижает задержку на ~50 мкс, но увеличивает потребление CPU. Требует поддержки со стороны сетевого драйвера.

**На RouterOS этой ручки нет вообще:** ядро собрано без `CONFIG_NET_RX_BUSY_POLL`, и `setsockopt` отвечает `ENOPROTOOPT`. Прокси сообщает исход при старте, чтобы отказ не выглядел как работающая настройка:

```
INFO: busy-poll: kernel 5.6.3, SO_BUSY_POLL=Protocol not available prefer=no budget=no
```

```
AWG_BUSY_POLL=0     # по умолчанию, отключено
AWG_BUSY_POLL=50    # 50 мкс активного ожидания
AWG_BUSY_POLL=100   # 100 мкс, для минимальной задержки
```

**`AWG_SPIN`** -- то же самое, но силами самого прокси и без прав в ядре. После пустого чтения поток указанное число микросекунд перечитывает сокет неблокирующе и только потом даёт себя усыпить, так что всплеск, пришедший внутри этого окна, забирается без пробуждения потока вовсе. Работает там, где `SO_BUSY_POLL` недоступен.

`auto` включает самонастройку: контроллер ходит по лестнице 0/50/100/200/400/800 мкс, оценивает каждое значение по доле дропов в буфере сокета и время от времени перепроверяет остальные. Окна без нагрузки не голосуют, при равном счёте побеждает меньшее значение.

**На роутере, который упирается в процессор, оставляйте 0.** Спин отнимает такты у softirq, который считает криптографию WireGuard, и это стоит пропускной способности: на hAP ax3 замер до Амстердама давал 155-192 Мбит со спином и 194-206 без него. Включать имеет смысл там, где процессора в избытке, а потери есть.

```
AWG_SPIN=0      # по умолчанию, отключено
AWG_SPIN=200    # 200 мкс неблокирующих перечитываний перед сном
AWG_SPIN=auto   # подбирать самому по доле дропов
```

**`AWG_STATS`** -- период строки со статистикой в секундах (`0` -- выключено). Печатается только когда что-то изменилось, поэтому простаивающий туннель лог не засоряет:

```
stats: c2s rx=173791 tx=173791 drop=0 | s2c tx=168402 drop=0 |
       kernel udp in_err=1952 rcvbuf_err=1952 sndbuf_err=0 |
       sockdrop listen=706 remote=233 | spin=0
```

Строка различает три источника потерь: `drop` -- наши (пакет принят, но отправить не удалось), `kernel udp rcvbuf_err` -- ядро выбросило до нас, не дождавшись, пока поток вычерпает буфер, `sockdrop` -- какой именно сокет переполнился. Последнее особенно полезно: при отдаче данные идут в listen-сокет, а обратно возвращаются только ACK, так что пара чисел сразу показывает, в какую сторону туннель не справляется.

Следом, отдельной строкой, идёт отчёт сегментного офлоада (см. [`AWG_NO_GSO`](#переменные-окружения)) -- он печатается только когда офлоад за период хоть раз сработал:

```
gso: c2s msgs=3104 segs=68443 run=22.0 | s2c msgs=4723 segs=95671 run=20.2
```

`msgs` -- сколько было вызовов `sendmsg` со склейкой, `segs` -- сколько пакетов они унесли, `run` -- среднее на вызов. `run=1.0` значит, что склеивать оказалось нечего (разнобой по размерам), отсутствие строки -- что офлоад не сработал ни разу. Отдельная строка, а не лишние поля в `stats:`, потому что RouterOS режет сообщение контейнера на 179 символах вместе с его именем, и `stats:` в это уже упирается.

### Маршрутизация трафика через туннель

Конкретный хост:

```routeros
/ip/route/add dst-address=8.8.8.8/32 gateway=wg-awg-proxy
```

Подсеть:

```routeros
/ip/route/add dst-address=10.0.0.0/8 gateway=wg-awg-proxy
```

Просмотр маршрутов:

```routeros
/ip/route/print where gateway=wg-awg-proxy
```

Удаление маршрута:

```routeros
/ip/route/remove [find where dst-address="8.8.8.8/32" gateway="wg-awg-proxy"]
```

### DNS через туннель

Чтобы DNS-запросы шли через туннель, укажите DNS-сервер и добавьте маршрут к нему:

```routeros
/ip/dns/set servers=8.8.8.8,8.8.4.4
/ip/route/add dst-address=8.8.8.8/32 gateway=wg-awg-proxy
/ip/route/add dst-address=8.8.4.4/32 gateway=wg-awg-proxy
```

### MTU: PPPoE и MTU из конфига

Внешняя UDP-датаграмма считается так:

```
IP (20 для IPv4, 40 для IPv6) + UDP 8 + S4 + WG-заголовок 16 + MTU + тег 16
```

При стандартном MTU 1420 и `S4 = 0` это 1484 байта — влезает в 1500. Но два
случая ломают этот расчёт:

- **PPPoE.** Канал даёт не 1500, а 1492. При `S4 = 12` наружу уходит 1496 байт,
  и каждый полноразмерный пакет едет двумя IP-фрагментами. `change-mss` тут не
  спасает: он про TCP, а переполняется внешняя UDP-датаграмма — задевая в том
  числе QUIC, на котором работают YouTube и Google Play. В конфигураторе есть
  поле **«MTU нижележащего канала»**: поставьте 1492 для PPPoE (меньше — для
  PPPoE поверх VLAN), и MTU туннеля будет посчитан от него.
- **`MTU` в самом `.conf`.** Если сервер прислал `MTU = 1300`, это значение
  теперь переносится на WireGuard-интерфейс. Расчётный потолок по `S4` может
  только опустить его ещё ниже, но не поднять обратно.

### DNS-маршрутизация: что она может и чего не может

Маршрутизация по сервисам работает так: клиент резолвит домен **через роутер**,
RouterOS кладёт полученный адрес в address-list, mangle помечает соединения к
этому адресу. Отсюда два ограничения, о которых стоит знать заранее:

- **Клиент должен спрашивать DNS у роутера.** Устройство с DoH, Android Private
  DNS, прошитым `8.8.8.8` или просто с адресом в своём кэше не создаёт запроса,
  который роутер бы увидел — его трафик пойдёт мимо туннеля. Проверить:
  открыть сервис и посмотреть `/ip/firewall/address-list/print where list=YouTube`.
  Пустой список означает, что проблема в DNS, а не в AWG.
- **Часть сервисов ходит по IP без DNS вообще.** Telegram (MTProto) подключается
  прямо к своим диапазонам, поэтому они прописываются в список статически —
  одними доменами его не поймать.

### Короткий TTL у CDN

У сервисов за Cloudflare TTL бывает в несколько секунд. Адрес успевает выпасть
из address-list, пока браузер ещё держит соединение, и следующее соединение
уходит мимо туннеля. Галочка **«Держать записи DNS address-list дольше TTL»**
задаёт `/ip/dns address-list-extra-time` (по умолчанию 30m).

Настройка общая на весь роутер, а не на конкретный список, поэтому:

- она применяется, **только** если там всё ещё дефолтные `0s` — чужое значение
  скрипт не трогает и пишет об этом в вывод;
- скрипт удаления сбрасывает её в `0s`, **только** если значение осталось ровно
  тем, что поставил конфигуратор.

Слишком большое значение держит устаревшие адреса CDN и может увести в туннель
лишнее, поэтому 10–30 минут — разумный потолок.

### Маршрутизация по address-list (продвинутое)

Для выборочной маршрутизации трафика через туннель используйте routing table и mangle rules.

Создание routing table:

```routeros
/routing/table/add disabled=no fib name=r_to_vpn
```

Маршрут по умолчанию через туннель для этой таблицы:

```routeros
/ip/route/add dst-address=0.0.0.0/0 gateway=wg-awg-proxy routing-table=r_to_vpn
```

Address-list с адресами, которые нужно направить через туннель:

```routeros
/ip/firewall/address-list/add address=8.8.8.8 list=to_vpn
/ip/firewall/address-list/add address=1.1.1.1 list=to_vpn
```

Mangle rules для маркировки трафика:

```routeros
# Пропускаем локальный трафик
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=10.0.0.0/8
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=172.16.0.0/12
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=192.168.0.0/16

# Маркируем соединения к адресам из списка
/ip/firewall/mangle/add chain=prerouting action=mark-connection \
    dst-address-list=to_vpn connection-mark=no-mark \
    new-connection-mark=to-vpn-conn passthrough=yes

# Маркируем маршрутизацию для отмеченных соединений
/ip/firewall/mangle/add chain=prerouting action=mark-routing \
    connection-mark=to-vpn-conn new-routing-mark=r_to_vpn passthrough=yes
```

NAT для маркированного трафика:

```routeros
/ip/firewall/nat/add chain=srcnat action=masquerade routing-mark=r_to_vpn
```

Теперь весь трафик к адресам из списка `to_vpn` будет идти через туннель. Добавляйте адреса в список по мере необходимости.

## Обновление

RouterOS 7.22 умеет `/container/repull`: перекачивает образ из реестра и перезапускает
контейнер с теми же параметрами — переменные окружения, veth, root-dir и comment остаются
на месте. Ключи WireGuard и настройки туннеля не трогаются.

Работает это только для контейнера, установленного **из реестра** (`remote-image=`).
Установка из файла (`file=`) так не обновляется: RouterOS нечего перекачивать. Проверить,
что именно стоит:

```routeros
/container/print detail where comment=awg-proxy-1
```

Непустой `remote-image` — обновление доступно.

### Через конфигуратор

Конфигуратор кладёт на роутер скрипт `<prefix>-update`:

```routeros
/system/script/run awg-proxy-1-update
```

Скрипт сначала спрашивает у GitHub, какой релиз опубликован, и сверяет его с установленной
версией. Если они совпали, он ничего не делает и туннель не трогает. Это важно: `repull`
перезапускает контейнер **всегда**, даже когда образ не изменился, и туннель на это время
(секунд десять) ложится.

Если версия сменилась — скрипт перекачивает образ, дожидается результата, поднимает
контейнер обратно и подчищает за `repull` каталоги `<root-dir>.backup`. Неудачную
перекачку он повторяет один раз, а если контейнер после неё не поднялся — пересобирает его
из реестра с теми же параметрами: провалившийся `repull` оставляет контейнер остановленным
и без образа, то есть туннель лежит до вмешательства.

### Самолечение

Сначала скрипт смотрит на состояние контейнера и только потом на версию — иначе
обновление, которое «окирпичило» контейнер, осталось бы навсегда: версия-то записана,
и следующей ночью скрипт сказал бы «уже актуально» над мёртвым туннелем.

Порядок такой:

1. Контейнер работает и образ на месте — сверяем версию, дальше по обстоятельствам.
2. Контейнер остановлен, но образ цел — просто запускаем. Это дёшево и не рвёт ничего
   лишнего (работает и на RouterOS ниже 7.22, где `repull` недоступен). На RouterOS 7.23+
   до скрипта обычно и не доходит: контейнеру выставлен `restart-policy=on-failure`
   (`restart-interval=30s`, `restart-max-count=10`), и упавший процесс RouterOS поднимает
   сам за полминуты — без скриптов, scheduler'а и netwatch. На версиях ниже параметра нет,
   там подъём остаётся за скриптом.
3. Образа нет или контейнер не поднялся — перекачиваем **независимо от версии**,
   потом повтор, потом пересборка из реестра.
4. Ничего не помогло — строка `:log error` и выход. Scheduler запустит скрипт
   следующей ночью, и попытка повторится.

Версия в `AWG_IMAGE_VER` записывается только после того, как контейнер действительно
поднялся. Пока туннель лежит, маршрут через него отключает netwatch-failover (галочка
«Netwatch failover»), так что роутер остаётся в сети через следующий туннель или напрямую.
Сам скрипт обновления netwatch, маршруты и WireGuard-интерфейс не трогает вообще.

Контейнер, установленный из файла, скрипт не трогает — кроме одного случая: если такой
контейнер мёртв, а RouterOS 7.22+, скрипт переключит его на реестр и вылечит перекачкой.
Это единственный доступный способ поднять его автоматически.

Галочка «Ежедневно проверять обновления контейнера» (включена по умолчанию) добавляет к
скрипту scheduler на 04:30. Если `scheduler` запрещён в device-mode, установка это переживёт
и просто скажет, что автопроверки не будет — обновлять придётся руками той же командой.

### Вручную

```routeros
/container/set [find where comment=awg-proxy-1] remote-image=ghcr.io/timbrs/awg-proxy:latest
/container/repull [find where comment=awg-proxy-1]
```

После перекачки убедитесь, что контейнер работает (`R` в `/container/print`), и удалите
оставшийся `<root-dir>.backup` — RouterOS его не убирает, а на 16 МБ flash такие копии
быстро съедают место.

### Со старой установки из файла

`repull` для неё недоступен. Проще всего переустановить конфигуратором на RouterOS 7.22+:
он поставит контейнер из реестра, и дальше обновления пойдут одной командой. Ключи и
параметры туннеля при этом генерируются заново, так что серверную сторону нужно обновить.

## Удаление

Если установка была через конфигуратор:

```routeros
/system/script/run awg-proxy-uninstall
```

Скрипт удалит контейнер, WireGuard-интерфейс, правила NAT, маршруты, переменные окружения, скрипт обновления с его scheduler'ом, восстановит DNS и удалит себя.

## Устранение неполадок

**Контейнер не запускается** -- проверьте установку пакета container (`/system/package/print`), режим устройства (`/system/device-mode/print`) и свободное место (`/system/resource/print`).

### execvpe /awg-proxy: No such file or directory

Контейнер запускается, но сразу завершается с ошибкой `exited with status 255: execvpe /awg-proxy: No such file or directory`. Это означает, что бинарник не распаковался — образ скачался некорректно или не полностью.

1. Удалите контейнер и root-dir:
```routeros
/container/stop [find where comment=awg-proxy]
:delay 7s
/container/remove [find where comment=awg-proxy]
/file/remove disk1/awg-proxy
:do { /file/remove [find where name~"awg-proxy.*tar"] } on-error={}
```

2. Заново скачайте образ и проверьте размер файла (`/file/print`) — он должен быть 100-300 КБ, не 0.

3. Пересоздайте контейнер.

### Site-to-site: handshake did not complete

В режиме site-to-site (два MikroTik через AWG proxy) handshake не завершается, хотя контейнеры работают. Типичные причины:

**1. Firewall forward chain на стороне B (сервер)**

DSTNAT-трафик идёт через `forward` chain, а не `input`. Если правило `accept` добавлено в конец цепочки, а выше есть `drop` — пакеты не доходят до контейнера.

Диагностика:
```routeros
/ip/firewall/filter/print where chain=forward
```

Исправление — переместите правило в начало:
```routeros
/ip/firewall/filter/remove [find where comment=PREFIX-awg-in]
/ip/firewall/filter/add chain=forward action=accept protocol=udp dst-port=AWG_PORT in-interface-list=WAN place-before=0 comment=PREFIX-awg-in
```

**2. Firewall input chain на стороне B (сервер)**

В reverse-режиме контейнер инициирует NEW-соединение к WG-порту MikroTik (в отличие от стандартного режима, где MikroTik инициирует → ответ контейнера = established). Если veth-интерфейс не в LAN interface-list, input chain дропает пакеты от контейнера.

Исправление:
```routeros
/ip/firewall/filter/add chain=input action=accept protocol=udp src-address=CONTAINER_IP dst-port=WG_PORT place-before=0 comment=PREFIX-wg-in
```

**3. DNS-резолв AWG_REMOTE на стороне A (клиент)**

Если `AWG_REMOTE` указан как hostname, контейнеру нужен работающий DNS: образ собран из `scratch`, своего `/etc/resolv.conf` в нём нет, и прокси пишет его из `AWG_DNS`. Конфигуратор спрашивает этот адрес полем **«DNS-сервер для контейнера»** и не даёт сгенерировать установку с доменом в endpoint без него. Вручную: установите `AWG_DNS=8.8.8.8` или `AWG_DNS=1.1.1.1` в переменных окружения контейнера. Адрес должен быть доступен из контейнера **без** туннеля — сам роутер или публичный резолвер, но не DNS из конфига, который обычно живёт внутри туннеля. Прокси периодически перепроверяет DNS в фоне (`AWG_DNS_REFRESH`, по умолчанию раз в 60 с) и сам переподключается при смене IP сервера. Если DNS тоже идёт через туннель (замкнутый круг) — разрешите hostname вручную и укажите IP:
```routeros
:put [:resolve vpn.example.com]
# Затем пропишите полученный IP в AWG_REMOTE
```

**4. Диагностика через логи**

Включите debug-логирование на обоих контейнерах:
```routeros
/container/envs/add list=PREFIX-env key=AWG_LOG_LEVEL value=debug
```
Перезапустите контейнеры и проверьте логи — они покажут ошибки DNS-резолва, connect, отправку handshake и junk-пакетов.

**5. Резервный профиль (fallback)**

Конфиги site-to-site и server из конфигуратора содержат основной профиль и цепочку отката. Если основная обфускация блокируется, инициатор через `AWG_FB_AFTER` секунд молчания сервера сам переходит к следующей ступени — в логах видно `fallback: remote silent, trying profile stage N` (инициатор) и `c2s: peer uses a different profile stage, switched` (отвечающая сторона). Стартовая строка `config: fallback chain of N profiles` подтверждает, что цепочка задана. Оба конца должны быть сгенерированы одним конфигуратором, иначе их профили не совпадут.

**Нет рукопожатия** -- убедитесь, что все параметры AWG (Jc, Jmin, Jmax, S1, S2, H1--H4) точно совпадают с сервером. Проверьте `AWG_REMOTE`, `AWG_SERVER_PUB` и `AWG_CLIENT_PUB`. Для диагностики установите `AWG_LOG_LEVEL=debug` -- в логах будет видно отправку handshake init и junk-пакетов. Если в логах `remote read error (Connection refused)` -- сервер недоступен или неправильный порт. На ARM64 попробуйте `AWG_NO_GRO=1` -- если ядро не поддерживает GRO, прокси может зависнуть в ожидании ответа.

**Нет трафика после рукопожатия** -- проверьте правило NAT (`/ip/firewall/nat/print`), маршрутизацию и `endpoint-address` пира (должен быть `172.18.0.2`).

**Контейнер перезапускается** -- установите `AWG_LOG_LEVEL=info` и проверьте логи. Частая причина -- отсутствующие переменные окружения.

### Storage device not found

Если при установке появляется ошибка `Storage device usb1 not found or has 0 free space` -- диск не отформатирован или имя точки монтирования не совпадает.

1. Проверьте доступные диски:

```routeros
/disk/print
```

2. Если диск виден как block-устройство, но без раздела -- отформатируйте его в ext4:

```routeros
/disk/format-drive usb1 file-system=ext4 label=usb1
```

3. После форматирования диск будет доступен как mount-point (обычно `usb1`). Проверьте имя через `/disk/print` и используйте его в конфигураторе (поле "Container storage").

> **Важно:** Контейнеры требуют файловую систему ext4. FAT32 не подходит.

### Insufficient disk space

Если при установке контейнера возникает ошибка `Insufficient disk space`, а на внешнем накопителе (USB, SD, NVMe) есть свободное место -- перенастройте директорию для загрузки образов:

```routeros
/container/config set tmpdir=usb1/pull memory-high=200M
```

Замените `usb1` на mount-point вашего накопителя (см. `/disk/print`).

После установки контейнера можно вернуть значение обратно:

```routeros
/container/config set tmpdir="" memory-high=0
```

Если используете конфигуратор -- выберите нужный накопитель в поле "Container storage", и tmpdir будет настроен автоматически.

### not allowed by device-mode

Ошибка `not allowed by device-mode` возникает в трёх случаях:

- При создании контейнера -- не включена поддержка контейнеров (`container=no`)
- При скачивании образа через `/tool/fetch` -- не включён fetch (`fetch=no`)
- При создании планировщика (`/system/scheduler/add`) -- не включён scheduler (`scheduler=no`); без него RU-список не обновляется автоматически, и записи с timeout молча исчезают через 30 дней

Проверьте текущее состояние:

```routeros
/system/device-mode/print
```

Затем включите нужные возможности:

```routeros
/system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
```

Роутер попросит подтверждение -- нажмите кнопку Reset или Mode на корпусе (зависит от модели) в течение нескольких минут, либо дождитесь автоматической перезагрузки. После перезагрузки повторите установку.

### child spawn failed / could not load next layer

На устройствах с 16 МБ flash (hAP ac2, hEX и др.) контейнер может не запускаться с ошибками:
- `child spawn failed: container run error` или `exited with status 255` (RouterOS 7.20)
- `download/extract error: could not load next layer` (RouterOS 7.21+)

Чек-лист:

1. **Формат образа** -- убедитесь, что используете правильный формат:
   - RouterOS 7.21+: `awg-proxy-{arch}.tar.gz` (OCI)
   - RouterOS 7.20 и ниже: `awg-proxy-{arch}-7.20-Docker.tar.gz` (Docker)

2. **tmpdir на USB** -- без этого RouterOS распаковывает образ на внутреннюю flash, которой не хватает (замените `usb1` на ваш mount-point из `/disk/print`):
   ```routeros
   /container/config set tmpdir=usb1/pull
   ```

3. **root-dir** -- указывайте путь к папке на USB, но **не создавайте её вручную** (RouterOS создаст её сам):
   ```routeros
   /container add ... root-dir=usb1/awg-proxy
   ```

4. **Формат USB** -- отформатируйте накопитель в ext4:
   ```routeros
   /disk/format-drive usb1 file-system=ext4 label=usb1
   ```

5. **Место под слои** -- на устройствах с 16 МБ flash во внутренней памяти обычно нет места
   под распаковку. Либо загружайте образ файлом:
   ```routeros
   /container add file=awg-proxy-arm.tar.gz ...
   ```
   либо, если ставите из реестра (`remote-image`, RouterOS 7.22+), сначала уведите временный
   каталог на USB — слои качаются в `tmpdir`, а не в root-dir:
   ```routeros
   /container/config set tmpdir=usb1/pull
   ```
   Конфигуратор делает это сам, когда контейнер ставится не на `disk1`.

### exited with signal 4 (Illegal instruction)

Контейнер сразу падает с ошибкой:

```
*** error: exited with signal 4 (Illegal instruction)
```

Причина: образ собран для более новой архитектуры CPU, чем у роутера. Типичный случай -- **hEX refresh (E50UG)** и **hEX S 2025 (E60iUGS)**: их CPU EN7562CT показывает `architecture-name: arm`, но исполняет только arm32v5-образы ([ограничение RouterOS](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container)), а стандартный `awg-proxy-arm.tar.gz` собран под ARMv7.

Решение -- используйте armv5-образ:

```routeros
/container/remove [find where comment=awg-proxy]
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-armv5.tar.gz" dst-path=awg-proxy-armv5.tar.gz
/container/add file=awg-proxy-armv5.tar.gz ... # остальные параметры как раньше
```

Для RouterOS 7.20 и ниже -- `awg-proxy-armv5-7.20-Docker.tar.gz`. Свежие конфиги из конфигуратора определяют эти устройства автоматически.

### Список RU-адресов не загружается после перезагрузки

При использовании сценария "Весь не-РФ трафик через туннель" список RU-адресов может не загружаться автоматически после перезагрузки роутера. Типичные причины:

**1. Scheduler: `start-time=startup` + `interval` несовместимы**

Это документированное поведение RouterOS: если у scheduler задан `interval` отличный от `0`, trigger `start-time=startup` **не срабатывает**. Scheduler покажет `run-count=0` после перезагрузки.

Решение — два отдельных scheduler:

```routeros
# Для запуска при загрузке (interval ОБЯЗАТЕЛЬНО 0)
/system/scheduler/add name=awg-proxy-ru-startup on-event="/system/script/run awg-proxy-ru-update" start-time=startup interval=0 comment=awg-proxy

# Для ежедневного обновления
/system/scheduler/add name=awg-proxy-ru-daily on-event="/system/script/run awg-proxy-ru-update" start-time=04:00:00 interval=1d comment=awg-proxy
```

Если у вас один scheduler с `start-time=startup interval=1d` — удалите его и создайте два.

**2. USB-диск монтируется с задержкой**

USB-накопитель появляется через 5-30 секунд после загрузки. Если скрипт запускается раньше, он не найдёт файл `.rsc` на USB. Конфигуратор добавляет в скрипт ожидание монтирования (до 60 секунд):

```routeros
:if ($disk != "disk1") do={
  :local waited 0
  :while ([:len [/disk/find where mount-point=$disk]] = 0 && $waited < 60) do={
    :delay 5s
    :set waited ($waited + 5)
  }
}
```

**3. Сеть не готова при загрузке**

DHCP и DNS могут быть недоступны в первые секунды после boot. Скрипт ожидает доступность сети (до 60 секунд) перед скачиванием:

```routeros
:local waitNet 0
:while ($waitNet < 60) do={
  :do { :resolve "github.com"; :set waitNet 99 } on-error={ :delay 5s; :set waitNet ($waitNet + 5) }
}
```

Если кешированный `.rsc` файл есть на диске — он импортируется сразу (не дожидаясь сети), и маршрутизация работает с первых секунд. Свежая версия скачивается позже.

**4. Потеря времени при перезагрузке (WG handshake fails)**

Некоторые модели MikroTik (без батарейки RTC) теряют время при перезагрузке. Если часы сильно отстают, WireGuard-сервер отклоняет handshake (защита TAI64N от replay-атак). Пока handshake не пройдёт — туннель не работает и LAN-трафик теряется.

Последовательность при загрузке:
1. Время сбито → WG handshake не проходит → туннель не работает
2. NTP синхронизирует время (~5-15 сек) → время исправлено
3. WG handshake проходит → туннель поднимается
4. USB монтируется (~10-30 сек) → RU-список загружается из кеша

**Важно:** NTP-трафик самого роутера идёт напрямую (не через туннель), т.к. mangle-правила действуют только на `in-interface-list=LAN`. Проблема не в маршрутизации NTP, а в скорости синхронизации.

Рекомендации:

```routeros
# Используйте IP-адреса для NTP (не требуют DNS-резолва при старте)
/system/ntp/client/set enabled=yes servers=216.239.35.0,216.239.35.4,ntp2.vniiftri.ru,ntp.ix.ru

# Добавьте NTP и DNS серверы в RU list (для LAN-устройств, чтобы их NTP тоже шёл напрямую)
/ip/firewall/address-list/add list=RU address=216.239.35.0 comment=awg-proxy-ntp
/ip/firewall/address-list/add list=RU address=216.239.35.4 comment=awg-proxy-ntp
/ip/firewall/address-list/add list=RU address=8.8.8.8 comment=awg-proxy-dns
/ip/firewall/address-list/add list=RU address=8.8.4.4 comment=awg-proxy-dns
```

Окно без интернета для LAN ~10-20 секунд (до синхронизации NTP + WG handshake). NTP time-jump также может сбить планирование scheduler.

### Handshake не проходит после восстановления из бэкапа

После воссановления RouterOS из бэкапа (backup/restore) WireGuard handshake не завершается, хотя контейнеры работают, veth-интерфейсы в состоянии running и пинг до контейнера проходит. В логах:

```
wg-awg-proxy: [peer] ...: Handshake for peer did not complete after 5 seconds, retrying (try 2)
```

**При��ина:** восстановление из бэкапа сбрасывает системные часы на дату создания бэкапа. WireGuard использует TAI64N timestamp в handshake init для защиты от replay-атак — сервер запоминает последний timestamp каждого пира и отбрасывает handshake с более старым временем. Если часы роутера отстают от реального времени, сервер молча игнорируе�� все handshake-пакеты.

**Диагностика:**

```routeros
/system/clock/print
# Если дата не совпадает с текущей — это причина
```

**Исправление:**

1. Установите правильное время:
```routeros
/system/clock/set date=apr/05/2026 time=12:00:00
```

2. Включите NTP-клиент для автоматической синхронизации:
```routeros
/system/ntp/client/set enabled=yes servers=time.google.com,pool.ntp.org
```

3. Перезапустите контейнеры и сбросьте WG-пиры для принудительного нового handshake:
```routeros
/container/stop [find]
:delay 5s
/container/start [find]
# Сбросить пиры (disable/enable)
/interface/wireguard/peers/set [find where !disabled] disabled=yes
:delay 2s
/interface/wireguard/peers/set [find where disabled] disabled=no
```

> **Совет:** После каждого восстановления из бэкапа первым делом проверяйте системные часы (`/system/clock/print`).

## Сборка из исходников

Требуется C компилятор (gcc/musl-gcc), Docker (для контейнерных образов) и make.

```bash
# Тесты
make test

# Локальная сборка бинарника
make build

# Docker-образы (OCI, для RouterOS 7.21+)
make docker-arm64    # ARM64
make docker-arm      # ARM v7
make docker-armv5    # ARM v5
make docker-amd64    # x86_64
make docker-all      # Все архитектуры

# Docker-образы (классический формат, для RouterOS 7.20 и ниже)
make docker-arm64-7.20-docker
make docker-arm-7.20-docker
make docker-armv5-7.20-docker
make docker-amd64-7.20-docker
make docker-all-7.20-docker
```

Артефакты создаются в директории `builds/`.

У конфигуратора есть отдельный набор проверок на jsdom (только для разработки -- у самого прокси зависимостей нет):

```bash
npm install jsdom
node tests/conf3.0-ipv6.test.js
node tests/conf3.0-dns.test.js
node tests/conf3.0-ports.test.js
node tests/conf3.1.test.js
```

## Лицензия

MIT -- см. [LICENSE](LICENSE).
