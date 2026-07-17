> This project is not affiliated with or endorsed by MikroTik / SIA Mikrotikls

# awg-proxy -- AmneziaWG для MikroTik

[![C11](https://img.shields.io/badge/C-11-blue)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[English version](README_en.md) | [GitHub](https://github.com/timbrs/amneziawg-mikrotik-c)

Легковесный Docker-контейнер, который позволяет MikroTik подключаться к серверам AmneziaWG. Весь трафик шифруется нативным WireGuard-клиентом роутера, а контейнер только преобразует формат пакетов.

## Содержание

- [Как это работает](#как-это-работает)
- [Быстрый старт (конфигуратор)](#быстрый-старт-конфигуратор)
- [Требования](#требования)
- [Ручная установка](#ручная-установка)
- [Получение параметров AWG](#получение-параметров-awg)
- [Серверный режим (1:N) — подробная настройка](#серверный-режим-1n--подробная-настройка)
  - [Маршрутизация между клиентами](#маршрутизация-между-клиентами)
- [Дополнительные настройки](#дополнительные-настройки)
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
2. Откройте [конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/configurator.html)
3. Вставьте содержимое `.conf`-файла
4. Скопируйте сгенерированные команды и выполните их в терминале MikroTik

Готово. Конфигуратор работает оффлайн, данные не отправляются на сервер.

<video src="https://github.com/user-attachments/assets/f0100789-0a23-42f8-a67f-085e5f8d13a3" controls width="100%"></video>

![Замеры скорости на MikroTik AX3](https://github.com/user-attachments/assets/9fb34444-681b-4f34-8306-8f202f1b121d)

*Замеры скорости на устройстве MikroTik AX3*

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

2. Откройте [конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/configurator.html), вставьте `.conf` и выполните команды на MikroTik.

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
| `AWG_REMOTE` | Да | -- | Адрес AWG-сервера |
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
| `AWG_I1`--`AWG_I5` | Нет | -- | CPS-шаблоны (v1.5/v2) |
| `AWG_MODE` | Нет | `normal` | Режим работы: `normal`, `reverse`, `server` |
| `AWG_FB_H1`--`AWG_FB_H4` | Нет | -- | Резервный (v1) профиль обфускации: типы сообщений. Включает fallback (только `normal`/`reverse`); требует `AWG_S4=0` |
| `AWG_FB_S1`, `AWG_FB_S2` | Да** | -- | Паддинг init/response для резервного профиля |
| `AWG_FB_S3` | Нет | `0` | Паддинг cookie reply для резервного профиля |
| `AWG_FB_AFTER` | Нет | `20` | Секунд тишины от сервера до пробы другого профиля (инициатор) |
| `AWG_SRC_PORT` | Нет | auto | Исходящий порт к серверу |
| `AWG_TIMEOUT` | Нет | `180` | Таймаут бездействия (сек) |
| `AWG_DNS_REFRESH` | Нет | `60` | Период фоновой проверки DNS для hostname в AWG_REMOTE (сек, `0` = выкл) |
| `AWG_LOG_LEVEL` | Нет | `info` | Уровень логирования |
| `AWG_NO_GRO` | Нет | `0` | Отключить UDP GRO |
| `AWG_SOCKET_BUF` | Нет | `16777216` | Размер буфера сокета |
| `AWG_CPU_C2S` | Нет | `-1` | CPU для потока client→server |
| `AWG_CPU_S2C` | Нет | `-1` | CPU для потока server→client |
| `AWG_BUSY_POLL` | Нет | `0` | SO_BUSY_POLL таймаут (мкс) |
| `AWG_DNS` | Нет | -- | DNS-сервер для резолва hostname в AWG_REMOTE |

`*` В `server` режиме должен быть задан либо legacy `AWG_CLIENT_PUB`, либо явный direct-peer список `AWG_CLIENT_PUBS` / `AWG_CLIENT_PUBS_FILE`.

`**` Обязательны только если включён резервный профиль (задан `AWG_FB_H1`).

Версия протокола определяется автоматически: **v2** если заданы S3/S4 или H в виде диапазонов, **v1.5** если заданы CPS-шаблоны (I1-I5), иначе **v1**.

**Резервный профиль (site-to-site, обратная совместимость / устойчивость к DPI).** Основной профиль (`AWG_S*`, `AWG_H*`, `AWG_I*`) может быть v2, а `AWG_FB_*` задаёт второй, v1-профиль (фиксированные H, без S3/S4/CPS). Инициатор (`normal`) работает на основном профиле и, если сервер молчит дольше `AWG_FB_AFTER` секунд, редко переключается на резервный и обратно, пока не найдёт рабочий. Отвечающая сторона (`reverse`) принимает оба профиля и отвечает тем, которым пришёл handshake. Конфигуратор для сценария site-to-site генерирует основной+резервный профили одинаковыми на обеих сторонах: если основную обфускацию начинают блокировать, туннель автоматически откатывается на резервную. Требует `AWG_S4=0`; в режиме `server` не поддерживается.

### Подробное описание переменных

#### Обязательные -- параметры обфускации

Все значения берутся из `.conf`-файла AmneziaVPN (секция `[Interface]` и `[Peer]`). Должны **точно** совпадать с параметрами сервера, иначе handshake не пройдёт.

**`AWG_LISTEN`** -- адрес и порт, на котором прокси принимает UDP-пакеты от WireGuard-клиента роутера. Формат: `адрес:порт` или `:порт` (слушать на всех интерфейсах).

```
AWG_LISTEN=:51820          # все интерфейсы, порт 51820 (стандартный)
AWG_LISTEN=172.18.0.2:9000 # конкретный адрес и порт
```

**`AWG_REMOTE`** -- адрес и порт AWG-сервера (`Endpoint` из `[Peer]`). Поддерживаются IP-адреса и доменные имена.

```
AWG_REMOTE=1.2.3.4:443        # IP + порт
AWG_REMOTE=vpn.example.com:51820  # домен + порт
```

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

**`AWG_I1`--`AWG_I5`** -- CPS-шаблоны (Constant Packet Size). До 5 шаблонов для генерации пакетов фиксированного формата перед handshake. Если заданы без S3/S4/диапазонов H, прокси работает в режиме v1.5. Формат шаблона описан в документации AWG.

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

**`AWG_SRC_PORT`** -- исходящий UDP-порт для соединения с AWG-сервером. По умолчанию (`auto`) прокси использует порт клиента WireGuard -- это нужно для корректной работы NAT на роутере. Если задано число, используется фиксированный порт.

```
AWG_SRC_PORT=auto    # по умолчанию, копирует порт WG-клиента
AWG_SRC_PORT=0       # то же что auto
AWG_SRC_PORT=12345   # фиксированный порт 12345
```

**`AWG_TIMEOUT`** -- таймаут бездействия в секундах. Если за это время не было ни одного пакета в любую сторону, прокси переподключается к серверу (re-resolve DNS + новый сокет). Полезно при смене IP-адреса сервера за DNS.

```
AWG_TIMEOUT=180   # по умолчанию, 3 минуты
AWG_TIMEOUT=60    # агрессивный таймаут для нестабильных соединений
AWG_TIMEOUT=3600  # 1 час, для стабильных каналов
```

**`AWG_DNS_REFRESH`** -- период фоновой проверки DNS в секундах, когда `AWG_REMOTE` задан hostname'ом (для literal IP проверка выключена). Прокси периодически заново резолвит hostname и, если текущий IP сервера исчез из A-записей две проверки подряд (защита от round-robin DNS), переподключается на новый адрес, не дожидаясь `AWG_TIMEOUT`. Гранулярность -- 5 секунд. Реконнект сбрасывает сессию клиента (как и при таймауте) -- WireGuard сам выполнит новый handshake.

```
AWG_DNS_REFRESH=60   # по умолчанию, проверка раз в минуту
AWG_DNS_REFRESH=0    # выключить фоновую проверку DNS
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

**`AWG_NO_GRO`** -- отключает UDP GRO (Generic Receive Offload) на сокете к серверу. GRO объединяет несколько входящих UDP-пакетов в один буфер, уменьшая количество системных вызовов. Включён по умолчанию, если ядро поддерживает. На некоторых платформах (ARM64 в RouterOS) ядро принимает setsockopt, но GRO фактически не работает -- в этом случае прокси зависает в ожидании пакетов. Установите `AWG_NO_GRO=1` для принудительного отключения.

```
AWG_NO_GRO=0   # по умолчанию, GRO включён (если ядро поддерживает)
AWG_NO_GRO=1   # принудительно отключить GRO, использовать recvmmsg
```

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

```
AWG_BUSY_POLL=0     # по умолчанию, отключено
AWG_BUSY_POLL=50    # 50 мкс активного ожидания
AWG_BUSY_POLL=100   # 100 мкс, для минимальной задержки
```

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

## Удаление

Если установка была через конфигуратор:

```routeros
/system/script/run awg-proxy-uninstall
```

Скрипт удалит контейнер, WireGuard-интерфейс, правила NAT, маршруты, переменные окружения, восстановит DNS и удалит себя.

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

Если `AWG_REMOTE` указан как hostname, контейнеру нужен работающий DNS. Установите `AWG_DNS=8.8.8.8` или `AWG_DNS=1.1.1.1` в переменных окружения контейнера. Прокси периодически перепроверяет DNS в фоне (`AWG_DNS_REFRESH`, по умолчанию раз в 60 с) и сам переподключается при смене IP сервера. Если DNS тоже идёт через туннель (замкнутый круг) — разрешите hostname вручную и укажите IP:
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

Конфиги site-to-site из конфигуратора содержат основной (v2) и резервный (v1) профили обфускации. Если основная обфускация блокируется, инициатор через `AWG_FB_AFTER` секунд молчания сервера сам переключается на резервный профиль — в логах видно `fallback: remote silent, trying v1 fallback profile` (инициатор) и `c2s: peer uses v1 fallback profile, switched` (отвечающая сторона). Стартовая строка `config: v1 fallback enabled` подтверждает, что резервный профиль задан. Оба конца должны быть сгенерированы одним конфигуратором, иначе их профили не совпадут.

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

5. **Загрузка из файла** -- на устройствах с 16 МБ flash загружайте образ через файл, а не remote-image:
   ```routeros
   /container add file=awg-proxy-arm.tar.gz ...
   ```

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

## Лицензия

MIT -- см. [LICENSE](LICENSE).
