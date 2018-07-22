#include <iostream>
#include <vector>
#include <string>
#include <map>

#include "instructions.hpp"
#include "common.hpp"
#include "constraints.hpp"

using std::tuple;
enum region_t {
    T_NUM,
    T_STACK,
    T_CTX,
    T_DATA,
    T_MAP,
};

// rough estimates:
static constexpr int perf_max_trace_size = 2048;
static constexpr int ptregs_size = (3 + 63 + 8 + 2) * 8;

static constexpr int cgroup_dev_regions = 3 * 4;
static constexpr int kprobe_regions = ptregs_size;
static constexpr int tracepoint_regions = perf_max_trace_size;
static constexpr int perf_event_regions = 3 * 8 + ptregs_size;
static constexpr int socket_filter_regions = 24 * 4;
static constexpr int sched_regions = 24 * 4;
static constexpr int xdp_regions = 5 * 4;
static constexpr int lwt_regions = 24 * 4;
static constexpr int cgroup_sock_regions = 12 * 4;
static constexpr int sock_ops_regions =  42 * 4 + 2 * 8;
static constexpr int sk_skb_regions = 24 * 4;

static constexpr ptype_descr sk_buff = { sk_skb_regions, 15*4, 16*4, 23*4};
static constexpr ptype_descr xdp_md = { xdp_regions, 0, 1*4, 2*4};
static constexpr ptype_descr sk_msg_md = { 11*4, 0, 1*4, -1};

static constexpr ptype_descr unspec_descr = { 0 };
static constexpr ptype_descr cgroup_dev_descr = {cgroup_dev_regions};
static constexpr ptype_descr kprobe_descr = {kprobe_regions};
static constexpr ptype_descr tracepoint_descr = {tracepoint_regions};
static constexpr ptype_descr perf_event_descr = {perf_event_regions};
static constexpr ptype_descr socket_filter_descr = sk_buff;
static constexpr ptype_descr sched_descr = sk_buff;
static constexpr ptype_descr xdp_descr = xdp_md;
static constexpr ptype_descr lwt_xmit_descr = sk_buff;
static constexpr ptype_descr lwt_inout_descr = sk_buff;
static constexpr ptype_descr cgroup_sock_descr = {cgroup_sock_regions};
static constexpr ptype_descr sock_ops_descr = {sock_ops_regions};
static constexpr ptype_descr sk_skb_descr = sk_buff;

const std::map<ebpf_prog_type, ptype_descr> descriptors {
	{EBPF_PROG_TYPE_UNSPEC, unspec_descr},
	{EBPF_PROG_TYPE_CGROUP_DEVICE, cgroup_dev_descr},
	{EBPF_PROG_TYPE_KPROBE, kprobe_descr},
	{EBPF_PROG_TYPE_TRACEPOINT, tracepoint_descr},
    {EBPF_PROG_TYPE_RAW_TRACEPOINT, tracepoint_descr},
	{EBPF_PROG_TYPE_PERF_EVENT, perf_event_descr},
	{EBPF_PROG_TYPE_SOCKET_FILTER, socket_filter_descr},
	{EBPF_PROG_TYPE_CGROUP_SKB, socket_filter_descr},
	{EBPF_PROG_TYPE_SCHED_ACT, sched_descr},
	{EBPF_PROG_TYPE_SCHED_CLS, sched_descr},
	{EBPF_PROG_TYPE_XDP, xdp_descr},
	{EBPF_PROG_TYPE_LWT_XMIT, lwt_xmit_descr},
	{EBPF_PROG_TYPE_LWT_IN,  lwt_inout_descr},
	{EBPF_PROG_TYPE_LWT_OUT, lwt_inout_descr},
	{EBPF_PROG_TYPE_CGROUP_SOCK, cgroup_sock_descr},
	{EBPF_PROG_TYPE_SOCK_OPS, sock_ops_descr},
	{EBPF_PROG_TYPE_SK_SKB, sk_skb_descr},
    {EBPF_PROG_TYPE_SK_MSG, sk_msg_md},
};

constraints::constraints(ebpf_prog_type prog_type) : ctx_desc{descriptors.at(prog_type)}
{
    for (int i=0; i < 16; i++) {
        regs.emplace_back(vfac, i);
    }
}

