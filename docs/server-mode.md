# Серверный режим (1:N) — подробная настройка

> Раздел документации [awg-proxy](../README.md): контейнер RouterOS, который подключает MikroTik к серверам AmneziaWG. Проще всего ставить [конфигуратором](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html): он сам генерирует все команды. Остальные разделы: [установка](install.md) · [обновление](update.md) · [переменные](env.md) · [AWG 3.x и IPv6](protocol.md) · [маршрутизация и MTU](routing.md) · [серверный режим](server-mode.md) · [неполадки](troubleshooting.md) · [FAQ](faq.md).

Серверный режим позволяет одному awg-proxy обслуживать множество MikroTik-клиентов одновременно. Это аналог полноценного AmneziaWG-сервера, но реализованный через связку WireGuard-сервер + awg-proxy.

## Архитектура

```
MikroTik1 + awg-proxy(normal) ──AWG──┐
MikroTik2 + awg-proxy(normal) ──AWG──┤──> VPS: [awg-proxy server :443] ──WG──> [WG-сервер :51820]
MikroTik3 + awg-proxy(normal) ──AWG──┘
```

- **WireGuard-сервер** — обычный WG-сервер, принимает стандартный WG-трафик от awg-proxy. Каждый MikroTik-клиент — отдельный peer.
- **awg-proxy в режиме `server`** — слушает публичный порт (например, 443/udp), принимает AWG-трафик от клиентских normal-прокси, преобразует в стандартный WG и пересылает локальному WG-серверу. Таблица сессий маршрутизирует ответы к правильному клиенту.

## 1. Настройка серверной стороны (VPS)

### WireGuard-сервер

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

### awg-proxy в режиме server

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

## 2. Настройка клиентской стороны (MikroTik)

Каждый MikroTik-клиент использует стандартный awg-proxy в режиме `normal` (по умолчанию).

1. Создайте файл `.conf` для каждого клиента. В секции `[Peer]`:
   - `Endpoint` = IP_вашего_VPS:443 (порт AWG_LISTEN сервера)
   - `PublicKey` = публичный ключ WG-сервера

2. Откройте [конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), вставьте `.conf` и выполните команды на MikroTik.

3. Параметры обфускации (Jc, S1, S2, H1--H4) должны **точно совпадать** с параметрами на сервере.

## 3. Добавление нового клиента

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

## 4. Проверка работы

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

## Несколько портов на хабе

Хаб может принимать клиентов не на одном порту, а на диапазоне: клиентские роутеры тогда берут случайный порт при каждом подключении и раз в 30–120 минут меняют его на ходу. Для хаба на MikroTik это поле **«Ещё порты сервера»** в конфигураторе, для хаба на VPS — скрипт `awg-ports.sh`. Как это включить, в том числе на уже стоящем хабе, — [ports.md](ports.md#хаб-на-mikrotik-серверный-режим). Хабу нужен awg-proxy 1.4.4 или новее: старый уводил ответы на старый порт, и смена у клиентов каждый раз откатывалась.

## Маршрутизация между клиентами

В топологии "звезда" каждый клиент по умолчанию видит только сеть сервера. Клиент 1 (192.168.8.0/24) **не знает** о сети клиента 2 (192.168.11.0/24) и наоборот. Трафик между клиентами идёт через сервер, но маршруты нужно прописать вручную.

### Схема

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

### Как добавить

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

### Проверка

С устройства в сети клиента 1 пингуйте устройство в сети клиента 2:

```
ping 192.168.11.X
```

Трафик пойдёт: клиент 1 → WG → сервер → WG → клиент 2. Это двойной hop через сервер — задержка будет суммой двух туннелей.

### Если клиентов много

Для N клиентов нужно:
- На сервере: в `allowed-address` каждого пира добавить подсети **всех остальных** клиентов
- На каждом клиенте: добавить маршруты ко **всем остальным** клиентским подсетям

При 3+ клиентах это проще автоматизировать скриптом на сервере.
