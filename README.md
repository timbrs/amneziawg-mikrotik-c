> This project is not affiliated with or endorsed by MikroTik / SIA Mikrotikls

# awg-proxy -- AmneziaWG для MikroTik

[![C11](https://img.shields.io/badge/C-11-blue)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[English version](README_en.md) | [**Конфигуратор**](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html) | [Релизы](https://github.com/timbrs/amneziawg-mikrotik-c/releases) | [FAQ](docs/faq.md) | [VPS для своего VPN — $34 в год](https://freakhosting.com/clientarea/aff.php?aff=730&redirectto=https://freakhosting.com/budget-vps/)

**awg-proxy подключает роутер MikroTik к VPN-серверу AmneziaWG** (его ставит, например, приложение AmneziaVPN). AmneziaWG — это WireGuard с замаскированными пакетами: по их виду протокол не распознать. Сам MikroTik умеет только обычный WireGuard, поэтому awg-proxy ставится на роутер маленьким контейнером, и дальше всё работает как обычный VPN-туннель роутера: для всех устройств в сети, без приложений на телефонах и компьютерах.

Ставится за 5–10 минут: [конфигуратор](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html) в браузере превращает `.conf` из AmneziaVPN в готовые команды для MikroTik.

## Быстрый старт

### 1. Проверьте, подходит ли роутер

Нужен MikroTik с процессором **ARM, ARM64 или x86** и RouterOS **7.4 или новее**. Откройте терминал (WinBox → **New Terminal**) и выполните:

```routeros
/system/resource/print
```

В строке `architecture-name` должно быть `arm64`, `arm` или `x86_64`. Если там `mipsbe`, `mmips` или `smips` (hEX, hEX S, hAP ac lite, hAP lite) — роутер **не подойдёт**: MikroTik не делает для них пакет контейнеров. Модели по архитектурам — в [таблице ниже](#пойдёт-ли-на-моём-роутере).

### 2. Подготовьте роутер (один раз)

Без этого шага команды из конфигуратора не сработают.

1. **Поставьте пакет `container`.** Проверить: `/system/package/print` — в списке должен быть `container`. Если его нет:
   - **RouterOS 7.18 и новее** — роутер скачает пакет сам. В WinBox: **System → Packages → Check For Updates**, в списке появится `container`: выделите его, нажмите **Enable**, затем **Apply Changes**. Роутер скачает пакет и перезагрузится. То же в терминале:

     ```routeros
     /system/package/update/check-for-updates
     /system/package/enable container
     /system/package/apply-changes
     ```

   - **RouterOS старше 7.18** — на [mikrotik.com/download](https://mikrotik.com/download) скачайте **Extra packages** для своей архитектуры и **точно своей** версии RouterOS, достаньте из архива файл `container-….npk`, перетащите его в окно **Files** в WinBox и перезагрузите роутер (`/system/reboot`).
2. **Разрешите контейнеры в режиме устройства:**

   ```routeros
   /system/device-mode/update container=yes fetch=yes bandwidth-test=yes scheduler=yes
   ```

   RouterOS попросит подтвердить, что вы рядом с роутером: **в течение 5 минут** нажмите кнопку на корпусе (Reset или Mode) или выключите и включите питание. Проверить: `/system/device-mode/print` — везде должно быть `yes`.

### 3. Возьмите `.conf`

В приложении **AmneziaVPN**: выберите подключение → **Поделиться** → протокол **AmneziaWG**, формат **AmneziaWG Format** → сохраните файл. Своего сервера нет — подойдёт бесплатный конфиг Cloudflare WARP, в конфигураторе для него есть отдельный режим с инструкцией.

> [!TIP]
> **Кнопка «Sponsor» наверху страницы — это аренда VPS, а не сбор денег.**
>
> Она ведёт на [Freakhosting BVPS-2](https://freakhosting.com/clientarea/aff.php?aff=730&redirectto=https://freakhosting.com/budget-vps/) — сервер для своего VPN:
>
> - **$34 в год**, около 2700 ₽;
> - 2 vCPU, 2 ГБ RAM, 40 ГБ NVMe;
> - порт 10 Гбит/с, трафик без ограничений.
>
> Вы платите хостеру обычную цену, а проекту хостер отчисляет небольшой бонус — выгодно обоим. Сервер AmneziaWG на этот VPS ставит само приложение AmneziaVPN.
>
> <sub>Цена на момент проверки, может меняться.</sub>

### 4. Сгенерируйте команды и вставьте в терминал

Откройте [**конфигуратор**](https://timbrs.github.io/amneziawg-mikrotik-c/conf3.0.html), вставьте содержимое `.conf`, нажмите **«Сгенерировать»** и выполните команды в терминале MikroTik по порядку. Конфигуратор работает офлайн в браузере: ключи никуда не отправляются. Те же команды можно получить и [из командной строки](#генератор-команд-из-командной-строки).

Готово: туннель поднят, весь трафик или только нужные сервисы (как выберете в конфигураторе) идут через сервер AmneziaWG.

> **Совет.** Если сервер свой, включите на нём приём AmneziaWG сразу на тысячах портов — это одна команда на сервере, [docs/ports.md](docs/ports.md). Тогда прокси берёт случайный порт при каждом подключении, а если какой-то порт перестанет отвечать, просто уйдёт на другой.

<video src="https://github.com/user-attachments/assets/f0100789-0a23-42f8-a67f-085e5f8d13a3" controls width="100%"></video>

## Что умеет

- **Маскирует WireGuard.** Роутер шифрует трафик своим обычным WireGuard, а awg-proxy на лету переодевает каждый пакет в формат AmneziaWG: другие заголовки, случайный паддинг, мусорные пакеты перед рукопожатием. Сервер принимает такие пакеты как свои, а DPI провайдера не узнаёт в них WireGuard. Ключей awg-proxy не знает и содержимое не видит.
- **Все версии AmneziaWG:** 1.0, 1.5, 2.0, 3.0 и 3.1. Какая нужна, прокси понимает сам по параметрам из `.conf`, настраивать ничего не надо.
- **Бесплатный Cloudflare WARP**, если своего сервера нет.
- **Через туннель — что нужно:** весь трафик, только выбранные сервисы (YouTube, Telegram, Instagram и другие) или всё, кроме российских сайтов. Это выбирается в конфигураторе.
- **Несколько портов сервера.** Если сервер принимает AmneziaWG на диапазоне портов, прокси берёт случайный порт при каждом подключении, уходит с неотвечающего примерно за 15 секунд и раз в 30–120 минут меняет порт на ходу — без обрыва, игры и стримы этого не замечают. Как настроить сервер (VPS одной командой или хаб на MikroTik) — [docs/ports.md](docs/ports.md).
- **Обновляется и лечится сам.** На RouterOS 7.22+ контейнер ставится из реестра, обновляется одной командой или ночью по расписанию и поднимается сам, если упал.
- **Лёгкий и быстрый.** Образ — около 90 КБ, в памяти — 1–2 МБ. На hAP ax² и ax³ — сотни Мбит/с: упирается в процессор роутера, который шифрует WireGuard, а не в прокси.
- **Не только клиент.** Два MikroTik друг с другом (site-to-site) и хаб на много клиентов на VPS или MikroTik (серверный режим).

Это актуальный репозиторий. Старая Go-версия [timbrs/amneziawg-mikrotik](https://github.com/timbrs/amneziawg-mikrotik) больше не развивается.

Последняя сверка документации: 30 октября 2026, awg-proxy 1.4.5. Проверено на RouterOS 7.24.4 (hAP ax², arm64) и hAP ac² (arm) с серверами AmneziaWG 3.1 (amneziawg-go и модуль ядра).

![Замеры скорости на MikroTik AX3](https://github.com/user-attachments/assets/9fb34444-681b-4f34-8306-8f202f1b121d)

*Замеры скорости на MikroTik hAP ax³*

## Пойдёт ли на моём роутере

Точный ответ даёт `/system/resource/print`, поле `architecture-name`: `arm64`, `arm` и `x86_64` подходят, `mipsbe`, `mmips`, `smips`, `tile` и `ppc` — нет. Конфигуратор сам выбирает образ по архитектуре, версии RouterOS и модели платы.

| Модели | Архитектура | Образ для RouterOS 7.21+ | Заметки |
|--------|-------------|--------------------------|---------|
| hAP ax², hAP ax³, RB5009, CCR2004, CCR2116, CCR2216 | arm64 | `awg-proxy-arm64.tar.gz` или `ghcr.io/timbrs/awg-proxy:latest` | Самый быстрый вариант. На 4 ядрах доступны пины потоков и `AWG_RT` |
| hAP ac², hAP ac³, RB4011, RB3011 | arm | `awg-proxy-arm.tar.gz` или `ghcr.io/timbrs/awg-proxy:latest` | У hAP ac² 16 МБ flash: ставьте на USB-накопитель в ext4, см. [неполадки](docs/troubleshooting.md#child-spawn-failed--could-not-load-next-layer) |
| hEX refresh (E50UG), hEX S 2025 (E60iUGS), hAP ax S (E62iUGS) | arm, но процессор понимает только ARMv5 | `awg-proxy-armv5.tar.gz` или `ghcr.io/timbrs/awg-proxy:latest-armv5` | Обычный arm-образ падает с `signal 4 (Illegal instruction)` |
| x86, CHR | x86_64 | `awg-proxy-amd64.tar.gz` или `ghcr.io/timbrs/awg-proxy:latest` | |
| hEX (RB750Gr3), hEX S (RB760iGS), hAP ac lite, hAP ac, hAP lite, RB2011, mAP | MIPS | — | **Не поддерживается**: у RouterOS нет пакета container для MIPS |
| CCR1009, CCR1016, CCR1036, CCR1072 | tile | — | **Не поддерживается** |

Для RouterOS 7.20 и ниже — те же имена с суффиксом `-7.20-Docker`, например `awg-proxy-arm-7.20-Docker.tar.gz`.

**Что ещё нужно:** RouterOS 7.4+ с пакетом `container`, 16 МБ свободной оперативной памяти и около 1 МБ места на диске (на роутерах с 16 МБ flash — USB-накопитель). Сервер AmneziaWG с конфигом `.conf` из AmneziaVPN или конфиг Cloudflare WARP.

## Как это работает

```
Устройства в сети ──> MikroTik WireGuard ──UDP──> [awg-proxy] ──UDP──> сервер AmneziaWG
                      (шифрует, как обычно)     (переодевает пакеты)
```

Роутер сам шифрует трафик штатным WireGuard и отправляет пакеты не на сервер, а в контейнер awg-proxy на этом же роутере. awg-proxy меняет у каждого пакета то, по чему DPI узнаёт WireGuard: тип в заголовке, длину (добавляет паддинг), а перед рукопожатием шлёт мусорные пакеты. Ответы сервера проходят обратную переделку. Содержимое пакетов и ключи остаются как есть: расшифровать трафик прокси не может.

Другие режимы:

- **site-to-site (reverse)** — два MikroTik соединяются друг с другом через AmneziaWG без отдельного сервера:
  `MikroTik1 WG ↔ [awg-proxy normal] ──AWG──> [awg-proxy reverse] ↔ MikroTik2 WG`. Только точка-точка.
- **server (хаб 1:N)** — один awg-proxy на VPS или MikroTik принимает много клиентов и отдаёт их обычному WireGuard-серверу: [docs/server-mode.md](docs/server-mode.md).

## Документация

| Файл | О чём |
|------|-------|
| [faq.md](docs/faq.md) | Частые вопросы своими словами: пойдёт ли на моём роутере, какая будет скорость, чем отличается от других контейнеров |
| [install.md](docs/install.md) | Где взять `.conf`, WARP, ручная установка по шагам, установка без доступа к GitHub |
| [ports.md](docs/ports.md) | Сервер на диапазоне портов: скрипт одной командой, ручная настройка, как указать в конфигураторе |
| [update.md](docs/update.md) | Обновление из реестра, самолечение, переход со старой установки из файла |
| [env.md](docs/env.md) | Все переменные окружения контейнера |
| [protocol.md](docs/protocol.md) | AmneziaWG 3.0 и 3.1 на проводе, цепочка отката, IPv6 |
| [routing.md](docs/routing.md) | Маршруты через туннель, DNS, MTU и PPPoE, address-list |
| [server-mode.md](docs/server-mode.md) | Хаб 1:N на VPS или MikroTik |
| [troubleshooting.md](docs/troubleshooting.md) | Тексты ошибок и что с ними делать |

Краткая выжимка со ссылками для ИИ-ассистентов — [llms.txt](https://timbrs.github.io/amneziawg-mikrotik-c/llms.txt). Тем, кто правит код, — [AGENTS.md](AGENTS.md).

## Установка без доступа к GitHub

Включите в конфигураторе **офлайн-режим**: шаг 0 спросит у роутера архитектуру и версию RouterOS и напечатает, какие файлы скачать и куда положить. Подробно — [docs/install.md](docs/install.md#установка-без-доступа-к-github).

## Ручная установка

Без конфигуратора, командами RouterOS: пакет container, образ (файлом или из реестра `ghcr.io/timbrs/awg-proxy`), veth, WireGuard-интерфейс, переменные окружения, контейнер — [docs/install.md](docs/install.md#ручная-установка).

Если в `.conf` не хватает параметров и что делает режим WARP — [docs/install.md](docs/install.md#получение-параметров-awg).

## Обновление

На RouterOS 7.22+ контейнер, поставленный из реестра, обновляется одной командой `/system/script/run awg-proxy-1-update` (скрипт кладёт конфигуратор; он же обновляет ночью в 04:30 и поднимает упавший контейнер). Вручную — `/container/repull`. Подробно — [docs/update.md](docs/update.md).

## Удаление

Если установка была через конфигуратор:

```routeros
/system/script/run awg-proxy-uninstall
```

Скрипт удалит контейнер, WireGuard-интерфейс, правила NAT, маршруты, переменные окружения, скрипт обновления с его scheduler'ом, восстановит DNS и удалит себя.

## Устранение неполадок

Ошибки с готовыми решениями — в [docs/troubleshooting.md](docs/troubleshooting.md):

- [`not allowed by device-mode`](docs/troubleshooting.md#not-allowed-by-device-mode) — не сделан шаг 2 быстрого старта
- [`execvpe /awg-proxy: No such file or directory`](docs/troubleshooting.md#execvpe-awg-proxy-no-such-file-or-directory) — образ скачался не полностью
- [`Site-to-site: handshake did not complete`](docs/troubleshooting.md#site-to-site-handshake-did-not-complete)
- [`Storage device not found`](docs/troubleshooting.md#storage-device-not-found) — диск не отформатирован в ext4
- [`Insufficient disk space`](docs/troubleshooting.md#insufficient-disk-space)
- [`no space to extract layer`](docs/troubleshooting.md#no-space-to-extract-layer) — нет места под распаковку образа, есть обход через RAM
- [`start failed: no default dns servers`](docs/troubleshooting.md#start-failed-no-default-dns-servers) — RouterOS 7.24, у контейнера не задан DNS
- [`child spawn failed` / `could not load next layer`](docs/troubleshooting.md#child-spawn-failed--could-not-load-next-layer) — роутеры с 16 МБ flash
- [`exited with signal 4 (Illegal instruction)`](docs/troubleshooting.md#exited-with-signal-4-illegal-instruction) — hEX refresh / hEX S 2025 / hAP ax S, нужен образ armv5
- [Список RU-адресов не загружается после перезагрузки](docs/troubleshooting.md#список-ru-адресов-не-загружается-после-перезагрузки)
- [Handshake не проходит после восстановления из бэкапа](docs/troubleshooting.md#handshake-не-проходит-после-восстановления-из-бэкапа)

Нет рукопожатия, нет трафика после рукопожатия, контейнер перезапускается — в начале того же файла. Вопросы своими словами — в [FAQ](docs/faq.md).

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

Артефакты создаются в директории `builds/`. Устройство кода, инварианты и как гонять тесты — в [AGENTS.md](AGENTS.md).

У конфигуратора есть отдельный набор проверок на jsdom (только для разработки -- у самого прокси зависимостей нет):

```bash
npm install jsdom
node tests/conf3.0-ipv6.test.js
node tests/conf3.0-dns.test.js
node tests/conf3.0-ports.test.js
node tests/conf3.1.test.js
```

### Генератор команд из командной строки

`tools/awg-gen.js` печатает те же команды RouterOS, что и конфигуратор, только без браузера. Он открывает `docs/conf3.0.html` в jsdom, заполняет форму и нажимает «Сгенерировать», так что логика генерации остаётся одна — страница, и вывод совпадает с ней байт в байт. Работает локально и офлайн: сетевые вызовы страницы отключены, приватный ключ не покидает машину. Годится и человеку, и ИИ-агенту на машине пользователя. Нужен Node.js 20+.

```bash
npm install jsdom                        # один раз, в корне репозитория
node tools/awg-gen.js amnezia.conf > awg.rsc
node tools/awg-gen.js --scenario non-ru --exclude steam,epic amnezia.conf
node tools/awg-gen.js --part routing --services youtube,telegram amnezia.conf
node tools/awg-gen.js --mode site-to-site --endpoint 198.51.100.1 --part side-b
node tools/awg-gen.js --help             # все опции, сервисы и накопители
```

Конфиг читается из файла или из stdin. Скрипт выходит в stdout частями (`# >>> [install]`, `# >>> [routing]`, …), их выполняют по очереди, каждую после завершения предыдущей; `--part` печатает одну часть. Заметки и ошибки страницы идут в stderr. Код выхода 1 — страница не приняла конфиг, 2 — ошибка в опциях.

## Лицензия

MIT -- см. [LICENSE](LICENSE).
