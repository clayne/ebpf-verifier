
kern/manual_memset.o:	file format ELF64-BPF

Disassembly of section sk_skb/manual-memset:
manual_memset:
       0:	r2 = *(u32 *)(r1 + 76)
       1:	r1 = *(u32 *)(r1 + 80)
       2:	r1 += -1
       3:	if r2 > r1 goto +4 <LBB0_3>
       4:	r3 = 15

LBB0_2:
       5:	*(u8 *)(r1 + 0) = r3
       6:	r1 += -1
       7:	if r1 >= r2 goto -3 <LBB0_2>

LBB0_3:
       8:	r0 = 0
       9:	exit