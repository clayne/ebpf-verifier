#include <uapi/linux/bpf.h>
#include "bpf_helpers.h"

struct bpf_map_def SEC("maps") m1 = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(u32),
	.value_size = 30,
	.max_entries = 1,
};

struct bpf_map_def SEC("maps") m2 = {
	.type = BPF_MAP_TYPE_ARRAY,
	.key_size = sizeof(u32),
	.value_size = 30,
	.max_entries = 1,
};

SEC("sk_skb/manual-strcmp")
int manual_strcmp(struct __sk_buff *skb)
{
	u32 key1 = 1;
	u32 key2 = 1;
	char* value1;
	char* value2;
	value1 = bpf_map_lookup_elem(&m1, &key1);
	value2 = bpf_map_lookup_elem(&m2, &key2);
	if (!value1 || !value2) return 1;
	int res = 1;
	int i = 0;
#pragma clang loop unroll_count(30)
	for (; i < 30; i++) {
		if (value1[i] != value2[i]) {
			res = 0;
			break;
		}
		if (value1[i] == '\0')
			break;
	}
	int j = 0;
#pragma clang loop unroll_count(30)
	for (; j < 30; j++) {
		if (value1[j] != value2[j]) {
			res = 0;
			break;
		}
		if (value1[j] == '\0')
			break;
	}
	return res && i == j;
}

char _license[] SEC("license") = "GPL";
u32 _version SEC("version") = 0x041800;
