/* SPDX-License-Identifier: GPL-2.0-only */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#define SOURCE_SLOTS 32768
#define PACKET_BUFFER 65536

struct source_stats {
    bool used;
    char address[INET6_ADDRSTRLEN];
    uint64_t packets;
    uint64_t bytes;
    uint64_t syn;
    uint64_t rst;
    uint64_t udp;
    uint16_t last_dport;
};

struct observer {
    struct source_stats sources[SOURCE_SLOTS];
    uint64_t packets;
    uint64_t bytes;
    uint64_t malformed;
};

enum capture_direction { DIRECTION_INGRESS, DIRECTION_EGRESS, DIRECTION_BOTH };

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: bfobserve [OPTIONS]\n"
            "  -i, --interface IFACE  observe one interface\n"
            "  -l, --list-interfaces list usable interfaces and exit\n"
            "  -s, --seconds N        observation window (default: 30)\n"
            "  -t, --top N            print top N sources (default: 20)\n"
            "      --direction MODE   ingress, egress or both (default: ingress)\n"
            "      --json             print machine-readable JSON\n"
            "      --suggest          print bfban commands for high-rate sources\n"
            "      --min-pps N         threshold for --suggest (default: 100)\n"
            "  -h, --help             show this help\n");
}

static int list_interfaces(void)
{
    struct if_nameindex *interfaces = if_nameindex();
    if (!interfaces) { perror("bfobserve: if_nameindex"); return 1; }
    puts("Available interfaces:");
    puts("  IFACE            STATE       MTU     COMMAND");
    for (struct if_nameindex *item = interfaces; item->if_index; ++item) {
        int control = socket(AF_INET, SOCK_DGRAM, 0);
        char state[16] = "unknown";
        int mtu = 0;
        if (control >= 0) {
            struct ifreq request = {0};
            snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", item->if_name);
            if (ioctl(control, SIOCGIFFLAGS, &request) == 0) {
                if (!(request.ifr_flags & IFF_UP)) snprintf(state, sizeof(state), "down");
                else if (request.ifr_flags & IFF_RUNNING) snprintf(state, sizeof(state), "up/running");
                else snprintf(state, sizeof(state), "up");
            }
            if (ioctl(control, SIOCGIFMTU, &request) == 0) mtu = request.ifr_mtu;
            close(control);
        }
        printf("  %-16s %-11s %-7d sudo bfobserve -i %s\n",
               item->if_name, state, mtu, item->if_name);
    }
    if_freenameindex(interfaces);
    return 0;
}