void constraints::setup_entry(basic_block_t& entry)
{
    //entry.havoc(regs[10].value);
    entry.assume(1 <= regs[10].value);
    entry.assign(regs[10].offset, 0);
    entry.assign(regs[10].region, T_STACK);

    //entry.havoc(regs[1].value);
    entry.assume(1 <= regs[1].value);
    entry.assign(regs[1].offset, 0);
    entry.assign(regs[1].region, T_CTX);

    entry.assume(0 <= total_size);
    if (ctx_desc.meta < 0) {
        entry.assign(meta_size, 0);
    } else {
        entry.assume(0 <= meta_size);
        entry.assume(meta_size <= total_size);
    }
}

static auto eq(var_t& a, var_t& b)
{
    return lin_cst_t(a - b, lin_cst_t::EQUALITY);
}

static lin_cst_t jmp_to_cst_offsets(uint8_t opcode, int imm, var_t& odst, var_t& osrc)
{
    switch (opcode) {
    case EBPF_OP_JEQ_REG:
        return eq(odst, osrc);

    case EBPF_OP_JGE_REG:  return odst >= osrc; // FIX unsigned
    case EBPF_OP_JSGE_REG: return odst >= osrc;
    case EBPF_OP_JLE_REG:  return odst <= osrc; // FIX unsigned
    case EBPF_OP_JSLE_REG: return odst <= osrc;
    case EBPF_OP_JNE_REG:
        return lin_cst_t(odst - osrc, lin_cst_t::DISEQUATION);
    
    case EBPF_OP_JGT_REG:  return odst > osrc; // FIX unsigned
    case EBPF_OP_JSGT_REG: return odst > osrc;

    // Note: reverse the test as a workaround strange lookup:
    case EBPF_OP_JLT_REG:  return osrc > odst; // FIX unsigned
    case EBPF_OP_JSLT_REG: return osrc > odst;
    }
    return odst - odst == 0;
}


static lin_cst_t jmp_to_cst(uint8_t opcode, int imm, var_t& vdst, var_t& vsrc)
{
    switch (opcode) {
    case EBPF_OP_JEQ_IMM:  return vdst == imm;
    case EBPF_OP_JEQ_REG:
        return lin_cst_t(vdst - vsrc, lin_cst_t::EQUALITY);

    case EBPF_OP_JGE_IMM:  return vdst >= imm; // FIX unsigned
    case EBPF_OP_JGE_REG:  return vdst >= vsrc; // FIX unsigned

    case EBPF_OP_JSGE_IMM: return vdst >= imm;
    case EBPF_OP_JSGE_REG: return vdst >= vsrc;
    
    case EBPF_OP_JLE_IMM:  return vdst <= imm; // FIX unsigned
    case EBPF_OP_JLE_REG:  return vdst <= vsrc; // FIX unsigned
    case EBPF_OP_JSLE_IMM: return vdst <= imm;
    case EBPF_OP_JSLE_REG: return vdst <= vsrc;

    case EBPF_OP_JNE_IMM:  return vdst != imm;
    case EBPF_OP_JNE_REG:
        return lin_cst_t(vdst - vsrc, lin_cst_t::DISEQUATION);
    
    case EBPF_OP_JGT_IMM:  return vdst > imm; // FIX unsigned
    case EBPF_OP_JGT_REG:  return vdst > vsrc; // FIX unsigned
    case EBPF_OP_JSGT_IMM: return vdst > imm;
    case EBPF_OP_JSGT_REG: return vdst > vsrc;

    case EBPF_OP_JLT_IMM:  return vdst < imm; // FIX unsigned
    // Note: reverse the test as a workaround strange lookup:
    case EBPF_OP_JLT_REG:  return vsrc > vdst; // FIX unsigned
    case EBPF_OP_JSLT_IMM: return vdst < imm;
    case EBPF_OP_JSLT_REG: return vsrc > vdst;
    }
    assert(false);
}


void constraints::jump(ebpf_inst inst, basic_block_t& block, bool taken)
{
    uint8_t opcode = taken ? inst.opcode : reverse(inst.opcode);
    lin_cst_t cst = jmp_to_cst(opcode, inst.imm, regs[inst.dst].value, regs[inst.src].value);
    block.assume(cst);

    lin_cst_t offset_cst = jmp_to_cst_offsets(opcode, inst.imm, regs[inst.dst].offset, regs[inst.src].offset);
    if (!offset_cst.is_tautology()) {
        block.assume(offset_cst);
    }
}

static void wrap32(basic_block_t& block, var_t& vdst)
{
    block.bitwise_and(vdst, vdst, UINT32_MAX);
}


void constraints::no_pointer(basic_block_t& block, dom_t& v)
{
    block.havoc(v.offset);
    block.assign(v.region, T_NUM);
}

