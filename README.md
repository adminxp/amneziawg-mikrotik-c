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
- [Дополнительные настройки](#дополнительные-настройки)
- [Удаление](#удаление)
- [Устранение неполадок](#устранение-неполадок)
  - [Storage device not found](#storage-device-not-found)
  - [Insufficient disk space](#insufficient-disk-space)
  - [not allowed by device-mode](#not-allowed-by-device-mode)
  - [child spawn failed / could not load next layer](#child-spawn-failed--could-not-load-next-layer)
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
Довольно сложный режим, все конфиги и дальнейшие пиры генерируйте самостоятельно. Режим ещё не обкатан на массе, сообщайте об ошибках.   

Совместим с AWG v1 и v2 -- версия определяется автоматически по переменным окружения.

## Быстрый старт (конфигуратор)

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
- Архитектура: ARM64, ARM (v7) или x86_64 ([проверить устройство](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container))
- Минимум 5 МБ свободного места на диске (или USB-накопитель)
- Минимум 16 МБ свободной оперативной памяти (RAM)

## Ручная установка

### 1. Включение контейнеров

Установите пакет container с [mikrotik.com](https://mikrotik.com/download), загрузите на роутер и перезагрузитесь. Затем:

```routeros
/system/device-mode/update container=yes
```

Роутер попросит подтверждение (кнопка или перезагрузка, зависит от модели).

### 2. Загрузка образа

Скачайте `awg-proxy-{arch}.tar.gz` со страницы [Releases](https://github.com/timbrs/amneziawg-mikrotik-c/releases) и загрузите на роутер через Winbox или SCP. Для RouterOS 7.20 и ниже используйте файлы с суффиксом `-7.20-Docker` (Docker-формат).

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
| `AWG_CLIENT_PUB` | Да | -- | Публичный ключ клиента |
| `AWG_S3` | Нет | `0` | Паддинг cookie reply (v2) |
| `AWG_S4` | Нет | `0` | Паддинг transport data (v2) |
| `AWG_I1`--`AWG_I5` | Нет | -- | CPS-шаблоны (v1.5/v2) |
| `AWG_MODE` | Нет | `normal` | Режим работы: `normal`, `reverse`, `server` |
| `AWG_SRC_PORT` | Нет | auto | Исходящий порт к серверу |
| `AWG_TIMEOUT` | Нет | `180` | Таймаут бездействия (сек) |
| `AWG_LOG_LEVEL` | Нет | `info` | Уровень логирования |
| `AWG_NO_GRO` | Нет | `0` | Отключить UDP GRO |
| `AWG_SOCKET_BUF` | Нет | `16777216` | Размер буфера сокета |
| `AWG_CPU_C2S` | Нет | `-1` | CPU для потока client→server |
| `AWG_CPU_S2C` | Нет | `-1` | CPU для потока server→client |
| `AWG_BUSY_POLL` | Нет | `0` | SO_BUSY_POLL таймаут (мкс) |

Версия протокола определяется автоматически: **v2** если заданы S3/S4 или H в виде диапазонов, **v1.5** если заданы CPS-шаблоны (I1-I5), иначе **v1**.

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
/container/config set tmpdir=usb1/pull ram-high=200M
```

Замените `usb1` на mount-point вашего накопителя (см. `/disk/print`).

После установки контейнера можно вернуть значение обратно:

```routeros
/container/config set tmpdir="" ram-high=0
```

Если используете конфигуратор -- выберите нужный накопитель в поле "Container storage", и tmpdir будет настроен автоматически.

### not allowed by device-mode

Если при загрузке образа или создании контейнера появляется ошибка `not allowed by device-mode`, значит контейнеры не активированы. Выполните:

```routeros
/system/device-mode/update container=yes
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
