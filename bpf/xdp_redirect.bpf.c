// Chronos-X XDP program: steer IPv4 packets into an AF_XDP socket.
//
// Build on Linux with libbpf headers installed:
//   clang -target bpf -O2 -g -c bpf/xdp_redirect.bpf.c -o bpf/xdp_redirect.bpf.o
//
// Userspace must create an AF_XDP socket and install its fd into xsks_map at
// key ctx->rx_queue_index. Until then, packets fall back to XDP_PASS.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>

#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 4);
    __type(key, __u32);
    __type(value, __u64);
} packet_count SEC(".maps");

#define CHRONOS_STAT_TOTAL 0
#define CHRONOS_STAT_REDIRECT 1
#define CHRONOS_STAT_PASS 2
#define CHRONOS_STAT_DROP 3

static __always_inline void bump_stat(__u32 index)
{
    __u64 *counter = bpf_map_lookup_elem(&packet_count, &index);
    if (counter) {
        *counter += 1;
    }
}

SEC("xdp")
int xdp_chronos_redirect(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) {
        bump_stat(CHRONOS_STAT_DROP);
        return XDP_DROP;
    }

    bump_stat(CHRONOS_STAT_TOTAL);

    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        bump_stat(CHRONOS_STAT_PASS);
        return XDP_PASS;
    }

    struct iphdr *ip = (struct iphdr *)(eth + 1);
    if ((void *)(ip + 1) > data_end || ip->ihl < 5) {
        bump_stat(CHRONOS_STAT_DROP);
        return XDP_DROP;
    }

    __u32 queue_index = ctx->rx_queue_index;
    if (bpf_map_lookup_elem(&xsks_map, &queue_index)) {
        bump_stat(CHRONOS_STAT_REDIRECT);
        return bpf_redirect_map(&xsks_map, queue_index, XDP_PASS);
    }

    bump_stat(CHRONOS_STAT_PASS);
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