static basic_block_t& insert_midnode(cfg_t& cfg, basic_block_t& pre, basic_block_t& post, std::string subname)
{
    auto& mid = cfg.insert(pre.label() + ":" + subname);
    pre >> mid;
    mid >> post;
    return mid;
}

template<typename Dom, typename T>
void load_datapointer(cfg_t& cfg, basic_block_t& pre, basic_block_t& post, Dom& target, 
    std::string subname, lin_cst_t cst, T lower_bound)
{
    auto& mid = insert_midnode(cfg, pre, post, subname);
    mid.assume(cst);

    mid.assign(target.region, T_DATA);
    mid.havoc(target.value);
    mid.assume(1 <= target.value);
    mid.assign(target.offset, lower_bound);
}

void constraints::exec_ctx_access(ikos::linear_expression<ikos::z_number, varname_t> addr,
        basic_block_t& mid, basic_block_t& exit, unsigned int _pc, cfg_t& cfg, ebpf_inst inst)
{
    int width = access_width(inst.opcode);
    if (is_load(inst.opcode)) {
        auto target = regs[inst.dst];
        if (ctx_desc.data >= 0) {
            load_datapointer(cfg, mid, exit, target, "data_start", addr == ctx_desc.data, meta_size);
            load_datapointer(cfg, mid, exit, target, "data_end", addr == ctx_desc.end, total_size);
        }
        if (ctx_desc.meta >= 0) {
            load_datapointer(cfg, mid, exit, target, "meta", addr == ctx_desc.meta, 0);
        }
        auto& normal = insert_midnode(cfg, mid, exit, "assume_ctx_not_special");
        if (ctx_desc.data >= 0) {
            normal.assume(addr != ctx_desc.data);
            normal.assume(addr != ctx_desc.end);
        }
        if (ctx_desc.meta >= 0) {
            normal.assume(addr != ctx_desc.meta);
        }
        ctx_arr.load(normal, regs[inst.dst], addr, width);
    } else {
        ctx_arr.store(mid, addr, regs[inst.src], width);
        mid >> exit;
    }
}

bool constraints::exec_mem_access(basic_block_t& block, basic_block_t& exit, unsigned int _pc, cfg_t& cfg, ebpf_inst inst)
{
    crab::cfg::debug_info di{"pc", _pc, 0};

    uint8_t mem = is_load(inst.opcode) ? inst.src : inst.dst;
    int width = access_width(inst.opcode);

    if (mem == 10) {
        auto offset = -inst.offset;
        // not dynamic
        assert(offset >= width);
        assert(offset <= STACK_SIZE);
        if (is_load(inst.opcode)) {
            stack_arr.load(block, regs[inst.dst], offset, width);
        } else {
            stack_arr.store(block, offset, regs[inst.src], width);
        }
        return false;
    } else if ((inst.opcode & 0xE0) == 0x20) { // TODO NAME
        // load only
        auto target = regs[inst.dst];
        ctx_arr.load(block, target, inst.offset, width);
        if (inst.offset == ctx_desc.data) {
            if (ctx_desc.meta > 0)
                block.assign(target.offset, meta_size);
            else 
                block.assign(target.offset, 0);
        } else if (inst.offset == ctx_desc.end) {
            block.assign(target.offset, total_size);
        } else if (inst.offset == ctx_desc.meta) {
            block.assign(target.offset, 0);
        } else {
            return false;
        }
        block.havoc(target.value);
        block.assertion(target.value != 0);
        block.assign(target.region, T_DATA);
        return false;
    } else {
        block.assertion(regs[mem].value != 0, di);
        block.assertion(regs[mem].region != T_NUM, di);

        {
            auto& mid = insert_midnode(cfg, block, exit, "assume_stack");
            auto addr = regs[mem].offset - inst.offset; // XXX: - ?
            mid.assume(regs[mem].region == T_STACK);
            mid.assertion(addr >= width, di);
            mid.assertion(addr <= STACK_SIZE, di);
            if (is_load(inst.opcode)) {
                stack_arr.load(mid, regs[inst.dst], addr, width);
            } else {
                stack_arr.store(mid, addr, regs[inst.src], width);
            }
        }
        {
            auto& mid = cfg.insert(label(_pc, "assume_ctx"));
            block >> mid;
            auto addr = regs[mem].offset + inst.offset;
            mid.assume(regs[mem].region == T_CTX);
            mid.assertion(addr >= 0, {"pc", _pc, 0});
            mid.assertion(addr <= ctx_desc.size - width, {"pc", _pc, 0});
            exec_ctx_access(addr, mid, exit, _pc, cfg, inst);
        }
        if (ctx_desc.data >= 0) {
            auto& mid = insert_midnode(cfg, block, exit, "assume_data");
            auto addr = regs[mem].offset + inst.offset;
            mid.assume(regs[mem].region == T_DATA);
            mid.assertion(addr >= 0, di);
            mid.assertion(addr <= total_size - width, di);
            if (is_load(inst.opcode)) {
                data_arr.load(mid, regs[inst.dst], addr, width);
            } else {
                data_arr.store(mid, addr, regs[inst.src], width);
            }
        }
        {
            auto& mid = insert_midnode(cfg, block, exit, "assume_map");
            auto addr = regs[mem].offset + inst.offset;
            mid.assume(regs[mem].region == T_MAP);
            mid.assertion(addr >= 0, di);
            constexpr int MAP_SIZE = 256;
            mid.assertion(addr <= MAP_SIZE - width, di);
        }
        return true;
    }
}