static uint64_t hash_string(const char *string)
{
    uint64_t hash = 1469598103934665603ULL;
    while (*string) {
        hash ^= (unsigned char)*string++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static struct source_stats *get_source(struct observer *observer,
                                       const char *address)
{
    size_t start = hash_string(address) % SOURCE_SLOTS;
    for (size_t i = 0; i < SOURCE_SLOTS; ++i) {
        struct source_stats *source =
            &observer->sources[(start + i) % SOURCE_SLOTS];
        if (!source->used) {
            source->used = true;
            snprintf(source->address, sizeof(source->address), "%s", address);
            return source;
        }
        if (strcmp(source->address, address) == 0) return source;
    }
    return NULL;
}

static void account_packet(struct observer *observer, const char *address,
                           size_t length, uint8_t protocol, uint16_t dport,
                           uint8_t tcp_flags)
{
    struct source_stats *source = get_source(observer, address);
    if (!source) return;
    ++source->packets;
    source->bytes += length;
    source->last_dport = dport;
    if (protocol == IPPROTO_UDP) ++source->udp;
    if (protocol == IPPROTO_TCP) {
        if ((tcp_flags & TH_SYN) && !(tcp_flags & TH_ACK)) ++source->syn;
        if (tcp_flags & TH_RST) ++source->rst;
    }
}

static size_t skip_vlan(const unsigned char *packet, size_t length,
                        uint16_t *protocol)
{
    size_t offset = ETH_HLEN;
    if (length < offset) return 0;
    memcpy(protocol, packet + 12, sizeof(*protocol));
    *protocol = ntohs(*protocol);
    while (*protocol == ETH_P_8021Q || *protocol == ETH_P_8021AD) {
        if (length < offset + 4) return 0;
        memcpy(protocol, packet + offset + 2, sizeof(*protocol));
        *protocol = ntohs(*protocol);
        offset += 4;
    }
    return offset;
}

static void process_packet(struct observer *observer,
                           const unsigned char *packet, size_t length)
{
    uint16_t ether_type;
    size_t offset = skip_vlan(packet, length, &ether_type);
    if (!offset) { ++observer->malformed; return; }

    char address[INET6_ADDRSTRLEN];
    uint8_t protocol = 0;
    uint16_t dport = 0;
    uint8_t tcp_flags = 0;

    if (ether_type == ETH_P_IP) {
        if (length < offset + sizeof(struct ip)) { ++observer->malformed; return; }
        struct ip header_storage;
        memcpy(&header_storage, packet + offset, sizeof(header_storage));
        const struct ip *header = &header_storage;
        size_t header_length = (size_t)header->ip_hl * 4;
        if (header_length < sizeof(struct ip) || length < offset + header_length) {
            ++observer->malformed; return;
        }
        if (!inet_ntop(AF_INET, &header->ip_src, address, sizeof(address))) return;
        protocol = header->ip_p;
        const unsigned char *transport = packet + offset + header_length;
        size_t available = length - offset - header_length;
        if (protocol == IPPROTO_TCP && available >= sizeof(struct tcphdr)) {
            struct tcphdr tcp_storage;
            memcpy(&tcp_storage, transport, sizeof(tcp_storage));
            const struct tcphdr *tcp = &tcp_storage;
            dport = ntohs(tcp->th_dport); tcp_flags = tcp->th_flags;
        } else if (protocol == IPPROTO_UDP && available >= sizeof(struct udphdr)) {
            struct udphdr udp_storage;
            memcpy(&udp_storage, transport, sizeof(udp_storage));
            dport = ntohs(udp_storage.uh_dport);
        }
    } else if (ether_type == ETH_P_IPV6) {
        if (length < offset + sizeof(struct ip6_hdr)) { ++observer->malformed; return; }
        struct ip6_hdr header_storage;
        memcpy(&header_storage, packet + offset, sizeof(header_storage));
        const struct ip6_hdr *header = &header_storage;
        if (!inet_ntop(AF_INET6, &header->ip6_src, address, sizeof(address))) return;
        protocol = header->ip6_nxt;
        size_t transport_offset = offset + sizeof(struct ip6_hdr);
        size_t available = length - transport_offset;
        for (unsigned int extensions = 0; extensions < 8; ++extensions) {
            size_t extension_length;
            if (protocol == IPPROTO_HOPOPTS || protocol == IPPROTO_ROUTING ||
                protocol == IPPROTO_DSTOPTS) {
                if (available < 8) { ++observer->malformed; return; }
                uint8_t next, units;
                memcpy(&next, packet + transport_offset, 1);
                memcpy(&units, packet + transport_offset + 1, 1);
                extension_length = ((size_t)units + 1) * 8;
                protocol = next;
            } else if (protocol == IPPROTO_FRAGMENT) {
                if (available < 8) { ++observer->malformed; return; }
                memcpy(&protocol, packet + transport_offset, 1);
                extension_length = 8;
            } else if (protocol == IPPROTO_AH) {
                if (available < 2) { ++observer->malformed; return; }
                uint8_t next, units;
                memcpy(&next, packet + transport_offset, 1);
                memcpy(&units, packet + transport_offset + 1, 1);
                extension_length = ((size_t)units + 2) * 4;
                protocol = next;
            } else break;
            if (extension_length > available) { ++observer->malformed; return; }
            transport_offset += extension_length;
            available -= extension_length;
        }
        const unsigned char *transport = packet + transport_offset;
        if (protocol == IPPROTO_TCP && available >= sizeof(struct tcphdr)) {
            struct tcphdr tcp_storage;
            memcpy(&tcp_storage, transport, sizeof(tcp_storage));
            const struct tcphdr *tcp = &tcp_storage;
            dport = ntohs(tcp->th_dport); tcp_flags = tcp->th_flags;
        } else if (protocol == IPPROTO_UDP && available >= sizeof(struct udphdr)) {
            struct udphdr udp_storage;
            memcpy(&udp_storage, transport, sizeof(udp_storage));
            dport = ntohs(udp_storage.uh_dport);
        }
    } else {
        return;
    }
    ++observer->packets;
    observer->bytes += length;
    account_packet(observer, address, length, protocol, dport, tcp_flags);
}

static int compare_sources(const void *left, const void *right)
{
    const struct source_stats *a = left, *b = right;
    if (a->packets < b->packets) return 1;
    if (a->packets > b->packets) return -1;
    return strcmp(a->address, b->address);
}

static size_t collect_sources(const struct observer *observer,
                              struct source_stats *output)
{
    size_t count = 0;
    for (size_t i = 0; i < SOURCE_SLOTS; ++i)
        if (observer->sources[i].used) output[count++] = observer->sources[i];
    qsort(output, count, sizeof(*output), compare_sources);
    return count;
}

static void print_report(const struct observer *observer, unsigned int top,
                         unsigned int seconds, bool json, bool suggest,
                         unsigned int min_pps)
{
    struct source_stats *sources = calloc(SOURCE_SLOTS, sizeof(*sources));
    if (!sources) return;
    size_t count = collect_sources(observer, sources);
    size_t shown = count < top ? count : top;
    if (json) {
        printf("{\"seconds\":%u,\"packets\":%llu,\"bytes\":%llu,\"sources\":[",
               seconds, (unsigned long long)observer->packets,
               (unsigned long long)observer->bytes);
        for (size_t i = 0; i < shown; ++i)
            printf("%s{\"ip\":\"%s\",\"packets\":%llu,\"bytes\":%llu,\"syn\":%llu,\"rst\":%llu,\"udp\":%llu,\"dport\":%u}",
                   i ? "," : "", sources[i].address,
                   (unsigned long long)sources[i].packets,
                   (unsigned long long)sources[i].bytes,
                   (unsigned long long)sources[i].syn,
                   (unsigned long long)sources[i].rst,
                   (unsigned long long)sources[i].udp, sources[i].last_dport);
        puts("]}");
    } else {
        printf("Observed %llu packets / %llu bytes in %u seconds\n",
               (unsigned long long)observer->packets,
               (unsigned long long)observer->bytes, seconds);
        printf("%-4s %-39s %12s %12s %10s %10s %10s %8s\n",
               "#", "SOURCE", "PACKETS", "BYTES", "PPS", "SYN", "UDP", "DPORT");
        for (size_t i = 0; i < shown; ++i)
            printf("%-4zu %-39s %12llu %12llu %10llu %10llu %10llu %8u\n",
                   i + 1, sources[i].address,
                   (unsigned long long)sources[i].packets,
                   (unsigned long long)sources[i].bytes,
                   (unsigned long long)(sources[i].packets / (seconds ? seconds : 1)),
                   (unsigned long long)sources[i].syn,
                   (unsigned long long)sources[i].udp, sources[i].last_dport);
    }
    if (suggest) {
        for (size_t i = 0; i < shown; ++i) {
            uint64_t pps = sources[i].packets / (seconds ? seconds : 1);
            if (pps >= min_pps)
                printf("bfban add %s  # %llu pps, %llu SYN, %llu UDP\n",
                       sources[i].address, (unsigned long long)pps,
                       (unsigned long long)sources[i].syn,
                       (unsigned long long)sources[i].udp);
        }
    }
    free(sources);
}

int main(int argc, char **argv)
{
    const char *interface = NULL;
    unsigned int seconds = 30, top = 20, min_pps = 100;
    bool json = false, suggest = false;
    enum capture_direction direction = DIRECTION_INGRESS;
    static const struct option options[] = {
        {"interface", required_argument, NULL, 'i'}, {"seconds", required_argument, NULL, 's'},
        {"top", required_argument, NULL, 't'}, {"list-interfaces", no_argument, NULL, 'l'},
        {"direction", required_argument, NULL, 1003},
        {"json", no_argument, NULL, 1000},
        {"suggest", no_argument, NULL, 1001}, {"min-pps", required_argument, NULL, 1002},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}};
    int option;
    while ((option = getopt_long(argc, argv, "i:s:t:lh", options, NULL)) != -1) {
        switch (option) {
        case 'i': interface = optarg; break;
        case 's': seconds = (unsigned int)strtoul(optarg, NULL, 10); break;
        case 't': top = (unsigned int)strtoul(optarg, NULL, 10); break;
        case 'l': return list_interfaces();
        case 1000: json = true; break;
        case 1001: suggest = true; break;
        case 1002: min_pps = (unsigned int)strtoul(optarg, NULL, 10); break;
        case 1003:
            if (strcmp(optarg, "ingress") == 0) direction = DIRECTION_INGRESS;
            else if (strcmp(optarg, "egress") == 0) direction = DIRECTION_EGRESS;
            else if (strcmp(optarg, "both") == 0) direction = DIRECTION_BOTH;
            else { fprintf(stderr, "bfobserve: invalid direction: %s\n", optarg); return 2; }
            break;
        default: usage(option == 'h' ? stdout : stderr); return option == 'h' ? 0 : 2;
        }
    }
    if (interface)
        fprintf(stderr, "bfobserve: observing interface %s\n", interface);
    else
        fprintf(stderr, "bfobserve: observing all packet interfaces\n");
    int socket_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (socket_fd < 0) { perror("bfobserve: socket(AF_PACKET)"); return 1; }
    if (interface) {
        unsigned int ifindex = if_nametoindex(interface);
        if (!ifindex) { fprintf(stderr, "bfobserve: unknown interface %s\n", interface); return 1; }
        struct sockaddr_ll address = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL),
                                      .sll_ifindex = (int)ifindex};
        if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("bfobserve: bind"); return 1;
        }
    }
    struct observer observer = {0};
    unsigned char packet[PACKET_BUFFER];
    time_t deadline = time(NULL) + seconds;
    while (time(NULL) < deadline) {
        struct pollfd pfd = {.fd = socket_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 1000);
        if (ready <= 0) continue;
        struct sockaddr_ll packet_address;
        socklen_t address_length = sizeof(packet_address);
        ssize_t length = recvfrom(socket_fd, packet, sizeof(packet), 0,
                                  (struct sockaddr *)&packet_address, &address_length);
        if (length > 0) {
            bool outgoing = packet_address.sll_pkttype == PACKET_OUTGOING;
            if (direction == DIRECTION_BOTH ||
                (direction == DIRECTION_EGRESS && outgoing) ||
                (direction == DIRECTION_INGRESS && !outgoing))
                process_packet(&observer, packet, (size_t)length);
        }
    }
    close(socket_fd);
    print_report(&observer, top, seconds, json, suggest, min_pps);
    return 0;
}
