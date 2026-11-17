# bfban: что это, зачем и какие преимущества

[English](README.md) | [Русский](README.ru.md)

## Что это

`bfban` — небольшой Linux-компонент для управления динамическим списком
заблокированных IP-адресов через `bpfilter` и eBPF.

`bfban` не анализирует логи и не принимает решение о том, кто является
атакующим. За это отвечают Fail2ban, CrowdSec или собственный security-agent.
`bfban` является быстрым слоем исполнения уже принятого решения:

```text
Fail2ban / CrowdSec / свой агент
                │
                ▼
        bfban daemon + Unix socket
                │
                ▼
        bpfilter sets / maps
                │
                ▼
          eBPF DROP на Linux
```

В текущем MVP `bfban` управляет одной bpfilter-chain через установленный
`bfcli`. Дополнительный демон `bfband` держит Unix-сокет, TTL и состояние
банов. Если `libbpfilter` найден через `pkg-config`, daemon обновляет sets
через прямой C API; иначе используется fallback через `bfban`/`bfcli`.

Исходный движок находится в [libbpfilter](https://bpfilter.io/developers/modules/libbpfilter/libbpfilter.html),
а локальные публичные заголовки — в
[`src/libbpfilter/include/bpfilter`](https://github.com/facebook/bpfilter/tree/main/src/libbpfilter/include/bpfilter).
Режим выбирается через `BFBAN_DIRECT=auto|yes|no`, а путь к bpffs — через
`BFBAN_BPFFS`.

## CIDR — полноценная поддержка сетей

CIDR — это рабочий формат ввода, а не только пример в документации:

```sh
sudo bfban add 203.0.113.10       # один IPv4-адрес
sudo bfban add 203.0.113.0/24     # IPv4-префикс
sudo bfban add 2001:db8::1        # один IPv6-адрес
sudo bfban add 2001:db8::/32      # IPv6-префикс
sudo bfban list
```

При `bfban init` создаются четыре независимых набора:

| Ввод | bpfilter set | Matcher |
|---|---|---|
| IPv4-адрес | `banlist4` | `ip4.saddr` |
| IPv4 CIDR | `banlist4net` | `ip4.snet` |
| IPv6-адрес | `banlist6` | `ip6.saddr` |
| IPv6 CIDR | `banlist6net` | `ip6.snet` |

Точные адреса и префиксы разделены: сеть не разворачивается в тысячи
отдельных IP, а проверяется через prefix/LPM set bpfilter. CIDR сохраняется в
state-файле и восстанавливается `bfband` после перезапуска.

## Конфигурация и установка

Для этого небольшого проекта отдельный Autotools/CMake не нужен: есть POSIX
`configure` и обычный Makefile.

```sh
./configure --prefix=/usr/local
make
sudo make install
sudo systemctl daemon-reload
sudo systemctl enable --now bfband
```

Основные параметры находятся в `/etc/bfban/bfban.env`: путь к `bfcli`,
Unix-сокету, state-файлу, chain и hook.

## Пошаговая методика применения

### 1. Проверить хост

Проверяйте фактически запущенное ядро, `libbpf` и доступные интерфейсы:

```sh
uname -r
pkg-config --modversion libbpf
mountpoint /sys/fs/bpf
sudo bfobserve --list-interfaces
```

Текущая целевая матрица — Linux 6.6+, `libbpf` 1.2+ и совместимый с вашим
ядром bpfilter hook. Примеры подходящих базовых ядер: Ubuntu 24.04 (6.8),
Debian 13 (6.12), Fedora 40 (6.8), RHEL/Rocky/Alma 10 (6.12), Ubuntu 22.04 с
HWE 6.8. RHEL/Rocky/Alma 9 (5.14) и Debian 12 (6.1) требуют нового
поддерживаемого ядра или backport.

### 2. Собрать и установить

```sh
cd ~/bfban
./configure --prefix=/usr/local
make
sudo make install
```

Ожидается, что `bfcli` находится в `/usr/local/sbin/bfcli`. Другой путь нужно
указать через `BFBAN_BPFCLI` в `/etc/bfban/bfban.env`. После установки
libbpfilter повторно запустите `./configure`: он обнаружит pkg-config-модуль
и включит direct C API для daemon. Это убирает fork/exec на каждый ban/unban,
но само по себе не доказывает ускорение packet path.

### 3. Запустить systemd-демон

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now bfband.service
sudo systemctl status bfband.service
sudo journalctl -u bfband.service -b --no-pager
sudo bfbandctl ping
```

Unit создаёт `/run/bfban` для сокета и `/var/lib/bfban` для состояния. Сейчас
демон запускается от `root`, поскольку backend загружает и изменяет BPF
объекты. Не включайте вслепую `ProtectSystem=strict` и не удаляйте
`CAP_BPF`/`CAP_NET_ADMIN`: это может сломать загрузку или обновление объектов.
Параметры systemd описаны в [systemd.exec](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec).

### 4. Создать и проверить chain

```sh
sudo bfbandctl init
sudo bfban add 203.0.113.10
sudo bfban list
sudo bfcli chain get --name bfban_input
sudo bfban remove 203.0.113.10
```

Для сухого теста используйте документационные сети `203.0.113.0/24` и
`2001:db8::/32`. На рабочем сервере держите консольный доступ или вторую SSH
сессию. `bfban flush` очищает динамическую chain.

### 5. Подключить Fail2ban

Создайте `/etc/fail2ban/jail.d/sshd-bfban.local`:

```ini
[sshd-bfban]
enabled = true
filter = sshd
backend = systemd
port = ssh
maxretry = 5
findtime = 10m
bantime = 1h
banaction = bfban
```

Проверьте конфигурацию:

```sh
sudo fail2ban-client reload
sudo fail2ban-client status sshd-bfban
sudo bfbandctl list
sudo journalctl -u fail2ban -u bfband -f
```

Fail2ban отвечает за фильтры логов, пороги, allowlist и срок бана; `bfban`
только применяет принятое решение. В MVP chain и state-файл глобальные, поэтому
несколько jail могут мешать друг другу через `actionstop`.

### 6. Определить источник нагрузки

```sh
ip -s link
sar -n DEV 1
ethtool -S eno1
ss -s
conntrack -S
nstat -az
sudo bfobserve --interface eno1 --seconds 30 --top 20
sudo bfobserve --interface eno1 --seconds 30 --suggest --min-pps 1000
```

`bfobserve` ничего не блокирует: он показывает IP-источники, PPS, байты,
SYN/RST, UDP и последний порт назначения. Команда `--suggest` только печатает
варианты `bfban add`. Перед блокировкой проверьте allowlist, NAT/CDN, порт,
протокол и независимый threat feed.

### 7. Применить CIDR/feed и уметь откатить

```sh
sudo bfban add 203.0.113.0/24
./scripts/fetch-threat-feed.sh --output=/tmp/feodo.txt
less /tmp/feodo.txt
sudo ./scripts/fetch-threat-feed.sh --apply
sudo bfban remove 203.0.113.10
sudo bfbandctl flush
```

Не блокируйте географию или ASN как замену IOC. Для volumetric DDoS, который
заполняет канал, нужен фильтр у провайдера или scrubbing: локальный host уже
не может вернуть пропускную способность канала.

## Как понять, откуда идёт DDoS

`bfban` сам по себе **не обнаруживает DDoS**. Он получает уже принятое решение
о блокировке. Источник атаки сначала нужно определить telemetry-инструментами.

Быстрая последовательность:

```sh
# нагрузка на интерфейсы и ошибки
ip -s link
sar -n DEV 1
ethtool -S eth0

# состояние TCP/UDP и conntrack
ss -s
conntrack -S
nstat -az
```

Для top-N IPv4-источников на обычном ingress можно использовать `tshark`:

```sh
sudo tshark -ni eth0 -a duration:30 -f 'ip' \
  -T fields -e ip.src 2>/dev/null |
  sort | uniq -c | sort -nr | head -50
```

Затем нужно проверить top-источники по независимому признаку: порт назначения,
TCP flags, долю новых соединений, rate и принадлежность к известному feed.
Автоматически банить любой самый частый IP опасно: это может быть NAT,
прокси, CDN или легитимный клиент.

### Ограничение наблюдения при XDP

Если пакет отбрасывается в native XDP, он может не дойти до обычного сетевого
стека и не попасть в `tcpdump`. Поэтому для расследования нужно либо временно
наблюдать трафик до включения XDP, либо иметь отдельную telemetry map/ringbuf,
которая экспортирует source IP, protocol, port и counters. В текущей версии
`bfban` такой per-source telemetry нет: chain counters показывают количество
совпадений, но не top-N источников.

Для production-варианта в дальнейшем нужен отдельный eBPF-based `bfobserve`:

```text
NIC → XDP observe map → userspace top-N → operator decision → bfban blocklist
```

Это разделяет detection и enforcement и снижает риск самоблокировки.

## Wow-фича: `bfobserve`

`bfobserve` — read-only режим расследования: он не блокирует адреса, а
собирает top-N источников за короткое окно и превращает результат в понятные
следующие действия. Текущий MVP использует AF_PACKET и требует `root` или
`CAP_NET_RAW`; это userspace-наблюдатель, а не XDP telemetry map.

Сначала посмотреть интерфейсы:

```sh
sudo bfobserve --list-interfaces
```

Пример вывода:

```text
Available interfaces:
  IFACE            STATE       MTU     COMMAND
  lo               up/running  65536   sudo bfobserve -i lo
  eth0             up/running  1500    sudo bfobserve -i eth0
  wg0              up           1420   sudo bfobserve -i wg0
```

Затем запустить наблюдение на WAN-интерфейсе:

```sh
sudo bfobserve --interface eth0 --seconds 30 --top 20
```

Отчёт показывает:

```text
#    SOURCE                                  PACKETS        BYTES        PPS        SYN        UDP    DPORT
1    198.51.100.20                             82000      61.2 MB       2733      81000          0       22
2    203.0.113.44                              47000      28.4 MB       1566          0      46000      443
```

Для automation/SIEM:

```sh
sudo bfobserve --interface eth0 --seconds 30 --top 100 --json
```

Для осторожных рекомендаций:

```sh
sudo bfobserve --interface eth0 --seconds 30 \
  --suggest --min-pps 1000
```

Команда только напечатает предложения вида:

```text
bfban add 198.51.100.20  # 2733 pps, 81000 SYN, 0 UDP
```

Администратор проверяет источник, allowlist и threat feed, после чего сам
решает, выполнять ли `bfban add`. Это сделано намеренно: автоматический ban
по одному только top-N может заблокировать NAT, CDN или легитимный клиент.

## Требования к ядру и дистрибутивам

Требование задаётся не самим `bfban`, а текущим bpfilter:

| Компонент | Требование |
|---|---|
| Linux kernel | **6.6 или новее** |
| libbpf | **1.2 или новее** |
| BPF token mode | **6.9 или новее** |
| userspace | `clang`, `libbpf`, `bpftool`/BPF filesystem по конфигурации системы |

Проверять нужно фактическое запущенное ядро, а не только версию дистрибутива:

```sh
uname -r
pkg-config --modversion libbpf
mountpoint /sys/fs/bpf
```

### Дистрибутивы, где базовое ядро подходит

| Дистрибутив | Базовая версия ядра | Статус |
|---|---:|---|
| Ubuntu 24.04 LTS | 6.8 | подходит |
| Ubuntu 22.04 LTS с HWE | 6.8 | подходит только с HWE |
| Debian 13 “trixie” | 6.12 | подходит |
| Fedora 40 | 6.8 | подходит |
| Fedora 41+ | новее 6.8 | подходит |
| RHEL 10 | 6.12 | подходит |
| Rocky Linux 10 | 6.12 | подходит |
| AlmaLinux 10 | 6.12 | подходит |
| RHEL 9 / CentOS Stream 9 | 5.14 | не подходит без отдельного нового kernel |
| Rocky Linux 9 | 5.14 | не подходит без отдельного нового kernel |
| AlmaLinux 9 | 5.14 | не подходит без отдельного нового kernel |
| Debian 12 “bookworm” | 6.1 | не подходит без backports/new kernel |

Это минимальная матрица по версии kernel. Дополнительно должны совпадать
архитектура, конфигурация BPF и версия `libbpf`. Fedora публикует актуальные
версии kernel в своём package index, Ubuntu 24.04 поставляется с веткой 6.8,
Debian 13 — с веткой 6.12, а RHEL 10 — с kernel 6.12. Rocky 10 и AlmaLinux
10 также поставляются с веткой 6.12, тогда как Rocky/Alma 9 остаются на 5.14:
[Fedora kernel
packages](https://packages.fedoraproject.org/pkgs/kernel/kernel/index.html),
[Ubuntu 24.04 kernel](https://discourse.ubuntu.com/t/introducing-kernel-6-8-for-the-24-04-noble-numbat-release/41958),
[Debian 13 release notes](https://www.debian.org/releases/stable/release-notes.en.pdf),
[RHEL release/kernel table](https://api.access.redhat.com/articles/red-hat-enterprise-linux-release-dates),
[Rocky releases](https://docs.rockylinux.org/releases/),
[AlmaLinux release notes](https://wiki.almalinux.org/release-notes/).

### Почему именно eBPF/XDP

Научное и инженерное обоснование не в том, что любой eBPF-код быстрее
iptables. Выигрыш возникает в конкретной точке обработки и при конкретном
профиле нагрузки:

1. XDP вызывается на раннем пути приёма пакета и может вернуть `XDP_DROP` до
   обычной обработки сетевым стеком.
2. bpfilter компилирует набор условий в одну проверяемую kernel-программу,
   а большие blocklist хранит в BPF maps/sets.
3. Динамический ban меняет данные map/set, а не добавляет новую firewall-rule
   в линейную цепочку.

Это делает решение перспективным для раннего L3/L4 drop, больших blocklist и
высокого packet rate. Это не доказывает преимущество над современным
nftables: nftables тоже поддерживает sets, dynamic sets, counters и
per-source rate limiting. Поэтому обязательна контрольная серия измерений:

```text
legacy iptables  vs  nftables  vs  bpfilter Netfilter  vs  bpfilter XDP
```

Сравнивать нужно одинаковые policy и workload: размер blocklist, packets/sec,
accept/drop ratio, CPU, latency, IPv4/IPv6 и наличие counters/logging.

Первичные источники описывают раннее выполнение XDP и live-frame режим в
[Linux kernel BPF documentation](https://docs.kernel.org/bpf/bpf_prog_run.html),
а требования и поддержка конкретных hooks должны проверяться по kernel API,
который использует сам bpfilter.

## Зачем это нужно

Типичная схема Fail2ban добавляет или удаляет отдельные правила firewall при
каждом бане. При большом количестве адресов это приводит к длинным цепочкам,
частым изменениям ruleset и дополнительной работе control plane.

`bfban` держит постоянные наборы адресов:

```text
одна eBPF chain
наборы banlist4/banlist6 и banlist4net/banlist6net
много IP-адресов и CIDR-сетей внутри этих sets
```

Добавление или удаление адреса меняет содержимое set, а не структуру всей
программы. В bpfilter для этого уже есть операция `chain update-set`.

Fail2ban предоставляет action-команды `actionstart`, `actionban` и
`actionunban`; именно через такой action `bfban` интегрируется с Fail2ban.
См. пример в [etc/fail2ban/action.d/bfban.conf](etc/fail2ban/action.d/bfban.conf)
и официальные actions Fail2ban: <https://github.com/fail2ban/fail2ban/tree/master/config/action.d>.

## Как это интегрируется с Fail2ban

Fail2ban и `bfban` выполняют разные роли:

```text
лог sshd/nginx/etc.
        │
        ▼
Fail2ban filter
        │ считает maxretry за findtime
        ▼
Fail2ban jail принимает решение о бане
        │
        ├── actionstart  → создать bpfilter chain и banlist
        ├── actionban    → добавить IP в banlist
        ├── actionunban  → удалить IP из banlist
        └── actionstop   → удалить chain
```

`bfban` не читает логи и не знает, почему адрес был заблокирован. Он получает
от Fail2ban только команды. Например, при пяти неудачных SSH-входах за десять
минут Fail2ban вызывает:

```sh
bfban add 203.0.113.10
```

Когда заканчивается `bantime`, Fail2ban вызывает:

```sh
bfban remove 203.0.113.10
```

В текущем MVP эти команды вызывают `bfcli chain update-set`. Поэтому eBPF
программа остаётся загруженной, а изменяется только содержимое соответствующего
named set.
Fail2ban уже управляет временем жизни бана; отдельный TTL в MVP не нужен.

### Установка action

После сборки установить бинарник и action:

```sh
cd ~/bfban
make
sudo make install
```

Проверить, что доступны:

```text
/usr/local/sbin/bfban
/etc/fail2ban/action.d/bfban.conf
```

### Пример jail для SSH

Создать `/etc/fail2ban/jail.d/sshd-bfban.local`:

```ini
[sshd-bfban]
enabled = true
filter = sshd
backend = systemd
port = ssh
maxretry = 5
findtime = 10m
bantime = 1h
banaction = bfban
```

Затем перезапустить Fail2ban:

```sh
sudo systemctl restart fail2ban
sudo fail2ban-client status sshd-bfban
```

При старте jail action выполнит `bfban init`. При бане и разбане IP будут
вызваны `actionban` и `actionunban` из
`/etc/fail2ban/action.d/bfban.conf`.

### Важное ограничение текущего MVP

Сейчас используется одна глобальная chain `bfban_input` и один state-файл.
Поэтому безопасный вариант — один jail, например SSH. Если подключить
несколько jail одновременно, они будут использовать общий список, а
`actionstop` одного jail может очистить баны другого.

Текущий `bfband` всё ещё использует одну глобальную chain; namespace по имени
jail будет добавлен позже. Поэтому до появления namespaces следует либо
использовать один общий jail/action, либо не включать `actionstop` на
отдельных jail.

### Проверка работы

Состояние Fail2ban:

```sh
sudo fail2ban-client status sshd-bfban
```

Список адресов, переданных в bpfilter:

```sh
sudo bfban list
```

Состояние bpfilter-chain:

```sh
sudo bfcli chain get --name bfban_input
```

В counters chain можно увидеть, сколько пакетов было отброшено. Для
диагностики сначала рекомендуется использовать `bantime = 10m` и проверять
chain вручную, а уже затем включать длительные баны.

## Какие преимущества

### Быстрый lookup

Адрес проверяется внутри скомпилированной eBPF-программы и соответствующего
named set. Это
особенно интересно для больших blocklist и частых обновлений.

### Раннее отбрасывание

В режиме XDP пакет можно отбросить на самом раннем этапе приёма сетевой
карты — до обычной обработки сетевым стеком. Такой режим полезен для
высокого PPS и простых L3/L4 blocklist. Для XDP нужны совместимые драйвер и
режим подключения; XDP не заменяет NAT и conntrack.

### Атомарное обновление

Обновление chain не должно оставлять окно без фильтрации. Для массовых
изменений используется обновление set, а для изменения программы — атомарный
`chain update`.

### Меньше конфликтов с основным firewall

`bfban` может работать отдельным слоем рядом с iptables/nftables. Основной
firewall продолжает отвечать за NAT, conntrack и постоянную политику, а
`bfban` — за динамический deny-list.

### Фильтрация по cgroup

Сам bpfilter также умеет cgroup hooks. Это позволяет в будущем применять
политику не только к интерфейсу, но и к группе процессов.

## Что это не делает

`bfban` не является полной защитой от volumetric DDoS. Если канал уже забит,
локальный Linux-host не сможет устранить проблему — нужен upstream-фильтр или
scrubbing у провайдера.

`bfban` также не заменяет полноценный stateful firewall. NAT, conntrack,
маршрутизация и постоянные правила должны оставаться в nftables/iptables или
другом основном firewall.

Для rate limiting per source потребуется отдельная реализация token bucket и
состояния в BPF maps. В текущем MVP этого нет.

## Память

Память состоит из трёх частей:

1. `bfband` держит таблицу адресов и TTL в userspace. При текущем лимите
   `65536` записей это примерно **4.5 MiB** зарезервированной памяти, плюс
   обычный runtime процесса.
2. `bfcli`/`libbpfilter` временно используют память при загрузке или обновлении
   chain.
3. Сам BPF map живёт в kernel memory. Для точной оценки нужно смотреть
   `bpftool map show` и `bpftool map dump`; расход зависит от типа map, размера
   ключа, числа элементов и kernel allocator overhead.

Грубая инженерная оценка для blocklist — десятки байт kernel memory на IPv4
адрес и больше для IPv6/LPM. То есть `100000` адресов обычно означают единицы
мегабайт, а не сотни мегабайт, но это нужно подтвердить benchmark’ом на
целевом ядре. В production нужно заменить фиксированный массив daemon на
bounded heap-backed storage и добавить лимит/метрики.

## CIDR и сети

Без CIDR полноценный threat-feed importer неполон. Важно, что bpfilter уже
поддерживает `ip4.snet`, `ip6.snet` и LPM-trie sets. Поэтому для blocklist
нужно разделять четыре набора:

```text
banlist4      — точные IPv4
banlist4net   — IPv4 CIDR
banlist6      — точные IPv6
banlist6net   — IPv6 CIDR
```

`bfban` теперь принимает как `203.0.113.10`, так и `203.0.113.0/24`, а при
инициализации chain создаёт соответствующие exact/LPM sets. Spamhaus DROP можно
будет подключить после добавления CIDR-aware feed importer; текущий пример
Feodo Tracker содержит IPv4-адреса и работает уже сейчас.

## Тестирование на threat-intelligence feeds

Не следует блокировать «китайские сети» только по географии или ASN: это не
признак ботнета и даст много false positives. Для воспроизводимого теста
нужны botnet-specific IOC feeds. Скрипт
[`scripts/fetch-threat-feed.sh`](scripts/fetch-threat-feed.sh) по умолчанию
использует рекомендованный Feodo Tracker C2 feed, скачивает и валидирует IPv4,
но ничего не блокирует без явного `--apply`.

```sh
./scripts/fetch-threat-feed.sh --output=/tmp/feodo.txt
less /tmp/feodo.txt
sudo ./scripts/fetch-threat-feed.sh --apply
```

Feodo Tracker описывает адреса как подтверждённые botnet C2 и предупреждает,
что aggressive historical feed имеет более высокий риск false positives:
<https://feodotracker.abuse.ch/blocklist/>.
Spamhaus DROP можно использовать для отдельного CIDR-aware importer, но его
обновление должно быть автоматическим и не чаще разрешённой частоты:
<https://www.spamhaus.org/faqs/do-not-route-or-peer-drop/>.

Эти feeds предназначены для defensive testing и blocklist integration, а не
для активного сканирования или атак на перечисленные адреса.

## Демон `bfband`

Демон `bfband`:

- слушать Unix socket `/run/bfban/bfban.sock`;
- принимать `ADD`, `DEL`, `LIST`, `FLUSH`;
- поддерживать TTL для банов;
- восстанавливать состояние после перезапуска;
- сериализовать изменения set;
- передавать команды через один Unix-сокет;
- ограничивать доступ Unix-пермишенами и root/group policy.

Текущий текстовый API:

```text
ADD 203.0.113.10 3600
DEL 203.0.113.10
LIST
FLUSH
```

Fail2ban вызывает только тонкий action-клиент `bfbandctl`, а не управляет
eBPF objects самостоятельно.

Запуск после установки:

```sh
sudo systemctl enable --now bfband
sudo bfbandctl ping
```

В текущей реализации daemon при каждом изменении вызывает `bfban`, поэтому
это уже рабочий Unix-socket слой, но ещё не оптимальный прямой FFI-доступ к
`libbpfilter`.

## Вау-фича: понятная диагностика источника атаки

`bfobserve` — наблюдатель трафика для ответа на практический вопрос «откуда
идёт нагрузка?». Он за короткое окно показывает top-N источников, PPS, байты,
SYN, UDP и последний destination port, а `--suggest` печатает готовые команды
для передачи подозрительных адресов в `bfban`.

Сначала администратор выбирает интерфейс понятным списком:

```sh
sudo bfobserve --list-interfaces
sudo bfobserve --interface eth0 --seconds 30 --top 20
sudo bfobserve --interface eth0 --seconds 30 --top 20 --suggest --min-pps 100
sudo bfobserve --interface eth0 --seconds 30 --json
```

Пример вывода интерфейсов:

```text
Available interfaces:
  IFACE            STATE       MTU     COMMAND
  lo               up/running  65536   sudo bfobserve -i lo
  eth0             up/running  1500    sudo bfobserve -i eth0
```

Это именно диагностический слой: он не блокирует адрес автоматически и не
выдаёт «ботнет» по одному признаку. Администратор видит источник, проверяет
порт/протокол и затем применяет адрес через `bfban`, Fail2ban или отдельную
политику. Нужны `root` или `CAP_NET_RAW`; текущая версия использует безопасный
AF_PACKET capture, поэтому это не обещание, что вся volumetric-атака будет
видна после аппаратного offload или upstream-фильтрации.

## Когда это имеет смысл

Проект имеет смысл как:

- backend для Fail2ban/CrowdSec;
- быстрый динамический blocklist для Linux edge-host;
- XDP/TC deny-layer для большого PPS;
- специализированная фильтрация, которую неудобно выражать обычными
  firewall-правилами.

Для небольшого сервера с несколькими десятками правил обычный nftables
обычно проще. Современный nftables также поддерживает sets, timeouts и
per-source rate limiting, поэтому преимущество `bfban` нужно подтверждать
измерениями, а не предполагать заранее.

## Лицензия

Проект распространяется под GPL-2.0-only. Производные распространяемые
программы, которые используют код `libbpfilter`, должны соблюдать условия
GPLv2. Интеграционные конфигурации и отдельные процессы могут иметь свои
лицензии, если не включают GPL-код, но составной дистрибутив всё равно должен
содержать лицензионные уведомления и исходники GPL-компонентов.