void constraints::exec(ebpf_inst inst, basic_block_t& block, basic_block_t& exit, unsigned int _pc, cfg_t& cfg)
{
    auto& dst = regs[inst.dst];

    var_t& vdst = regs[inst.dst].value;
    var_t& odst = regs[inst.dst].offset;
    var_t& rdst = regs[inst.dst].region;

    var_t& vsrc = regs[inst.src].value;
    var_t& osrc = regs[inst.src].offset;
    var_t& rsrc = regs[inst.src].region;

    int imm = inst.imm;

    bool exit_linked = false;

    switch (inst.opcode) {
    case EBPF_OP_ADD_IMM:
        block.add(vdst, vdst, imm);
        wrap32(block, vdst);
        block.add(odst, odst, imm);
        break;
    case EBPF_OP_ADD_REG:
        block.add(vdst, vdst, vsrc);
        wrap32(block, vdst);
        block.add(odst, odst, vsrc);
        break;
    case EBPF_OP_SUB_IMM:
        block.sub(vdst, vdst, imm);
        wrap32(block, vdst);
        block.sub(odst, odst, imm);
        break;
    case EBPF_OP_SUB_REG:
        block.sub(vdst, vdst, vsrc);
        wrap32(block, vdst);
        // TODO: meet of this and "odst - odst" if same region
        block.sub(odst, odst, vsrc);
        break;
    case EBPF_OP_MUL_IMM:
        block.mul(vdst, vdst, imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MUL_REG:
        block.mul(vdst, vdst, vsrc);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_DIV_IMM:
        block.div(vdst, vdst, vsrc); // TODO: u32(vdst) / u32(imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_DIV_REG:
        block.div(vdst, vdst, vsrc); // TODO: u32(vdst) / u32(src);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_OR_IMM:
        block.bitwise_or(vdst, vdst, imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_OR_REG:
        block.bitwise_or(vdst, vdst, vsrc);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_AND_IMM:
        block.bitwise_and(vdst, vdst, imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_AND_REG:
        block.bitwise_and(vdst, vdst, vsrc);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_LSH_IMM:
        block.shl(vdst, vdst, imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_LSH_REG:
        block.shl(vdst, vdst, vsrc);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_RSH_IMM:
        block.lshr(vdst, vdst, imm); // TODO u32(vdst) >> imm;
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_RSH_REG:
        block.lshr(vdst, vdst, vsrc); // TODO u32(vdst) >> src;
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_NEG:
        block.mul(vdst, vdst, -1); // ???
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOD_IMM:
        block.rem(vdst, vdst, imm); // FIX: vdst = u32(vdst) % u32(imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOD_REG:
        block.rem(vdst, vdst, vsrc); // FIX: vdst = u32(vdst) % u32(vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_XOR_IMM:
        block.bitwise_xor(vdst, vdst, imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_XOR_REG:
        block.bitwise_xor(vdst, vdst, vsrc);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOV_IMM:
        block.assign(vdst, imm);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOV_REG:
        block.assign(vdst, vsrc);
        wrap32(block, vdst);
        block.assign(odst, osrc);
        block.assign(rdst, rsrc);
        break;
    case EBPF_OP_ARSH_IMM:
        block.ashr(vdst, vdst, imm); // FIX: (int32_t)dst >> imm;
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;
    case EBPF_OP_ARSH_REG:
        block.ashr(vdst, vdst, vsrc); // FIX = (int32_t)dst >> u32(src);
        wrap32(block, vdst);
        no_pointer(block, dst);
        break;

    case EBPF_OP_LE:
    case EBPF_OP_BE:
        block.havoc(vdst);
        no_pointer(block, dst);
        break;

    case EBPF_OP_ADD64_IMM:
        block.add(vdst, vdst, imm);
        block.add(odst, odst, imm);
        break;
    case EBPF_OP_ADD64_REG:
        block.add(vdst, vdst, vsrc);
        block.add(odst, odst, vsrc); //XXX note vsrc
        break;
    case EBPF_OP_SUB64_IMM:
        block.sub(vdst, vdst, imm);
        block.sub(odst, odst, imm);
        break;
    case EBPF_OP_SUB64_REG:
        block.sub(odst, odst, osrc);
        block.sub(odst, odst, vsrc); // XXX note vsrc
        break;
    case EBPF_OP_MUL64_IMM:
        block.mul(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MUL64_REG:
        block.mul(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_DIV64_IMM:
        block.div(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_DIV64_REG:
        block.div(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_OR64_IMM:
        block.bitwise_or(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_OR64_REG:
        block.bitwise_or(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_AND64_IMM:
        block.bitwise_and(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_AND64_REG:
        block.bitwise_and(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_LSH64_IMM:
        block.lshr(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_LSH64_REG:
        block.lshr(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_RSH64_IMM:
        block.ashr(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_RSH64_REG:
        block.ashr(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_NEG64:
        block.mul(vdst, vdst, -1); // ???
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOD64_IMM:
        block.rem(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOD64_REG:
        block.rem(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_XOR64_IMM:
        block.bitwise_xor(vdst, vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_XOR64_REG:
        block.bitwise_xor(vdst, vdst, vsrc);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOV64_IMM:
        block.assign(vdst, imm);
        no_pointer(block, dst);
        break;
    case EBPF_OP_MOV64_REG:
        block.assign(vdst, vsrc);
        block.assign(odst, osrc);
        block.assign(rdst, rsrc);
        break;
    case EBPF_OP_ARSH64_IMM:
        block.ashr(vdst, vdst, imm); // = (int64_t)dst >> imm;
        no_pointer(block, dst);
        break;
    case EBPF_OP_ARSH64_REG:
        block.ashr(vdst, vdst, vsrc); // = (int64_t)dst >> src;
        no_pointer(block, dst);
        break;
    case EBPF_OP_LDDW:
        block.assign(vdst, (uint32_t)inst.imm | ((uint64_t)imm << 32));
        no_pointer(block, dst);
        break;
    case EBPF_OP_JEQ_IMM:
    case EBPF_OP_JEQ_REG:
    case EBPF_OP_JGE_IMM:
    case EBPF_OP_JGE_REG:
    case EBPF_OP_JSGE_IMM:
    case EBPF_OP_JSGE_REG:
    case EBPF_OP_JLE_IMM:
    case EBPF_OP_JLE_REG:
    case EBPF_OP_JSLE_IMM:
    case EBPF_OP_JSLE_REG:
    case EBPF_OP_JNE_IMM:
    case EBPF_OP_JNE_REG:
    case EBPF_OP_JGT_IMM:
    case EBPF_OP_JGT_REG:
    case EBPF_OP_JSGT_IMM:
    case EBPF_OP_JSGT_REG:
    case EBPF_OP_JLT_IMM:
    case EBPF_OP_JLT_REG:
    case EBPF_OP_JSLT_IMM:
    case EBPF_OP_JSLT_REG:
    case EBPF_OP_JA:
    case EBPF_OP_EXIT:
        break;

    case EBPF_OP_CALL:
        for (int i=1; i<=5; i++) {
            block.havoc(regs[i].value);
            block.havoc(regs[i].region);
            block.havoc(regs[i].offset);
        }
        if (inst.imm == 0x1) {
            block.assign(regs[0].offset, 0);
            block.assign(regs[0].region, T_MAP);
            block.havoc(regs[0].value);
        } else {
            block.havoc(regs[0].offset);
            block.havoc(regs[0].value);
            block.havoc(regs[0].region);
        }
        break;
    default:
        if (is_access(inst.opcode)) {
            exit_linked = exec_mem_access(block, exit, _pc, cfg, inst);
        } else {
            std::cout << "bad instruction " << (int)inst.opcode << " at "<< _pc << "\n";
        }
        break;
    }

    if (!exit_linked) {
        block >> exit;
    }
}