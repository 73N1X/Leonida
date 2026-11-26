"""Wave64 programs on 32-wide subgroups: is the program partition invariant? (SHADER-10)

A wave64 program is exact on 32-lane subgroups when what it observes (exports, memory) does not change if its
wave is cut into two 32-lane halves, each run as a wave of its own. The host forms the subgroups of a VS/PS/GS, so
there is no partner half to exchange values with. A compute or mesh shader has workgroup memory and
--wave64-emulate, which the translator now keeps for tier C only (rdna2spirv.compute_halves): tiers A and B run
natively with one half per subgroup. This module decides that property from the code, conservatively, for the
translator (rdna2spirv.py, shader index keys `wave64_graphics` and `wave64_compute`) and for the corpus census
(`--census`):

    analyse(blocks, succ, masks, stage, group=None, lin=()) -> {"tier": "A" | "B" | "C", "reasons": [...],
        "idioms": [...], "uniform_readlane": {id(ins), ...}, "sync": {id(ins), ...}, "drop_barriers": bool}

    A  nothing wave-wide is consumed except votes that only skip lane-masked work, votes on a wave-uniform compare
       (a v_cmp of untainted scalars and constants with EXEC the launch mask: every half votes as the wave does),
       whole-quad masks and per-lane arithmetic: exact as translated, on any subgroup size.
    B  also uses a wave-wide idiom whose result is invariant by construction: a waterfall loop, a wave-aggregated
       atomic (ballot count, one atomic from the first lane, rank), a v_readfirstlane / v_readlane of a value that
       is the same in every lane. Exact (the aggregated atomic up to the order in which lanes get slots), with the
       rewrites listed in docs/design.md (a readlane of a uniform value reads the own copy).
    C  not exact on a 32-wide subgroup: a lane count, rank, first-lane index or lane id that reaches an export,
       a store's data or a branch, readlane / writelane of varying data, a 32-bit access to half of a lane mask,
       scalar state defined behind a wave-wide vote and read where the vote did not run, ... Each reason is a
       short code (REASONS).

Compute and mesh (stage "cs" / "ms", see docs/design.md "Wave64 compute and mesh: partition invariance"): lanes keep
their wave numbers there (the translator relabels the thread index from the subgroup), so a lane id is exact; the
first lane's D + lane keeps its bits from 6 up; a one-half EXEC (thread 0) marks the region only that half runs;
and compute_checks adds what two subgroups must agree on besides values: s_barrier (left out when nothing is
written, else not where a vote decides), LDS and scalar-load order inside the wave (a workgroup barrier in front of
an access that follows a conflicting one, `sync`, only where every invocation runs), the group size.

How: a forward dataflow over the basic blocks gives every register a set of taint bits (what partition-dependent
quantity its value derives from) and, for the few idioms that are exact, a tag naming the proof object (the exec
token a rank counts, the VGPR a waterfall scalar was read from). Sinks (exports, store data, branch conditions)
must be clean. Scalar registers are also checked against control dependence: a scalar defined in code that only
runs when a wave-wide vote says some lane is on (an s_cbranch_vccz, execz, ...) differs between the halves when
one half votes yes and the other no, so a read that may see that def or another one is tainted (CTRL).

The pre-translation opcode scan over a disassembly folder is tools/wave64_graphics_scan.py (#1132): it counts
candidate cross-half opcodes and decides nothing. This module decides per shader (tier); its --census only totals
the tiers the translator wrote into an index.json. They answer different questions.

Standard library only.
Usage: `python -I tools/wave64_halves.py --help` lists the options.
"""
import argparse
import json
import os
import re
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import lane_masks  # noqa: E402

# taint bits
CNT = 1         # number of lanes of a mask in the own half (s_bcnt1 of a lane mask)
RNK = 2         # rank among the set lanes of a mask below this lane (v_mbcnt)
FF1 = 4         # index of the first set lane of a mask (s_ff1)
BASE = 8        # value that depends on the order lanes reached an atomic or on which lane was elected
ELECT = 16      # defined by the elected lane of a wave-aggregated atomic (valid for the first lane of a mask only)
LANE = 32       # raw lane number
X = 64          # anything else that depends on the partition
CTRL = 128      # scalar defined behind a wave-wide vote and read where the def may not have run
MASKD = 256     # a lane mask used as data
WFS = 512       # scalar read from the first lane of a waterfall loop
ACT_RFL = 1024  # scalar from v_readfirstlane of active-EXEC-only uniform VGPR (valid only inside that EXEC mask)
LINF = 2048     # compute/mesh: v_readfirstlane of a lane-linear VGPR (D + lane, D a multiple of 64 the same in the
                # whole wave): D + an offset below 64 that depends on the half, so only bits 6 and up are invariant
TAG_BITS = RNK | ELECT | WFS | ACT_RFL | LINF

NAMES = {CNT: "lane-count", RNK: "lane-rank", FF1: "first-lane", BASE: "allocation-order", ELECT: "elected-lane",
         LANE: "lane-id", X: "cross-lane", CTRL: "stale-scalar", MASKD: "mask-as-data", WFS: "waterfall-scalar",
         ACT_RFL: "cross-lane", LINF: "first-lane"}
# compute and mesh only (see the Compute section of the docstring): vote-barrier, lds-in-wave, mem-in-wave,
# nested-barrier, group-size, tainted-message
REASONS = sorted(set(NAMES.values()) | {"tainted-branch", "unsafe-vote-branch", "mask-half", "elected-side-effect",
                                        "tainted-exec", "mask-compare", "unmodelled-op", "unsafe-execz-branch",
                                        "analysis-error", "vote-barrier", "lds-in-wave", "mem-in-wave",
                                        "nested-barrier", "group-size", "tainted-message"})

BRANCHES = {"s_branch", "s_cbranch_scc0", "s_cbranch_scc1", "s_cbranch_vccz", "s_cbranch_vccnz",
            "s_cbranch_execz", "s_cbranch_execnz"}
NO_DEST = ("s_cmp", "s_bitcmp", "v_cmpx", "exp", "buffer_store", "global_store", "flat_store", "scratch_store",
           "tbuffer_store", "image_store", "ds_write", "ds_store", "s_store", "s_cbranch", "s_branch", "s_waitcnt",
           "s_nop", "s_endpgm", "s_barrier", "s_sendmsg", "s_setprio", "s_inst_prefetch", "s_clause", "s_trap",
           "s_icache", "s_dcache", "s_sleep", "s_code_end", "s_denorm_mode", "s_round_mode", "s_waitcnt_vscnt",
           "s_ttracedata", "v_nop", "s_set", "s_sethalt", "s_setkill", "s_wakeup", "s_version", "buffer_gl",
           "buffer_wbinvl", "ds_gws", "ds_nop")
LOADS = ("buffer_load", "global_load", "flat_load", "scratch_load", "tbuffer_load", "image_load", "image_sample",
         "image_gather", "image_get", "s_load", "s_buffer_load", "ds_read", "v_interp")
SINKS = ("exp", "buffer_store", "global_store", "flat_store", "scratch_store", "tbuffer_store", "image_store",
         "ds_write", "ds_store")
SCC_LESS = ("s_mov", "s_cmov", "s_cselect", "s_load", "s_buffer_load", "s_getpc", "s_setpc", "s_nop", "s_waitcnt",
            "s_sendmsg", "s_getreg", "s_setreg", "s_movrel", "s_bitset", "s_movk", "s_mul", "s_pack", "s_sext",
            "s_flbit", "s_ff0", "s_ff1", "s_brev", "s_bfm", "s_bitreplicate", "s_get_waveid", "s_cbranch", "s_branch",
            "s_endpgm", "s_barrier", "v_", "buffer_", "global_", "ds_", "exp", "image_", "flat_", "s_set", "s_inst",
            "s_clause")
SAVEEXEC = ("s_and_saveexec_b", "s_or_saveexec_b", "s_xor_saveexec_b", "s_andn2_saveexec_b", "s_orn2_saveexec_b",
            "s_nand_saveexec_b", "s_nor_saveexec_b", "s_xnor_saveexec_b", "s_andn1_saveexec_b", "s_orn1_saveexec_b",
            "s_and_wrexec_b", "s_andn2_wrexec_b", "s_andn1_wrexec_b")
RMW_DEST = (
    "v_fmac_f32", "v_fmac_f16", "v_mac_f32", "v_mac_legacy_f32", "v_pk_fmac_f16",
    "v_dot2c_f32_f16", "v_dot2c_f32_bf16", "v_dot2c_i32_i16", "v_dot4c_i32_i8", "v_dot8c_i32_i4",
    "s_addk_i32", "s_mulk_i32", "s_cmovk_i32", "s_cmov_b32", "s_cmov_b64",
    "s_bitset0_b32", "s_bitset0_b64", "s_bitset1_b32", "s_bitset1_b64",
)
MOVREL_OPS = (
    "v_movrels_b32", "v_movreld_b32", "v_movrelsd_b32", "v_movrelsd_2_b32",
    "v_swaprel_b32", "s_movrels_b32", "s_movrels_b64",
)
MASK_BITWISE = ("s_and_b", "s_or_b", "s_xor_b", "s_andn2_b", "s_orn2_b", "s_nand_b", "s_nor_b", "s_xnor_b", "s_not_b",
                "s_mov_b", "s_cselect_b", "s_wqm_b")
DPP_BAD = ("row_bcast", "wave_")
UVOTE = ("uvote", None)  # tag of a lane mask that is all launch lanes or none in the wave and in each half (see vote)


_TYPE16 = re.compile(r"^[biuf]16$")
_TYPE = re.compile(r"^[biuf](8|16|24|32|64)$")
# GFX10 VOP2 16-bit integer ops: in their VOP2 (_e32) encoding they define the whole register (design.md, "16-bit
# results"); their _e64 forms are opsel-capable and keep the high half
VOP2_INT16 = {"v_add_u16", "v_sub_u16", "v_subrev_u16", "v_mul_lo_u16", "v_lshlrev_b16", "v_lshrrev_b16",
              "v_ashrrev_i16", "v_max_u16", "v_max_i16", "v_min_u16", "v_min_i16"}


def partial_write(ins, op):
    """Whether a VALU instruction writes only part of its 32-bit destination and keeps the rest (design.md, "16-bit
    results" and "16-bit SDWA result"): SDWA with a byte or word dst_sel and UNUSED_PRESERVE; otherwise a 16-bit
    result, the first type in the name (v_add_f16, v_cvt_f16_f32, v_mad_u16, v_add_u16_e64), except the VOP2 16-bit
    integer ops in _e32 form, which define the whole register. Packed and 32-bit results are whole: v_pk_*,
    v_cvt_pk*, v_sad_*, v_cvt_f32_f16, v_mad_u32_u16. When unsure it says partial, which only costs tier A."""
    mods = ins.get("mods") or []
    op = ins.get("op", op)   # the printed name: base_op drops _e32/_e64/_sdwa, which decide it here
    if "_sdwa" in op or any(m.startswith("dst_sel:") for m in mods):
        return "dst_unused:UNUSED_PRESERVE" in mods and any(m.startswith(("dst_sel:BYTE_", "dst_sel:WORD_"))
                                                             for m in mods)
    name = op.split("_e32")[0].split("_e64")[0].split("_dpp")[0]
    if name.startswith(("v_pk_", "v_cvt_pk", "v_sad_")):
        return False
    first = next((p for p in name.split("_")[1:] if _TYPE.match(p)), None)
    if not (first and _TYPE16.match(first)):
        return False
    return not (name in VOP2_INT16 and "_e64" not in op)


def base_op(op):
    for suffix in ("_e32", "_e64", "_sdwa", "_dpp"):
        if op.endswith(suffix):
            return op[:-len(suffix)]
    return op


def _regs(a):
    """Register keys an operand names: ("s", n), ("v", n), "vcc", "exec", "scc", "m0"."""
    k = a.get("k")
    if k == "sgpr":
        return [("s", a["r"] + i) for i in range(a.get("n", 1))]
    if k == "vgpr":
        return [("v", a["r"] + i) for i in range(a.get("n", 1))]
    if k == "vlist":
        return [("v", r) for r in a["r"]]
    if k == "slist":
        regs = []
        for r in a["r"]:
            if isinstance(r, int):
                key = ("s", r)
            elif isinstance(r, str) and r.startswith("vcc"):
                key = "vcc"
            elif isinstance(r, str) and r.startswith("exec"):
                key = "exec"
            else:
                continue
            if key not in regs:
                regs.append(key)
        return regs
    if k == "special":
        t = a.get("t", "")
        if t.startswith("vcc"):
            return ["vcc"]
        if t.startswith("exec"):
            return ["exec"]
        if t in ("scc", "m0"):
            return [t]
    return []


def _half(a):
    """'lo' / 'hi' for a 32-bit half of EXEC or VCC (not the whole register), else None."""
    if a.get("k") == "special":
        t = a.get("t", "")
        if t in ("exec_lo", "vcc_lo"):
            return "lo"
        if t in ("exec_hi", "vcc_hi"):
            return "hi"
    return None


def _zero(a):
    return a.get("k") == "imm" and a.get("v") == 0


def _mask64(a):
    """A whole 64-bit SGPR pair or VCC (a lane mask register), not EXEC and not a 32-bit half."""
    if a.get("k") == "sgpr":
        return a.get("n", 1) == 2
    return a.get("k") == "special" and a.get("t") == "vcc"


ZERO_SCC = ("s_and_b64", "s_andn2_b64", "s_or_b64", "s_orn2_b64", "s_xor_b64", "s_nand_b64", "s_nor_b64",
            "s_xnor_b64", "s_not_b64", "s_wqm_b64")      # SCC = (64-bit result != 0)
# Before the run side narrows EXEC: mask arithmetic, and the SAVEEXEC/WREXEC forms whose new EXEC is S0 & EXEC or
# S0 & ~EXEC (a narrowing into S0; confine_step decides whether S0 is inside, and what the destination holds).
OFF_OK = ("s_nop", "s_waitcnt", "s_mov_b64", "s_wqm_b64", "s_and_b64", "s_and_saveexec_b64", "s_and_wrexec_b64",
          "s_andn2_saveexec_b64", "s_andn2_wrexec_b64")


def operands(ins):
    """(dests, srcs, implicit defs, implicit uses): operand dicts, and register keys the text does not show."""
    op, a = base_op(ins["op"]), ins["args"]
    mods = ins.get("mods") or []
    defs, uses = [], []
    if op.startswith(NO_DEST) and not op.startswith(("s_cmp", "s_bitcmp", "v_cmpx")) or op in ("s_endpgm", "s_barrier"):
        dests, srcs = [], list(a)
    elif op.startswith(("s_cmp", "s_bitcmp")):
        dests, srcs = [], list(a)
        defs.append("scc")
    elif op.startswith("v_cmpx"):
        dests, srcs = [], list(a)
        defs.append("exec")
    elif op.startswith("v_cmp"):
        if len(a) == 2:
            dests, srcs = [], list(a)
            defs.append("vcc")
        else:
            dests, srcs = [a[0]], list(a[1:])
    elif op.startswith("buffer_atomic") or op.startswith(("global_atomic", "flat_atomic")):
        dests = [a[0]] if "glc" in mods and a else []
        srcs = list(a)
    elif op.startswith("ds_"):
        returns = op.startswith(("ds_read", "ds_bpermute", "ds_permute", "ds_swizzle", "ds_consume", "ds_append")) \
            or "_rtn" in op
        dests, srcs = ([a[0]], list(a[1:])) if returns and a else ([], list(a))
    elif op == "v_swap_b32" and len(a) >= 2:
        dests, srcs = [a[0], a[1]], [a[0], a[1]]
    elif a:
        if op in RMW_DEST:
            dests, srcs = [a[0]], [a[0], *a[1:]]
        else:
            dests, srcs = [a[0]], list(a[1:])
        if op.startswith("v_") and len(a) > 2 and (
                "_co_" in op or op in ("v_mad_u64_u32", "v_mad_i64_i32", "v_div_scale_f32", "v_div_scale_f64")) \
                and a[1].get("k") in ("sgpr", "special", "slist"):
            dests, srcs = [a[0], a[1]], list(a[2:])
    else:
        dests, srcs = [], []
    if any(op.startswith(p) for p in SAVEEXEC):
        defs += ["exec", "scc"]
    if op.startswith("s_") and not op.startswith(SCC_LESS) and "scc" not in defs and not op.startswith("s_cbranch"):
        defs.append("scc")
    if op in ("v_add_co_ci_u32", "v_sub_co_ci_u32", "v_subrev_co_ci_u32",
              "v_addc_co_u32", "v_subb_co_u32", "v_subbrev_co_u32",
              "v_cndmask_b32", "v_addc_u32", "v_subb_u32") and len(srcs) == 2:
        uses.append("vcc")
    if op in ("v_div_fmas_f32", "v_div_fmas_f64") and "vcc" not in [r for x in srcs for r in _regs(x)]:
        uses.append("vcc")
    if op in ("s_cselect_b32", "s_cselect_b64", "s_addc_u32", "s_subb_u32", "s_cmov_b32", "s_cmov_b64",
              "s_cmovk_i32", "s_cbranch_scc0", "s_cbranch_scc1"):
        uses.append("scc")
    if op in ("s_cbranch_vccz", "s_cbranch_vccnz"):
        uses.append("vcc")
    if op in MOVREL_OPS or op.startswith("s_sendmsg"):
        uses.append("m0")
    return dests, srcs, defs, uses


class Analysis:
    def __init__(self, blocks, succ, masks, stage, group=None, lin=()):
        self.blocks, self.succ, self.masks, self.stage = blocks, succ, masks or {}, stage
        # compute and mesh: each 32-lane half runs as its own subgroup, but lanes keep their wave numbers 0..63 (the
        # translator takes the lane from the relabelled thread index), so a lane id is exact there
        self.compute = stage in ("cs", "ms")
        self.group = group                          # invocations per workgroup (compute and mesh)
        self.lin_entry = frozenset(lin)             # VGPRs that hold D + lane at entry (D a multiple of 64)
        self.oh_region = frozenset()                # blocks only the half holding a one-half EXEC runs (compute)
        self.nonempty = {}                          # exec token -> blocks where no running half has it empty
        self.wide = {("e",)}                        # exec tokens that hold every launch lane (the entry EXEC, its WQM)
        self.n = len(blocks)
        self.vgprs = sorted({r[1] for b in blocks for ins in b for x in ins["args"]
                             for r in _regs(x) if isinstance(r, tuple) and r[0] == "v"})
        self.tok = {("e",): (None, None, True)}      # exec token -> (parent token, exec tag after, exec is the entry exec)
        self.stale = set()
        self.voted = {}                              # block -> mask register key the terminating branch votes on
        self.branch_class = {}
        self.pure_spill_vgprs = self._find_pure_spill_vgprs()

    def _find_pure_spill_vgprs(self):
        """VGPRs used ONLY as constant-lane scalar spill storage (v_writelane_b32 / v_readlane_b32 with immediate
        lane 0..63 and non-VGPR source, never read/written as a vector operand, never aliased by v_movrel*, and
        never holding a lane mask). Every constant lane 0..63 of such a VGPR can be lowered to a parked scalar slot
        in rdna2spirv.py so SGPR spills past lane 31 stay exact on 32-wide (and narrower) subgroups."""
        candidates = set()
        disqualified = set()
        movrel_min = None
        for blk in self.blocks:
            for ins in blk:
                op = base_op(ins["op"])
                a = ins["args"]
                if op in ("v_movrels_b32", "v_movreld_b32", "v_movrelsd_b32", "v_movrelsd_2_b32", "v_swaprel_b32"):
                    for x in a:
                        if x.get("k") == "vgpr":
                            movrel_min = x["r"] if movrel_min is None else min(movrel_min, x["r"])
                if op == "v_writelane_b32" and len(a) == 3 and a[0].get("k") == "vgpr" and a[0].get("n", 1) == 1 \
                        and a[2].get("k") == "imm" and a[1].get("k") != "vgpr":
                    vr = a[0]["r"]
                    slot = lane_masks.lane_slot(a[0], a[2])
                    if slot and (slot in self.masks.get(id(ins), {}) or self.is_mask(ins, a[1])):
                        disqualified.add(vr)
                    else:
                        candidates.add(vr)
                    continue
                if op == "v_readlane_b32" and len(a) == 3 and a[1].get("k") == "vgpr" and a[1].get("n", 1) == 1 \
                        and a[2].get("k") == "imm":
                    vr = a[1]["r"]
                    slot = lane_masks.lane_slot(a[1], a[2])
                    if slot and slot in self.masks.get(id(ins), {}):
                        disqualified.add(vr)
                    else:
                        candidates.add(vr)
                    continue
                for x in a:
                    for r in _regs(x):
                        if isinstance(r, tuple) and r[0] == "v":
                            disqualified.add(r[1])
        if movrel_min is not None:
            candidates = {vr for vr in candidates if vr < movrel_min}
        return frozenset(candidates - disqualified)

    # ---- helpers ----
    def is_mask(self, ins, a):
        t = a.get("t", "") if a.get("k") == "special" else ""
        if t.startswith("exec"):
            return True
        return lane_masks.kind(self.masks.get(id(ins), {}), a) in lane_masks.MASK_KINDS

    def rd(self, st, ins, a):
        """Taint of an operand; stale scalar reads add CTRL."""
        t = 0
        for r in _regs(a):
            t |= st["t"].get(r, 0)
            if (id(ins), r) in self.stale:
                t |= CTRL
        return t

    LMOD_KEEP = ("v_mov_b32", "v_add_nc_u32", "v_add_u32", "v_sub_nc_u32", "v_subrev_nc_u32", "v_sub_u32",
                 "v_subrev_u32", "v_xor_b32", "v_or_b32", "v_and_b32")

    def lmod(self, st, ins, a):
        """k when the operand is the same in the wave and in a half modulo 2**k: 32 for a partition-invariant value
        (an immediate, an untainted register), the tag's k for a lane id, -1 for anything else."""
        if a.get("k") in ("imm", "fimm"):
            return 32
        if self.is_mask(ins, a) or a.get("neg") or a.get("abs"):
            return -1
        regs = _regs(a)
        if not regs or self.rd(st, ins, a):
            if regs and self.rd(st, ins, a) == LANE and all(st["g"].get(r, ("",))[0] == "lmod" for r in regs):
                return min(st["g"][r][1] for r in regs)
            return -1
        return 32

    def lmod_result(self, st, ins, op, a):
        """k of a VALU result whose only partition-dependent source is a lane id exact modulo 2**k: sums,
        differences and bitwise ops with invariant values keep k, a left shift by an immediate c raises it to k + c."""
        # SDWA selects, DPP, op_sel and integer clamp change the value: no tag
        mods = ins.get("mods") or []
        if ins["op"].endswith(("_sdwa", "_dpp")) or any(
                m == "clamp" or m.startswith(("op_sel", "dst_unused")) or "_sel:" in m for m in mods):
            return None
        src = a[1:]
        ks = [self.lmod(st, ins, x) for x in src]
        if not src or min(ks) < 0:
            return None
        if op in self.LMOD_KEEP:
            return min(ks)
        if op == "v_lshlrev_b32" and len(src) == 2 and src[0].get("k") == "imm":
            return min(32, ks[1] + (src[0]["v"] & 31))
        if op in ("v_lshl_add_u32", "v_lshl_or_b32") and len(src) == 3 and src[1].get("k") == "imm":
            return min(32, ks[0] + (src[1]["v"] & 31), ks[2])
        return None

    def linear_first_use(self, st, ins, op, srcs):
        """Compute/mesh: whether an op whose only partition-dependent source is the first-lane read of D + lane (tag
        "linf": D a multiple of 64 the same in the whole wave, plus an offset below 64) keeps only D: a copy (the tag
        goes on), a right shift by 6 or more, an AND with a constant whose low 6 bits are clear, a bit-field extract
        from bit 6 up. Such a value is the wave base: mesh shaders take their group index as the first lane's v5 >> 7
        (v5 = group * 128 + thread), compute shaders a wave index as their first lane's thread id >> 6."""
        mods = ins.get("mods") or []
        if ins["op"].endswith(("_sdwa", "_dpp")) or any(m == "clamp" or m.startswith(("op_sel", "omod")) for m in mods):
            return False

        def linf(x):
            return x.get("k") in ("sgpr", "vgpr") and not x.get("neg") and not x.get("abs") and len(_regs(x)) == 1 \
                and st["g"].get(_regs(x)[0]) == ("linf",) and st["t"].get(_regs(x)[0], 0) == LINF \
                and (id(ins), _regs(x)[0]) not in self.stale

        def imm(x):
            return x.get("v") & 0xFFFFFFFF if x.get("k") == "imm" else None
        if op in ("s_mov_b32", "v_mov_b32") and len(srcs) == 1:
            return linf(srcs[0])
        if len(srcs) != 2:
            return False
        if op == "s_lshr_b32":
            return linf(srcs[0]) and imm(srcs[1]) is not None and imm(srcs[1]) & 31 >= 6
        if op == "v_lshrrev_b32":
            return linf(srcs[1]) and imm(srcs[0]) is not None and imm(srcs[0]) & 31 >= 6
        if op in ("s_and_b32", "v_and_b32"):
            return any(linf(x) and imm(y) is not None and imm(y) & 63 == 0 for x, y in (srcs, srcs[::-1]))
        if op in ("s_bfe_u32",):
            return linf(srcs[0]) and imm(srcs[1]) is not None and imm(srcs[1]) & 31 >= 6
        return False

    def linear_result(self, st, ins, op, a):
        """Compute/mesh: whether a VALU result is again D + lane with D a multiple of 64 the same in the whole wave
        (tag "lin"), from one lane-linear source: a copy, (wave-uniform value << 6 or more) + it or | it, it + or | a
        constant whose low 6 bits are clear. The mesh entry forms its thread index so: v_lshl_add_u32 v0, s1, 6, v0
        with s1 the wave id and v0 the lane id."""
        mods = ins.get("mods") or []
        if ins["op"].endswith(("_sdwa", "_dpp")) or any(
                m == "clamp" or m.startswith(("op_sel", "dst_unused", "omod")) or "_sel:" in m for m in mods):
            return False
        src = a[1:]

        def lin(x):
            return x.get("k") == "vgpr" and x.get("n", 1) == 1 and not x.get("neg") and not x.get("abs") \
                and st["g"].get(("v", x["r"])) == ("lin",) and not self.rd(st, ins, x)

        def same(x):    # a value that is the same in every lane of the wave and partition invariant
            if x.get("k") == "imm":
                return True
            if self.is_mask(ins, x) or x.get("neg") or x.get("abs") or self.rd(st, ins, x):
                return False
            return x.get("k") == "sgpr" or (x.get("k") == "vgpr" and all(r[1] in st["u"] for r in _regs(x)))

        def k64(x):
            return x.get("k") == "imm" and x["v"] & 63 == 0
        if op == "v_mov_b32" and len(src) == 1:
            return lin(src[0])
        if op in ("v_lshl_add_u32", "v_lshl_or_b32") and len(src) == 3:
            return same(src[0]) and src[1].get("k") == "imm" and src[1]["v"] & 31 >= 6 and lin(src[2])
        if op in ("v_add_nc_u32", "v_add_u32", "v_or_b32") and len(src) == 2:
            return any(lin(x) and k64(y) for x, y in (src, src[::-1]))
        return False

    def join(self, a, b, blk=0):
        t = dict(a["t"])
        for k, v in b["t"].items():
            t[k] = t.get(k, 0) | v
        g = {k: v for k, v in a["g"].items() if b["g"].get(k) == v}
        for k, v in t.items():
            if v & TAG_BITS and k not in g and (k in a["g"] or k in b["g"] or v & TAG_BITS):
                t[k] = v | X
        x = a["x"] if a["x"] == b["x"] else ("j", blk)
        self.tok.setdefault(x, (None, None, False))
        u = a["u"] & b["u"]
        ua = (a["ua"] & b["ua"]) if a["x"] == b["x"] else u
        return {"t": t, "g": g, "x": x, "xg": a["xg"] if a["xg"] == b["xg"] else None, "f": a["f"] and b["f"],
                "u": u, "ua": ua, "l": {k: v for k, v in a["l"].items() if b["l"].get(k) == v},
                "r": self.union_map(a["r"], b["r"]), "b": self.union_map(a["b"], b["b"])}

    @staticmethod
    def union_map(a, b):
        return {k: a.get(k, frozenset()) | b.get(k, frozenset()) for k in set(a) | set(b)}

    def entry(self):
        return {"t": {}, "g": {("v", r): ("lin",) for r in self.lin_entry}, "x": ("e",), "xg": None, "f": True,
                "u": frozenset(), "ua": frozenset(), "l": {}, "r": {}, "b": {}}

    # ---- the transfer function ----
    def block(self, n, st, ev):
        st = {"t": dict(st["t"]), "g": dict(st["g"]), "x": st["x"], "xg": st["xg"], "f": st["f"], "u": st["u"],
              "ua": st["ua"], "l": dict(st["l"]), "r": dict(st["r"]), "b": dict(st["b"])}
        for i, ins in enumerate(self.blocks[n]):
            self.step(n, i, ins, st, ev)
        return st

    def step(self, n, i, ins, st, ev):
        """One instruction. Around the transfer function it keeps, per register, the exec tokens of the lane ranks
        (st["r"]) and of the aggregated-atomic bases (st["b"]) its value is made of: a slot address is exact only
        when its rank and its base belong to the same mask."""
        if base_op(ins["op"]) == "v_swap_b32" and len(ins["args"]) == 2:
            r0, r1 = _regs(ins["args"][0]), _regs(ins["args"][1])
            if len(r0) == 1 and len(r1) == 1:
                full = st["f"]
                rk0, rk1 = st["r"].get(r0[0], frozenset()), st["r"].get(r1[0], frozenset())
                bs0, bs1 = st["b"].get(r0[0], frozenset()), st["b"].get(r1[0], frozenset())
                self.new_rank = self.new_base = None
                self.transfer(n, i, ins, st, ev)
                pairs = ((r0[0], rk1, bs1), (r1[0], rk0, bs0)) if full else (
                    (r0[0], rk0 | rk1, bs0 | bs1), (r1[0], rk0 | rk1, bs0 | bs1))
                for r, r_toks, b_toks in pairs:
                    t = st["t"].get(r, 0)
                    for key, bit, toks in (("r", RNK, r_toks), ("b", BASE, b_toks)):
                        kept = toks if t & bit else frozenset()
                        if kept:
                            st[key][r] = kept
                        else:
                            st[key].pop(r, None)
                return
        self.new_rank = self.new_base = None
        op, a = base_op(ins["op"]), ins["args"]
        dests, srcs, _, uses = operands(ins)
        read = [r for x in srcs for r in _regs(x)] + [r for r in uses if r != "exec"]
        if op == "v_movrels_b32" and len(a) > 1:
            src_base = a[1]["r"] if a[1].get("k") == "vgpr" else 0
            read.extend(("v", vr) for vr in self.vgprs if vr >= src_base)
        dyn_v = op in ("v_movreld_b32", "v_movrelsd_b32", "v_movrelsd_2_b32", "v_swaprel_b32") and len(a) >= 2
        if dyn_v:
            dst_base = (min(a[0]["r"], a[1]["r"])
                        if op == "v_swaprel_b32" and a[0].get("k") == "vgpr" and a[1].get("k") == "vgpr"
                        else (a[0]["r"] if a[0].get("k") == "vgpr" else 0))
            if op != "v_movreld_b32":
                src_base = dst_base if op == "v_swaprel_b32" else (a[1]["r"] if a[1].get("k") == "vgpr" else 0)
                read.extend(("v", vr) for vr in self.vgprs if vr >= src_base)
        rk = frozenset().union(*[st["r"].get(r, frozenset()) for r in read])
        bs = frozenset().union(*[st["b"].get(r, frozenset()) for r in read])
        self.transfer(n, i, ins, st, ev)
        if dyn_v:
            for vr in sorted(set(self.vgprs) | {dst_base}):
                if vr < dst_base:
                    continue
                r = ("v", vr)
                t = st["t"].get(r, 0)
                for key, bit, inherited in (("r", RNK, rk), ("b", BASE, bs)):
                    if t & bit and inherited:
                        st[key][r] = st[key].get(r, frozenset()) | inherited
            return
        for d in dests:
            for r in _regs(d):
                if r == "exec":
                    continue
                t = st["t"].get(r, 0)
                for key, bit, own, inherited in (("r", RNK, self.new_rank, rk), ("b", BASE, self.new_base, bs)):
                    toks = (inherited | ({own} if own is not None else frozenset())) if t & bit else frozenset()
                    if toks:
                        st[key][r] = toks
                    else:
                        st[key].pop(r, None)

    def operand_literal(self, n, i, x):
        """The literal value of a scalar source before instruction `i` of block `n`, else None."""
        if x.get("abs") or x.get("neg"):
            return None
        if x.get("k") == "imm":
            return x["v"] & 0xFFFFFFFF
        if x.get("k") == "special" and x.get("t") == "null":
            return 0
        if x.get("k") == "sgpr" and x.get("n", 1) == 1:
            return self.literal_sgpr(n, i, x["r"])
        return None

    def literal_sgpr(self, n, i, r):
        """The 32-bit literal SGPR `r` holds before instruction `i` of block `n`, as rdna2spirv's `sgpr_imm` knows
        it: the value the last writer in this basic block gave it, when that writer is one of the scalar ops below
        and its sources are literals too. None for any other writer, or none in the block (a run-time value).

        MIRROR of rdna2spirv's `sgpr_imm` folding (it cannot be imported here: this analysis runs before the
        translator emits a block, so its `sgpr_imm` is empty): s_mov_b32, s_movk_i32, s_mov_b64 (an immediate or an
        SGPR pair), s_pack_ll/lh/hh_b32_b16, s_bfm_b32, s_not_b32, and the S2 folds s_and_b32, s_lshl_b32,
        s_lshr_b32, s_mul_i32 (not s_or_b32: rdna2spirv does not record it). Not mirrored, so treated as run-time:
        s_abs, s_sext, s_min/max, s_add/sub, s_xor/andn2 family, s_bfe, s_absdiff, s_lshlN_add. Keep the two lists
        in step when rdna2spirv folds another op."""
        for j in range(i - 1, -1, -1):
            ins = self.blocks[n][j]
            if ("s", r) not in [x for d in operands(ins)[0] for x in _regs(d)]:
                continue
            op, a = base_op(ins["op"]), ins["args"]
            if op == "s_mov_b64" and len(a) == 2 and a[0].get("k") == "sgpr" and a[0].get("n") == 2:
                half = r - a[0]["r"]
                if a[1].get("k") == "imm":
                    return (a[1]["v"] & 0xFFFFFFFFFFFFFFFF) >> (32 * half) & 0xFFFFFFFF
                if a[1].get("k") == "sgpr" and a[1].get("n") == 2:
                    return self.literal_sgpr(n, j, a[1]["r"] + half)
                return None
            if not (a and a[0].get("k") == "sgpr" and a[0].get("n", 1) == 1):
                return None
            x = self.operand_literal(n, j, a[1]) if len(a) > 1 else None
            y = self.operand_literal(n, j, a[2]) if len(a) > 2 else None
            if op == "s_mov_b32" and len(a) == 2:
                return x
            if op == "s_movk_i32" and len(a) == 2 and a[1].get("k") == "imm":
                v = a[1]["v"] & 0xFFFF
                return (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
            if op == "s_not_b32" and x is not None:
                return ~x & 0xFFFFFFFF
            if x is None or y is None or len(a) != 3:
                return None
            if op in ("s_pack_ll_b32_b16", "s_pack_lh_b32_b16", "s_pack_hh_b32_b16"):
                lo = x >> 16 if op == "s_pack_hh_b32_b16" else x & 0xFFFF
                hi = (y & 0xFFFF) << 16 if op == "s_pack_ll_b32_b16" else y & 0xFFFF0000
                return lo | hi
            if op == "s_bfm_b32":
                return (((1 << (x & 31)) - 1) << (y & 31)) & 0xFFFFFFFF
            folds = {"s_and_b32": x & y, "s_lshl_b32": x << (y & 31), "s_lshr_b32": x >> (y & 31), "s_mul_i32": x * y}
            return folds[op] & 0xFFFFFFFF if op in folds else None
        return None

    def add_tid_lane(self, n, i, a, mods):
        """LANE when a MUBUF/MTBUF access (vdata, vaddr, V#, ...) has no idxen and its V# word3 is a literal with
        ADD_TID_ENABLE (bit 23) set: the effective index is then the wave lane (idxen ignores the bit), so the
        result or the address is the lane id. A run-time V# cannot be judged here: tiers A and B assume the bit is
        0 there (docs/design.md, SHADER-10). Compute and mesh: the translator's lane is the wave lane, exact."""
        if self.compute or "idxen" in mods or len(a) < 3:
            return 0
        rsrc = a[2]
        if rsrc.get("k") == "sgpr" and rsrc.get("n") == 4:
            w3 = rsrc["r"] + 3
        elif rsrc.get("k") == "slist" and len(rsrc["r"]) == 4 and isinstance(rsrc["r"][3], int):
            w3 = rsrc["r"][3]
        else:
            return 0
        lit = self.literal_sgpr(n, i, w3)
        return LANE if lit is not None and lit >> 23 & 1 else 0

    def transfer(self, n, i, ins, st, ev):
        op, a = base_op(ins["op"]), ins["args"]
        mods = ins.get("mods") or []
        dests, srcs, defs, uses = operands(ins)
        T = st["t"]

        def violation(code):
            if ev is not None:
                ev["viol"].append((code, ins.get("pc"), op))

        def idiom(name):
            if ev is not None:
                ev["idioms"].add(name)

        def src_taint():
            t = 0
            for x in srcs:
                t |= self.rd(st, ins, x)
            for r in uses:
                t |= T.get(r, 0) | (CTRL if (id(ins), r) in self.stale else 0)
            return t

        def put(a_, t, tag=None, uniform=False, uniform_active=False):
            for r in _regs(a_):
                if r == "exec":
                    continue
                T[r] = t
                st["g"].pop(r, None)
                # SHADER-820: a vote's SCC names the mask register it tested (its first SGPR for a pair). A new
                # value in that register (v_cmp into the pair or VCC, s_mov_b64, s_cselect_b64: none of them
                # writes SCC) leaves SCC a vote on the old mask, while a run side narrows EXEC to the new one, so
                # the tag goes. Ops that vote on their own destination set the new tag after this put().
                vote = st["g"].get("scc")
                if vote and vote[0] == "vote" and r != "scc" and (
                        r == vote[1] or (isinstance(vote[1], tuple) and r == (vote[1][0], vote[1][1] + 1))):
                    del st["g"]["scc"]
                if tag is not None:
                    st["g"][r] = tag
                if isinstance(r, tuple) and r[0] == "v":
                    st["u"] = st["u"] | {r[1]} if uniform else st["u"] - {r[1]}
                    st["ua"] = st["ua"] | {r[1]} if (uniform or uniform_active) else st["ua"] - {r[1]}
                    for k in [k for k in st["l"] if k[0] == r[1]]:
                        del st["l"][k]
                    # a waterfall scalar / mask names the VGPR it was read from: once that is rewritten the
                    # equality no longer says "every lane owns this value"
                    for k, v in list(st["g"].items()):
                        if v[0] in ("wfs", "wfmask") and v[1] == r[1]:
                            del st["g"][k]
                            T[k] = T.get(k, 0) | X

        def bits_violation(t, allowed=0):
            bad = t & ~allowed
            for b, name in NAMES.items():
                if bad & b:
                    violation(name)
                    return

        # --- branches
        if op in BRANCHES:
            self.branch(n, ins, op, st, ev)
            return
        if op == "s_endpgm":
            return

        if op.startswith(("ds_append", "ds_consume", "ds_ordered_count", "ds_gws", "ds_add_gs_reg", "ds_sub_gs_reg",
                          "s_setpc", "s_swappc", "s_getpc_dummy")):
            violation("unmodelled-op")        # wave-wide counters and indirect jumps: the count / target is the wave's
            for d in dests:
                put(d, X)
            return

        # --- sinks
        if op.startswith(SINKS) and not op.startswith(("buffer_atomic",)):
            # buffer_store/tbuffer_store/image_store: operand 0 is data, the rest address and descriptor;
            # global_store/flat_store/scratch_store: operand 0 is vaddr, operand 1 is vdata, the rest saddr;
            # ds_write/ds_store: operand 0 is vaddr, the rest is vdata;
            # exp target, v, v, v, v: the first operand is the target (no taint), the rest is data
            addr, data = [], list(srcs)
            if op.startswith(("global_store", "flat_store", "scratch_store")) and len(srcs) >= 2:
                addr, data = [srcs[0]] + list(srcs[2:]), [srcs[1]]
            elif op.startswith(("ds_write", "ds_store")) and len(srcs) >= 2:
                addr, data = [srcs[0]], list(srcs[1:])
            elif op != "exp" and srcs:
                data, addr = [srcs[0]], list(srcs[1:])
            if self.stage == "gs" and op.startswith(("ds_write", "ds_store")):
                # ds_write vaddr, vdata...: in a legacy GS the write is an output slot from its offset alone
                # (rdna2spirv.gs_ds drops the VGPR address, the lane's GSVS area), so only the data is observable
                data, addr = list(srcs[1:]), []
            dt = 0
            for x in data:
                dt |= self.rd(st, ins, x) | (MASKD | X if self.is_mask(ins, x) and x.get("k") != "imm" else 0)
            at = 0
            for x in addr:
                at |= self.rd(st, ins, x)
            if op.startswith(("buffer_store", "tbuffer_store")):
                at |= self.add_tid_lane(n, i, a, mods)
            bits_violation(dt)
            bits_violation(at, BASE | RNK)
            if at & RNK and at & BASE:
                rt = frozenset().union(*[st["r"].get(r, frozenset()) for x in addr for r in _regs(x)])
                bt = frozenset().union(*[st["b"].get(r, frozenset()) for x in addr for r in _regs(x)])
                if len(rt) != 1 or rt != bt:
                    violation("lane-rank")          # the rank counts another mask than the base was allocated for
            if bool(at & RNK) != bool(at & BASE):
                # a slot is the aggregated atomic's base plus the rank in the mask it counted; the rank alone (halves
                # overwrite each other) or the base alone (every lane of the half writes one word) is not
                violation("lane-rank" if at & RNK else "allocation-order")
            if st["xg"] and st["xg"][0] == "elect":
                violation("elected-side-effect")
            if op == "exp" and a and a[0].get("t", "").startswith(("pos", "param", "mrt", "z", "null")):
                pass
            return
        if op.startswith(("buffer_atomic", "global_atomic", "flat_atomic", "ds_add", "ds_sub", "ds_inc", "ds_dec",
                          "ds_min", "ds_max", "ds_and", "ds_or", "ds_xor", "ds_cmpst", "ds_wrxchg", "ds_mskor")):
            if op.startswith(("global_atomic", "flat_atomic")):
                if "glc" in mods:
                    data_op = a[2] if len(a) > 2 else None
                    addr_ops = ([a[1]] if len(a) > 1 else []) + list(a[3:])
                else:
                    data_op = a[1] if len(a) > 1 else None
                    addr_ops = ([a[0]] if a else []) + list(a[2:])
            elif op.startswith("ds_"):
                if "_rtn" in op:
                    data_op = a[2] if len(a) > 2 else None
                    addr_ops = [a[1]] if len(a) > 1 else []
                else:
                    data_op = a[1] if len(a) > 1 else None
                    addr_ops = [a[0]] if a else []
            else:
                data_op = a[0] if a else None
                addr_ops = list(a[1:])
            data_t = self.rd(st, ins, data_op) if data_op is not None else 0
            addr_t = 0
            for x in addr_ops:
                addr_t |= self.rd(st, ins, x)
            if op.startswith("buffer_atomic"):
                addr_t |= self.add_tid_lane(n, i, a, mods)
            elected = bool(st["xg"] and st["xg"][0] == "elect")
            bits_violation(addr_t, ELECT if elected else 0)
            if elected and any(x.get("k") == "vgpr" and not all(
                    r in st["u"] or r in st["ua"] or self.scalar_def_since_exec(n, i, r)
                    for r in range(x["r"], x["r"] + x.get("n", 1))) for x in addr_ops):
                # only the elected lane runs it, at its own address: each half elects another lane, so a per-lane
                # address (an untainted pixel coordinate too) picks another word there and the counter splits
                violation("elected-lane")
            if data_t & CNT:
                dreg = _regs(data_op)[0] if data_op is not None and _regs(data_op) else None
                if not (elected and (op.endswith("add") or op.endswith("add_u32") or op.endswith("_add"))
                        and self.cnt_token(st, dreg) == st["xg"][1]):
                    violation("lane-count")
                elif data_t & ~(CNT | ELECT):
                    bits_violation(data_t, CNT | ELECT)
                else:
                    idiom("aggregated-atomic")
            else:
                bits_violation(data_t, ELECT if elected else 0)
                if elected:
                    violation("elected-side-effect")
            if dests:
                put(dests[0], BASE | (ELECT if elected else 0), ("elect", st["xg"][1]) if elected else None)
            return

        t_src = src_taint()
        # a lane mask read as data (not as a mask operand of a mask instruction, a select or an exec write): on
        # hardware it is the 64 lane bits, here the bits of the lanes of the half
        md = 0
        if not op.startswith(("v_cmp", "s_cmp", "s_bitcmp")) and op not in (
                "v_cndmask_b32", "v_cndmask_b16", "s_bcnt1_i32_b64", "s_ff1_i32_b64", "v_mbcnt_lo_u32_b32",
                "v_mbcnt_hi_u32_b32") and not op.startswith(MASK_BITWISE + SAVEEXEC) and "exec" not in defs:
            for x in srcs:
                if _half(x):
                    violation("mask-half")
                    md |= X
                elif self.is_mask(ins, x) and x.get("k") != "special" or (
                        x.get("k") == "special" and x.get("t") in ("vcc", "exec") and self.is_mask(ins, x)):
                    md |= MASKD | X
        t_src |= md
        if op.startswith(("buffer_load", "tbuffer_load")):
            t_src |= self.add_tid_lane(n, i, a, mods)

        # --- lane masks and counts
        if op in ("s_bcnt1_i32_b64", "s_ff1_i32_b64") and len(a) == 2 and self.is_mask(ins, a[1]):
            if t_src & ~0:
                bits_violation(t_src & ~(ELECT), 0)
            bit = CNT if op.startswith("s_bcnt1") else FF1
            mreg = _regs(a[1])[0] if _regs(a[1]) else None
            if a[1].get("k") == "special" and a[1].get("t", "").startswith("exec"):
                ctok = st["x"]
            else:
                ctok = st["g"].get(mreg, (None, None))[1] if st["g"].get(mreg, ("",))[0] == "mask" else None
            put(a[0], bit | (t_src & ELECT), ("cnt", ctok) if bit == CNT and ctok is not None else None)
            if op == "s_bcnt1_i32_b64":
                T["scc"] = X | (t_src & ELECT)
                st["g"]["scc"] = ("vote", _regs(a[1])[0] if _regs(a[1]) else "exec")
            return
        if op in ("s_bcnt0_i32_b64", "s_bcnt0_i32_b32", "s_bcnt1_i32_b32", "s_ff0_i32_b64", "s_ff0_i32_b32",
                  "s_ff1_i32_b32", "s_flbit_i32_b64", "s_flbit_i32_b32", "s_flbit_i32") and len(a) == 2 \
                and self.is_mask(ins, a[1]):
            put(a[0], X | t_src)
            if op.startswith("s_bcnt"):
                T["scc"] = X | t_src
                st["g"].pop("scc", None)
            return
        if op in ("v_mbcnt_lo_u32_b32", "v_mbcnt_hi_u32_b32") and len(a) == 3:
            m, acc = a[1], a[2]
            tacc = self.rd(st, ins, acc)
            if self.compute and not self.is_mask(ins, m):
                # compute and mesh: the translator counts below the wave lane (the relabelled thread index mod 64),
                # which a half keeps, so a lane id, or a count of the bits of a number, is what the wave computes
                tag = None
                if m.get("k") == "imm" and m["v"] in (-1, 0xFFFFFFFF):
                    if op == "v_mbcnt_lo_u32_b32" and acc.get("k") == "imm" and acc["v"] == 0:
                        tag = ("lane_lo",)
                    elif op == "v_mbcnt_hi_u32_b32" and not tacc and _regs(acc) and \
                            st["g"].get(_regs(acc)[0]) == ("lane_lo",):
                        tag = ("lin",)
                put(a[0], t_src, tag)
                return
            if m.get("k") == "imm" and m["v"] in (-1, 0xFFFFFFFF):
                # v_mbcnt_lo(-1, 0) then v_mbcnt_hi(-1, that) is the lane id: 0..63 in the wave, 0..31 in a half,
                # so the two differ by a multiple of 32 (tag ("lmod", 5)); the low count alone does not (lanes 32..63
                # count 32 in the wave)
                if op == "v_mbcnt_lo_u32_b32" and acc.get("k") == "imm" and acc["v"] == 0:
                    tag = ("lane_lo",)
                elif op == "v_mbcnt_hi_u32_b32" and tacc == LANE and _regs(acc) and \
                        st["g"].get(_regs(acc)[0]) == ("lane_lo",):
                    tag = ("lmod", 5)
                else:
                    tag = None
                put(a[0], LANE | tacc, tag)
            elif self.is_mask(ins, m):
                if m.get("k") == "special" and m.get("t", "").startswith("exec"):
                    token = st["x"]
                else:
                    reg = _regs(m)[0] if _regs(m) else None
                    token = st["g"].get(reg, (None, None))[1] if st["g"].get(reg, ("",))[0] == "mask" else None
                prev = st["g"].get(_regs(acc)[0]) if _regs(acc) else None
                if prev and prev[0] == "rank" and prev[1] != token:
                    token = None
                put(a[0], (RNK | (tacc & ~RNK)) if token is not None else (X | RNK | tacc),
                    ("rank", token) if token is not None else None)
                self.new_rank = token if token is not None else ("unbound", n, i)
            else:
                put(a[0], X | LANE | tacc)
            return
        if op == "s_bfm_b64" and dests:
            # rdna2spirv.py compares per-half wave_lane (0..31) against offset..offset+size, so the upper half
            # produces bits for lanes 0..31 instead of 32..63. Compute and mesh keep the wave lane: exact.
            put(dests[0], t_src | (0 if self.compute else LANE))
            return

        # --- cross-lane reads and writes
        if op == "v_readfirstlane_b32" and len(a) == 2:
            tv = self.rd(st, ins, a[1])
            vr = a[1]["r"] if a[1].get("k") == "vgpr" else None
            if vr is not None and vr in st["u"]:
                idiom("readfirstlane-uniform")
                put(a[0], tv)
            elif vr is not None and vr in st["ua"]:
                # Uniform over active lanes only: if a half has EXEC == 0 here, v_readfirstlane returns lane 0's
                # stale value, so this scalar is only exact while EXEC remains a subset of st["x"].
                idiom("readfirstlane-uniform")
                put(a[0], tv | ACT_RFL, ("act_rfl", st["x"]))
            elif tv & ELECT and st["g"].get(_regs(a[1])[0], ("",))[0] == "elect" \
                    and st["g"][_regs(a[1])[0]][1] == st["x"]:
                idiom("aggregated-atomic")
                put(a[0], (tv & ~ELECT) | BASE)
                self.new_base = st["g"][_regs(a[1])[0]][1]
            elif self.compute and tv == 0 and vr is not None and st["g"].get(("v", vr)) == ("lin",):
                # D + lane read at the first lane: D + an offset below 64 in the wave and in either half (lane 0 of
                # its half when none is on). Only bits 6 and up are D (linear_first_use clears them)
                put(a[0], LINF, ("linf",))
            elif tv == 0 and vr is not None:
                put(a[0], WFS, ("wfs", vr))
            else:
                put(a[0], tv | X)
            return
        if op == "v_readlane_b32" and len(a) == 3:
            vr = a[1]["r"] if a[1].get("k") == "vgpr" else None
            tl = self.rd(st, ins, a[2])
            if vr is not None and vr in st["u"]:
                # Every lane holds the same value in v[vr], so the selected lane index (even if derived from
                # first-lane, lane-count, or lane-id) cannot change the result across 32-lane halves.
                idiom("readlane-uniform")
                if ev is not None:
                    ev["uniform_readlane"].add(id(ins))
                put(a[0], self.rd(st, ins, a[1]))
            elif vr is not None and a[2].get("k") == "imm" and (vr, a[2]["v"] & 63) in st["l"] \
                    and ((a[2]["v"] & 63) < 32 or vr in self.pure_spill_vgprs):
                lane = a[2]["v"] & 63
                spill_t = st["l"][(vr, lane)]
                if (id(ins), ("spill", vr, lane)) in self.stale:
                    spill_t |= CTRL
                put(a[0], spill_t)
                idiom("lane-spill")
                if ev is not None and vr in self.pure_spill_vgprs:
                    ev["scalar_spills"].add((vr, lane))
            else:
                put(a[0], X | self.rd(st, ins, a[1]) | tl)
            return
        if op == "v_writelane_b32" and len(a) == 3:
            vr = a[0]["r"]
            tv, tl = self.rd(st, ins, a[1]), self.rd(st, ins, a[2])
            T[("v", vr)] = T.get(("v", vr), 0) | X
            st["u"] = st["u"] - {vr}
            st["ua"] = st["ua"] - {vr}
            if a[2].get("k") == "imm" and not tl and ((a[2]["v"] & 63) < 32 or vr in self.pure_spill_vgprs):
                lane = a[2]["v"] & 63
                st["l"][(vr, lane)] = tv
                if ev is not None and vr in self.pure_spill_vgprs:
                    ev["scalar_spills"].add((vr, lane))
            else:
                violation("cross-lane")
            return
        if op.startswith(("v_permlane", "ds_bpermute", "ds_permute", "ds_swizzle")) or "_dpp" in ins["op"] \
                or any(m.startswith("row_") or m.startswith("quad_perm") for m in mods):
            bad = any(m.startswith(DPP_BAD) for m in mods)
            put_t = t_src | (X if bad else 0)
            # a wave64 ds_bpermute/ds_permute works on each half (ISA 10.4.4) and uses address bits 2..6 only, so a
            # lane-id address that is the same modulo 128 in the wave and in a half reads/writes the same lane
            if op in ("ds_bpermute_b32", "ds_permute_b32") and len(a) >= 3 and self.lmod(st, ins, a[1]) >= 7:
                put_t = self.rd(st, ins, a[2])
                for r in uses:
                    put_t |= T.get(r, 0) | (CTRL if (id(ins), r) in self.stale else 0)
            for d in dests:
                put(d, put_t)
            return

        # --- exec writes and mask producers
        if op.startswith(SAVEEXEC) or "exec" in defs or any(_regs(d) == ["exec"] for d in dests):
            self.exec_write(n, i, ins, op, a, dests, srcs, st, ev, violation, idiom, t_src)
            return
        if op.startswith("v_cmp") and not op.startswith("v_cmpx"):
            d = dests[0] if dests else {"k": "special", "t": "vcc"}
            tag, tj = self.cmp_tag(st, ins, op, srcs, t_src, idiom)
            if tag is None and self.uniform_cmp(st, ins, srcs, uses, tj):
                tag = UVOTE
            elif tag is None and self.uniform_cmp(st, ins, srcs, uses, tj, active=True):
                # the same compare under a narrowed EXEC: every active lane or none (exact where EXEC is not empty
                # in any half that runs the vote, see nonempty_exec)
                tag = ("uvote_a", st["x"])
            put(d, tj, tag)
            if len(dests) > 1:
                put(dests[1], t_src)
            return
        if op.startswith(("s_cmp", "s_bitcmp")):
            masks_in = [x for x in srcs if self.is_mask(ins, x)]
            if masks_in:
                m = masks_in[0]
                zero = any(x.get("k") == "imm" and x["v"] == 0 for x in srcs)
                if op in ("s_cmp_eq_u64", "s_cmp_lg_u64") and zero and not _half(m):
                    T["scc"] = X | (t_src & ELECT)
                    # cmp_eq: SCC = (mask == 0), the opposite sense of every other vote (SCC = mask != 0)
                    st["g"]["scc"] = ("vote", _regs(m)[0] if _regs(m) else "exec") + \
                        (("empty",) if op == "s_cmp_eq_u64" else ())
                    if t_src & ~ELECT & ~0:
                        bits_violation(t_src & ~ELECT)
                else:
                    T["scc"] = X | MASKD
                    st["g"].pop("scc", None)
                    violation("mask-compare")
                return
            T["scc"] = t_src
            st["g"].pop("scc", None)
            return
        if op.startswith(MASK_BITWISE) and dests and any(self.is_mask(ins, x) for x in srcs):
            if not op.endswith("b64") or any(_half(x) for x in srcs + dests):
                violation("mask-half")
                put(dests[0], t_src | X)
                return
            if any(not self.is_mask(ins, x) and not (x.get("k") == "imm" and x["v"] in (
                    0, -1, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF)) for x in srcs):
                # a lane mask combined with a register that is not one (a user value, a loaded mask): the result is
                # no mask the translator tracks, and its bits are the lanes of the half
                put(dests[0], t_src | X | MASKD)
                T["scc"] = X | MASKD
                st["g"].pop("scc", None)
                return
            tag = None
            if op == "s_mov_b64" and len(srcs) == 1:
                r = _regs(srcs[0])
                if srcs[0].get("k") == "special" and srcs[0].get("t") == "exec":
                    tag = ("mask", st["x"])
                elif r and r[0] in st["g"] and st["g"][r[0]][0] in ("mask", "elect", "wfmask", "uvote", "uvote_a"):
                    tag = st["g"][r[0]]
            elif op in ("s_and_b64", "s_or_b64", "s_xor_b64", "s_andn2_b64", "s_orn2_b64", "s_nand_b64", "s_nor_b64",
                        "s_xnor_b64", "s_not_b64", "s_cselect_b64") and not t_src and all(
                    (_regs(x) == ["exec"] and st["f"]) or (_regs(x) and all(st["g"].get(r) == UVOTE for r in _regs(x)))
                    or (x.get("k") == "imm" and x["v"] in (0, -1, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF))
                    for x in srcs) and any(_regs(x) != ["exec"] for x in srcs):
                # a uniform compare ANDed with EXEC while EXEC is the launch mask (LLVM: s_and_b64 vcc, exec, s[..]);
                # also EXEC or 0 picked by a clean SCC (s_cselect_b64 s[0:1], exec, 0) and any bitwise mix of such
                # all-or-none masks: each is every launch lane or none, in the wave and in each half alike
                tag = UVOTE
            elif not t_src and self.active_all_or_none(st, op, srcs):
                # every active lane or none (see active_all_or_none)
                tag = ("uvote_a", st["x"])
            put(dests[0], t_src, tag)
            if not op.startswith(("s_mov", "s_cselect")):     # those leave SCC as it was: no vote on their dest
                T["scc"] = X
                st["g"]["scc"] = ("vote", _regs(dests[0])[0] if _regs(dests[0]) else "exec")
            return

        # --- register swap and dynamic indexing
        if op == "v_swap_b32" and len(a) == 2:
            r0, r1 = _regs(a[0]), _regs(a[1])
            t0, t1 = self.rd(st, ins, a[0]), self.rd(st, ins, a[1])
            if st["f"] and not (st["xg"] and st["xg"][0] == "elect"):
                g0 = st["g"].get(r0[0]) if r0 else None
                g1 = st["g"].get(r1[0]) if r1 else None
                u0 = bool(r0 and isinstance(r0[0], tuple) and r0[0][0] == "v" and r0[0][1] in st["u"])
                u1 = bool(r1 and isinstance(r1[0], tuple) and r1[0][0] == "v" and r1[0][1] in st["u"])
                ua0 = bool(r0 and isinstance(r0[0], tuple) and r0[0][0] == "v" and r0[0][1] in st["ua"])
                ua1 = bool(r1 and isinstance(r1[0], tuple) and r1[0][0] == "v" and r1[0][1] in st["ua"])
                put(a[0], t1, g1, uniform=u1, uniform_active=ua1)
                put(a[1], t0, g0, uniform=u0, uniform_active=ua0)
            else:
                tb = t0 | t1 | (ELECT if st["xg"] and st["xg"][0] == "elect" else 0)
                put(a[0], tb, uniform=False)
                put(a[1], tb, uniform=False)
            return
        if op in ("s_movrels_b32", "s_movrels_b64") and dests:
            any_s = 0
            for r, tv in T.items():
                if isinstance(r, tuple) and r[0] == "s":
                    any_s |= tv
            if any(isinstance(k, int) and v in lane_masks.MASK_KINDS for k, v in self.masks.get(id(ins), {}).items()):
                any_s |= MASKD | X
            if any_s & TAG_BITS:
                any_s |= X
            put(dests[0], t_src | any_s)
            return
        if op == "v_movrels_b32" and dests:
            src_base = a[1]["r"] if len(a) > 1 and a[1].get("k") == "vgpr" else 0
            any_v = 0
            for r, tv in T.items():
                if isinstance(r, tuple) and r[0] == "v" and r[1] >= src_base:
                    any_v |= tv
            if any(isinstance(k, tuple) and k[0] in ("v", "lane") and k[1] >= src_base and v in lane_masks.MASK_KINDS
                   for k, v in self.masks.get(id(ins), {}).items()):
                any_v |= MASKD | X
            if any_v & TAG_BITS:
                any_v |= X
            if st["xg"] and st["xg"][0] == "elect":
                any_v |= ELECT
            put(dests[0], t_src | any_v, uniform=False)
            return
        if op in ("v_movreld_b32", "v_movrelsd_b32", "v_movrelsd_2_b32", "v_swaprel_b32") and len(a) >= 2:
            dst_base = (min(a[0]["r"], a[1]["r"])
                        if op == "v_swaprel_b32" and a[0].get("k") == "vgpr" and a[1].get("k") == "vgpr"
                        else (a[0]["r"] if a[0].get("k") == "vgpr" else 0))
            any_v = 0
            if op != "v_movreld_b32":
                src_base = dst_base if op == "v_swaprel_b32" else (a[1]["r"] if a[1].get("k") == "vgpr" else 0)
                for r, tv in T.items():
                    if isinstance(r, tuple) and r[0] == "v" and r[1] >= src_base:
                        any_v |= tv
                if any(isinstance(k, tuple) and k[0] in ("v", "lane") and k[1] >= src_base
                       and v in lane_masks.MASK_KINDS for k, v in self.masks.get(id(ins), {}).items()):
                    any_v |= MASKD | X
            wt = t_src | any_v
            if wt & TAG_BITS:
                wt |= X
            if st["xg"] and st["xg"][0] == "elect":
                wt |= ELECT
            st["u"] = frozenset(u for u in st["u"] if u < dst_base)
            st["ua"] = frozenset(u for u in st["ua"] if u < dst_base)
            for k in [k for k in st["l"] if k[0] >= dst_base]:
                del st["l"][k]
            for k, v in list(st["g"].items()):
                if v[0] in ("wfs", "wfmask") and v[1] >= dst_base:
                    del st["g"][k]
                    T[k] = T.get(k, 0) | X
            if wt:
                bits_violation(wt)
                for vr in self.vgprs:
                    if vr >= dst_base:
                        T[("v", vr)] = T.get(("v", vr), 0) | wt
                T[("v", dst_base)] = T.get(("v", dst_base), 0) | wt
            return

        # --- compute/mesh: the wave base of a first-lane read of D + lane (bits 6 and up)
        if self.compute and t_src & LINF and dests and self.linear_first_use(st, ins, op, srcs):
            for d in dests:
                put(d, t_src & ~LINF, ("linf",) if op in ("s_mov_b32", "v_mov_b32") else None)
            if "scc" in defs:
                T["scc"] = t_src & ~LINF
                st["g"].pop("scc", None)
            if op not in ("s_mov_b32", "v_mov_b32"):
                idiom("readfirstlane-wave-base")
            return
        if op.startswith("s_sendmsg") and self.compute:
            # the message's M0 operand (a mesh shader's vertex and primitive counts) must be the wave's
            if T.get("m0", 0) or (id(ins), "m0") in self.stale:
                violation("tainted-message")
            return

        # --- everything else: results depend on the sources
        def scaled_cnt_token():
            """Linear scaling of a lane count by a clean uniform 32-bit stride preserves wave-aggregated atomic
            partition invariance: (c_lo + c_hi) * K == c_lo * K + c_hi * K (mod 2^32).
            Shift ops require the count to be the shifted value operand (never the shift distance), and
            24-bit truncating multiplies (v_mul_u32_u24) are excluded because mod-2^24 truncation is nonlinear."""
            if op in ("s_lshl_b32", "v_lshlrev_b32"):
                if len(srcs) != 2:
                    return None
                val_op, sh_op = (srcs[0], srcs[1]) if op == "s_lshl_b32" else (srcs[1], srcs[0])
                vt = self.rd(st, ins, val_op)
                st_sh = self.rd(st, ins, sh_op)
                if not (vt & CNT) or (vt & ~(CNT | ELECT)):
                    return None
                if st_sh != 0 or self.is_mask(ins, sh_op) or (
                        sh_op.get("k") == "vgpr" and not all(r[1] in st["ua"] for r in _regs(sh_op))):
                    return None
                r = _regs(val_op)
                return self.cnt_token(st, r[0]) if r else None
            if op in ("s_mul_i32", "s_mulk_i32", "v_mul_lo_u32"):
                cnt_toks = []
                for x in srcs:
                    xt = self.rd(st, ins, x)
                    if xt & CNT:
                        r = _regs(x)
                        ct = self.cnt_token(st, r[0]) if r else None
                        if ct is None or (xt & ~(CNT | ELECT)):
                            return None
                        cnt_toks.append(ct)
                    elif xt != 0 or self.is_mask(ins, x) or (
                            x.get("k") == "vgpr" and not all(r[1] in st["ua"] for r in _regs(x))):
                        return None
                if len(cnt_toks) == 1:
                    return cnt_toks[0]
            return None

        if op in ("v_cndmask_b32", "v_cndmask_b16") or op.startswith("v_"):
            tj = t_src
            uniform = False
            uniform_active = False
            if op.startswith("v_") and not op.startswith(("v_interp", "v_mbcnt", "v_readlane", "v_writelane", "v_permlane",
                                                          "v_cndmask")) and dests:
                uniform = st["f"] and all(
                    x.get("k") in ("imm", "fimm") or (x.get("k") == "sgpr" and not self.is_mask(ins, x)) or
                    (x.get("k") == "vgpr" and all(r[1] in st["u"] for r in _regs(x))) for x in srcs) and not uses
                uniform_active = not (st["xg"] and st["xg"][0] == "elect") and all(
                    x.get("k") in ("imm", "fimm") or (x.get("k") == "sgpr" and not self.is_mask(ins, x)) or
                    (x.get("k") == "vgpr" and all(r[1] in st["ua"] for r in _regs(x))) for x in srcs) and not uses
            if op == "v_cndmask_b32" and dests and len(srcs) in (2, 3) and not ins["op"].endswith(("_sdwa", "_dpp")):
                # a select by a wave-uniform lane mask (every launch lane or none, UVOTE: a uniform compare, or EXEC or
                # 0 picked by a clean SCC) between values that are the same in every lane gives every lane one value
                mregs = _regs(srcs[2]) if len(srcs) == 3 else ["vcc"]

                def alike(x, s):
                    return x.get("k") in ("imm", "fimm") or (x.get("k") == "sgpr" and not self.is_mask(ins, x)) or \
                        (x.get("k") == "vgpr" and all(r[1] in s for r in _regs(x)))
                if mregs and all(st["g"].get(r) == UVOTE and not T.get(r, 0) for r in mregs) and \
                        not (st["xg"] and st["xg"][0] == "elect"):
                    uniform = st["f"] and all(alike(x, st["u"]) for x in srcs[:2])
                    uniform_active = all(alike(x, st["ua"]) for x in srcs[:2])
            if op.startswith("v_") and dests:
                # a write of part of the VGPR (SDWA byte/word with UNUSED_PRESERVE, a 16-bit result) keeps the rest
                # of it: uniform only if the whole register already was
                if partial_write(ins, op):
                    if uniform:
                        uniform = all(r[1] in st["u"] for d in dests for r in _regs(d) if isinstance(r, tuple))
                    if uniform_active:
                        uniform_active = all(r[1] in st["ua"] for d in dests for r in _regs(d) if isinstance(r, tuple))
            # a waterfall scalar used by a lane whose v equals it is that lane's own value
            if tj & WFS:
                ok = st["xg"] and st["xg"][0] == "wf"
                good = True
                for x in srcs:
                    for r in _regs(x):
                        g = st["g"].get(r)
                        if T.get(r, 0) & WFS and not (ok and g and g[0] == "wfs" and g[1] == st["xg"][1]):
                            good = False
                tj = tj & ~WFS if good else tj | X
            # an active-EXEC-only v_readfirstlane scalar is exact only on lanes active within its EXEC mask
            if tj & ACT_RFL:
                good = True
                for x in srcs:
                    for r in _regs(x):
                        g = st["g"].get(r)
                        if T.get(r, 0) & ACT_RFL and not (g and g[0] == "act_rfl" and self.ancestor(g[1], st["x"])):
                            good = False
                tj = tj & ~ACT_RFL if good else tj | X
            if tj & X:
                uniform = False
                uniform_active = False
            tag = None
            ctok = None
            if op in ("v_mov_b32", "v_mov_b32_e32") and len(srcs) == 1 and tj & CNT and _regs(srcs[0]):
                ctok = self.cnt_token(st, _regs(srcs[0])[0])
                tag = ("cnt", ctok) if ctok is not None else None
            elif tj & CNT:
                ctok = scaled_cnt_token()
                tag = ("cnt", ctok) if ctok is not None else None
            if tag is None and tj == LANE and len(dests) == 1:
                k = self.lmod_result(st, ins, op, a)
                if k is not None:
                    tag = ("lmod", k)
            if tag is None and self.compute and not tj and len(dests) == 1 and self.linear_result(st, ins, op, a):
                tag = ("lin",)
            if st["xg"] and st["xg"][0] == "elect" and dests:
                tj |= ELECT
                tag = ("elect", st["xg"][1], ctok)
            for d in dests:
                put(d, tj, tag, uniform=uniform and d.get("k") == "vgpr",
                    uniform_active=uniform_active and d.get("k") == "vgpr")
            return
        # scalar ALU, loads, anything unnamed
        tag = None
        own = False
        if op.startswith(("buffer_load", "tbuffer_load", "global_load")) and dests and not t_src and not any(
                m in ("tfe", "lds") for m in mods) and "_d16" not in op and not (st["xg"] and st["xg"][0] == "elect") \
                and not self.after_store(ins):
            # VIctory's uniform data (lower.rs lane_uniform): a load from an address made of values that are the same in
            # every lane (constants, scalars that are no lane mask, uniform VGPRs) gives every lane one value, when no
            # store can have run before it (a store of the other half could land between the two halves' loads)
            def alike(x, s):
                return x.get("k") in ("imm", "fimm", "off") or (x.get("k") == "special" and x.get("t") in (
                    "off", "null")) or (x.get("k") == "sgpr" and not self.is_mask(ins, x)) or \
                    (x.get("k") == "vgpr" and all(r[1] in s for r in _regs(x)))
            if all(alike(x, st["u"]) for x in srcs):
                for d in dests:
                    put(d, 0, uniform=st["f"] and d.get("k") == "vgpr", uniform_active=d.get("k") == "vgpr")
                return
            if all(alike(x, st["ua"]) for x in srcs):
                for d in dests:
                    put(d, 0, uniform_active=d.get("k") == "vgpr")
                return
        if t_src & WFS and t_src & ACT_RFL:
            t_src |= X
        elif t_src & WFS:
            gs = {st["g"].get(r) for x in srcs for r in _regs(x) if T.get(r, 0) & WFS}
            if len(gs) == 1 and None not in gs:
                tag = next(iter(gs))
                if st["xg"] and st["xg"][0] == "wf" and tag == ("wfs", st["xg"][1]) and op.startswith(
                        ("buffer_load", "tbuffer_load", "global_load", "image_load", "image_sample", "image_gather")):
                    # a load in the waterfall body through what the waterfall scalar selects (its descriptor): each
                    # lane runs it in the pass whose value is its own, so the lane's result is the wave's
                    own = True
            else:
                t_src |= X
        elif t_src & ACT_RFL:
            gs = {st["g"].get(r) for x in srcs for r in _regs(x) if T.get(r, 0) & ACT_RFL}
            if len(gs) == 1 and None not in gs:
                g0 = next(iter(gs))
                if g0[0] == "act_rfl" and self.ancestor(g0[1], st["x"]):
                    tag = g0
                else:
                    t_src |= X
            else:
                t_src |= X
        elif op.startswith(("s_mov", "s_load", "s_buffer_load")) and len(srcs) == 1 and _regs(srcs[0]) and \
                _regs(srcs[0])[0] in st["g"] and st["g"][_regs(srcs[0])[0]][0] in ("mask", "elect", "wfmask", "cnt"):
            tag = st["g"][_regs(srcs[0])[0]]
        elif t_src & CNT:
            ctok = scaled_cnt_token()
            if ctok is not None:
                tag = ("cnt", ctok)
        for d in dests:
            if own and d.get("k") in ("vgpr", "vlist"):
                put(d, t_src & ~WFS)
            else:
                put(d, t_src, tag)
        if "scc" in defs:
            T["scc"] = t_src
            st["g"].pop("scc", None)

    @staticmethod
    def cnt_token(st, reg):
        """The exec token whose lanes a register's lane count counted (None: unknown or not a count)."""
        g = st["g"].get(reg)
        if g and g[0] == "cnt":
            return g[1]
        if g and g[0] == "elect" and len(g) > 2:
            return g[2]
        return None

    def active_all_or_none(self, st, op, srcs):
        """Whether a 64-bit mask op gives every lane of the current EXEC or none, from the lanes' own bits of clean
        all-or-none values: EXEC AND (or AND NOT) a mask that is all or none of an EXEC this one is inside (UVOTE, or
        uvote_a of an ancestor token); EXEC or 0 picked by a clean SCC (s_cselect_b64 s[0:1], exec, 0); AND / OR of
        two such masks of the current EXEC."""
        def kind(x):
            if _regs(x) == ["exec"] and not _half(x):
                return "exec"
            if x.get("k") == "imm" and x["v"] == 0:
                return "zero"
            g = st["g"].get(_regs(x)[0]) if _regs(x) else None
            if g == UVOTE or (g and g[0] == "uvote_a" and self.ancestor(g[1], st["x"])):
                return "sub" if not (g and g[0] == "uvote_a" and g[1] == st["x"]) else "same"
            return None
        ks = [kind(x) for x in srcs]
        if None in ks or len(srcs) != 2:
            return False
        if op == "s_cselect_b64":
            return set(ks) <= {"exec", "zero"}
        if op in ("s_and_b64", "s_or_b64") and set(ks) <= {"same", "zero"}:
            return True
        if op == "s_and_b64":
            return {"exec"} <= set(ks) and set(ks) - {"exec"} <= {"sub", "same"} and ks != ["exec", "exec"]
        if op == "s_andn2_b64":
            return ks[0] in ("exec", "same") and ks[1] in ("sub", "same")
        return False

    def uniform_cmp(self, st, ins, srcs, uses, tj, active=False):
        """Whether a v_cmp writes a wave-uniform lane mask: EXEC is the launch mask and every source is the same in
        every lane (an immediate, a scalar that is no lane mask, a VGPR written from those with EXEC the launch mask)
        and carries no taint, so the mask is every launch lane or none, in the wave and in each half alike. A value
        read with v_readfirstlane from a varying VGPR is tainted (waterfall-scalar), so it is not one. `active`: the
        same under any EXEC, with VGPRs the same in every active lane (st["ua"]): every active lane or none."""
        if tj or uses or not (st["f"] or active) or not srcs:
            return False
        same = st["ua"] if active else st["u"]
        for x in srcs:
            k = x.get("k")
            if k in ("imm", "fimm"):
                continue
            if self.rd(st, ins, x) or self.is_mask(ins, x):
                return False
            if k == "sgpr" or (k == "vgpr" and all(r[1] in same for r in _regs(x))):
                continue
            return False
        return True

    def cmp_tag(self, st, ins, op, srcs, t_src, idiom):
        """(tag, taint) of the lane mask a v_cmp / v_cmpx writes. The two recognised comparisons: a waterfall
        scalar against the VGPR it was read from (every lane of the mask owns that value), and a lane rank
        against 0 (the first lane of the mask the rank counts)."""
        T = st["t"]
        tag, tj = None, t_src
        if "_eq_" in op and len(srcs) == 2:
            for sc, vv in ((srcs[0], srcs[1]), (srcs[1], srcs[0])):
                r = _regs(sc)
                g = st["g"].get(r[0]) if r else None
                if g and g[0] == "wfs" and vv.get("k") == "vgpr" and vv["r"] == g[1] and not vv.get("neg"):
                    tag, tj = ("wfmask", g[1]), self.rd(st, ins, vv) | (T.get(r[0], 0) & ~WFS)
                    idiom("waterfall")
                elif g and g[0] == "rank" and sc.get("k") == "vgpr" and vv.get("k") == "imm" and vv["v"] == 0 and                         (T.get(r[0], 0) & ~TAG_BITS) == 0:
                    tag, tj = ("elect", g[1]), ELECT
            if tag is None and self.compute and not t_src and "_eq_" in op and len(srcs) == 2 and \
                    op.startswith(("v_cmp_eq_u32", "v_cmp_eq_i32", "v_cmpx_eq_u32", "v_cmpx_eq_i32")):
                # compute/mesh: D + lane equal to a wave-uniform value holds in one lane of a wave at most (a mesh
                # shader's v_cmpx_eq_u32 0, v0 picks thread 0): the mask is inside one half
                for lv, other in ((srcs[0], srcs[1]), (srcs[1], srcs[0])):
                    if lv.get("k") == "vgpr" and lv.get("n", 1) == 1 and not lv.get("neg") and not lv.get("abs") \
                            and st["g"].get(("v", lv["r"])) == ("lin",) and not self.is_mask(ins, other) \
                            and not other.get("neg") and not other.get("abs") and (
                                other.get("k") in ("imm", "sgpr") or (other.get("k") == "vgpr" and all(
                                    r[1] in st["u"] for r in _regs(other)))):
                        tag = ("onehalf",)
                        break
        return tag, tj

    def exec_write(self, n, i, ins, op, a, dests, srcs, st, ev, violation, idiom, t_src):
        T = st["t"]
        site = ("n", n, i)
        old = st["x"]
        if any(_half(x) for x in dests + srcs) or (op.startswith("s_") and op.endswith("_b32")):
            violation("mask-half")
        if any(op.startswith(p) for p in SAVEEXEC):
            m = srcs[0] if srcs else None
            mt = self.rd(st, ins, m) if m else 0
            mg = st["g"].get(_regs(m)[0]) if m and _regs(m) else None
            is_wrexec = "_wrexec_" in op
            narrowing = op.startswith(("s_and_saveexec_", "s_andn1_saveexec_", "s_and_wrexec_", "s_andn1_wrexec_"))
            pos_mask = op.startswith(("s_and_saveexec_", "s_and_wrexec_"))
            self.new_exec(site, old, st, narrowing, mg if pos_mask else None, mt, violation, idiom)
            if dests:
                saved_tok = site if is_wrexec else old
                for r in _regs(dests[0]):
                    T[r] = 0
                    st["g"][r] = ("mask", saved_tok)
            T["scc"] = X
            st["g"]["scc"] = ("vote", "exec")
            return
        if a and _regs(a[0]) == ["exec"] or (not dests and "exec" in operands(ins)[2]):
            # s_mov / s_and / s_or / s_xor / s_andn2 ... with EXEC as the destination, or v_cmpx
            srcregs = [x for x in srcs if _regs(x) != ["exec"]]
            x0 = srcregs[0] if srcregs else None
            if op.startswith("v_cmpx"):
                mg, mt = self.cmp_tag(st, ins, "v_cmp" + op[len("v_cmpx"):], srcs, t_src, idiom)
                if mg:
                    mt = 0
            else:
                mg = st["g"].get(_regs(x0)[0]) if len(srcregs) == 1 and x0 is not None and _regs(x0) else None
                mt = 0
                for x in srcregs:
                    mt |= self.rd(st, ins, x)
            if self.compute and op == "s_mov_b64" and len(a) == 2 and a[1].get("k") == "imm" and \
                    a[1]["v"] in (-1, 0xFFFFFFFFFFFFFFFF) and self.group and self.group % 64 == 0:
                # compute/mesh with whole waves: every lane of the wave is a launched invocation, so EXEC = -1 is the
                # launch mask again (a mesh entry starts with it)
                st["x"], st["xg"], st["f"] = ("e",), None, True
                st["ua"] = st["u"]
                return
            if (op == "s_mov_b64" or (op == "s_or_b64" and any(_regs(x) == ["exec"] for x in srcs))) \
                    and mg and mg[0] == "mask" and (op == "s_mov_b64" or self.ancestor(mg[1], old)):
                st["x"] = mg[1]
                st["xg"], st["f"] = self.tok.get(mg[1], (None, None, False))[1:]
                st["ua"] = st["u"]
                if op == "s_or_b64":                            # s_mov_b64 leaves SCC as it was
                    T["scc"] = X
                    st["g"]["scc"] = ("vote", "exec")
                return
            if op.startswith("v_cmpx"):
                self.new_exec(site, old, st, True, mg, mt, violation, idiom)
            elif op == "s_and_b64" and any(_regs(x) == ["exec"] for x in srcs):
                self.new_exec(site, old, st, True, mg, mt, violation, idiom)
            elif op == "s_andn2_b64" and len(srcs) == 2 and _regs(srcs[0]) == ["exec"]:
                self.new_exec(site, old, st, True, None, mt, violation, idiom)
            else:
                if old in self.wide and op in ("s_wqm_b64", "s_or_b64") and any(
                        _regs(x) == ["exec"] and not _half(x) for x in srcs):
                    self.wide.add(site)     # whole quads of, or more than, an EXEC that holds the launch lanes
                st["x"] = site
                self.tok[site] = (None, None, False)
                st["xg"], st["f"] = None, False
                st["ua"] = st["u"]
                if mt & ~0:
                    for b, name in NAMES.items():
                        if mt & b and not (b & ELECT):
                            violation("tainted-exec")
                            break
            if op.startswith("s_") and op != "s_mov_b64":
                T["scc"] = X
                st["g"]["scc"] = ("vote", "exec")
            return
        st["x"] = site
        self.tok[site] = (None, None, False)
        st["xg"], st["f"] = None, False
        st["ua"] = st["u"]

    def scalar_def_since_exec(self, n, i, r):
        """Whether VGPR r was last written in block n before instruction i, after the block's last EXEC write, from
        scalars and constants only (the elected lane computed it itself from wave-uniform values)."""
        for j in range(i - 1, -1, -1):
            ins = self.blocks[n][j]
            dests, srcs, defs, _ = operands(ins)
            if ins["op"].startswith(SAVEEXEC) or "exec" in defs:
                return False
            if any(("v", r) in _regs(d) for d in dests):
                return all(x.get("k") in ("sgpr", "imm") or (x.get("k") == "special" and x.get("t") not in (
                    "exec", "exec_lo", "exec_hi", "vcc", "vcc_lo", "vcc_hi")) for x in srcs)
        return False

    def ancestor(self, anc, tok):
        seen = set()
        while tok is not None and tok not in seen:
            if tok == anc:
                return True
            seen.add(tok)
            tok = self.tok.get(tok, (None,))[0]
        return False

    def new_exec(self, site, old, st, narrowing, mg, mt, violation, idiom):
        """EXEC becomes a subset of the old EXEC (narrowing) restricted by mask `mg` of taint `mt`."""
        xg = st["xg"] if narrowing else None
        if mg and mg[0] == "elect" and mg[1] == old and narrowing:
            xg = ("elect", old)
            idiom("elected-lane")
        elif mg and mg[0] == "wfmask" and narrowing:
            xg = ("wf", mg[1])
        elif mg and mg[0] == "onehalf" and narrowing:
            xg = ("onehalf",)       # EXEC inside one half of the wave (compute/mesh, see oh_regions)
        else:
            bad = mt & ~0
            if bad:
                for b, name in NAMES.items():
                    if bad & b:
                        violation("tainted-exec")
                        break
        self.tok[site] = (old if narrowing else None, xg, st["f"] and False)
        st["x"], st["xg"], st["f"] = site, xg, False
        if not narrowing:
            st["ua"] = st["u"]

    def branch(self, n, ins, op, st, ev):
        T = st["t"]
        if op == "s_branch":
            return
        if op in ("s_cbranch_scc0", "s_cbranch_scc1"):
            t, g = T.get("scc", 0) | (CTRL if (id(ins), "scc") in self.stale else 0), st["g"].get("scc")
            if g and g[0] == "vote":
                t &= ~X           # SCC of a vote carries X for every other reader; this branch is the vote's consumer
            if t & ~ELECT:
                self.bad_branch(ev, ins, op, t)
                return
            if g and g[0] == "vote":
                if g[1] != "exec" and (st["g"].get(g[1]) == UVOTE and not T.get(g[1], 0) and (id(ins), g[1]) not in
                                       self.stale or self.nonempty_vote(n, st["g"].get(g[1]), T.get(g[1], 0),
                                                                        (id(ins), g[1]) in self.stale)):
                    return      # SCC of an all-or-none mask: the same in the wave and in each half, no lane vote
                # the vote is yes (some lane in the mask) when SCC = 1, or SCC = 0 for an "empty" test
                self.vote(n, ins, op, g[1], (op == "s_cbranch_scc1") != (g[2:] == ("empty",)), ev, None)
            return
        reg = "vcc" if "vcc" in op else "exec"
        t = (T.get(reg, 0) | (CTRL if (id(ins), "vcc") in self.stale else 0)) if reg == "vcc" else 0
        if t & ~ELECT:
            self.bad_branch(ev, ins, op, t)
            return
        self.vote(n, ins, op, reg, op.endswith("nz"), ev, st)

    def bad_branch(self, ev, ins, op, t):
        if ev is not None:
            for b, name in NAMES.items():
                if t & b:
                    ev["viol"].append((name if b != X else "tainted-branch", ins.get("pc"), op))
                    return
            ev["viol"].append(("tainted-branch", ins.get("pc"), op))

    def vote(self, n, ins, op, reg, on_taken, ev, st):
        """A wave-wide vote on mask register `reg` decides the branch. EXEC's own vote is always a skip of
        lane-masked work; for any other mask the side that runs when the vote is yes must start by narrowing EXEC
        to that mask, so lanes that voted no are inert there.

        A VCC that is a wave-uniform compare (UVOTE: every launch lane or none) is no lane vote: each half votes as
        the wave does, so either side may have code of its own and scalars it defines are not stale. EXEC must have
        been the launch mask at the compare: under a narrowed EXEC a half with no active lane votes no while the
        wave votes yes."""
        # st only for s_cbranch_vccz/vccnz: an SCC vote was taken on the mask as it was then, and VCC may since hold
        # another value (a later v_cmp writes no SCC)
        if reg == "vcc" and st is not None and st["g"].get("vcc") == UVOTE and not st["t"].get("vcc", 0) \
                and (id(ins), "vcc") not in self.stale:
            return
        if reg == "vcc" and st is not None and self.nonempty_vote(n, st["g"].get("vcc"), st["t"].get("vcc", 0),
                                                                  (id(ins), "vcc") in self.stale):
            return
        if n in self.oh_region and self.within_exec(n, reg):
            # compute/mesh: only the half that holds the one-half EXEC runs this block, on a mask inside EXEC: its
            # vote is the wave's (oh_regions)
            return
        self.voted[n] = reg
        if n + 1 > self.n:
            return
        targets = self.succ[n]
        if not targets:
            return
        run = targets[0] if on_taken else (targets[1] if len(targets) > 1 else None)
        skip = targets[1] if on_taken and len(targets) > 1 else (targets[0] if not on_taken else None)
        if reg == "exec":
            # The side that runs when EXEC is zero is taken by a half with no active lane, not only by a wave with
            # none, and then runs its scalar work and effects while the other half does not. So it must be the join
            # (or only jumps to it). The exit of a loop closed by the vote is the join of every lane: allowed.
            if run is None or skip is None or (self.reaches(run, n, -1) and not self.reaches(skip, n, -1)) \
                    or self.reaches(run, self.past_jumps(skip), n):
                return
            if ev is not None:
                ev["viol"].append(("unsafe-execz-branch", ins.get("pc"), op))
            return
        # On hardware nobody runs the other side when some lane votes yes; a half whose lanes all vote no does. So
        # that side must be no code of its own: the join, which the run side reaches again.
        ok = run is not None and self.narrows(run, reg) and skip is not None \
            and self.reaches(run, self.past_jumps(skip), n)
        # The early exit of a pixel kill: the side a half takes when its mask is empty only empties EXEC (from that
        # mask or 0), may export with that EXEC and ends, so it writes nothing; the run side narrows EXEC into the
        # mask's whole quads and never widens it again, so in the wave the lanes of that half stay off to the end.
        # Any lane mask would do; the vote must be the mask's own "not empty" (checked from the SCC writer), not a
        # stale or other SCC.
        # A pixel's fate is its EXEC bit at the last `exp ... vm` it ran (else at the end), so no such export may run
        # before the vote: one that did would keep the pixels of the empty half alive in the wave.
        kregs = self.kill_vote(n, op, reg)
        if not ok and kregs and run is not None and skip is not None and self.inert_exit(skip, kregs) \
                and self.confined(run, kregs) and not self.vm_export_before(n):
            ok = True
        if not ok and ev is not None:
            ev["viol"].append(("unsafe-vote-branch", ins.get("pc"), op))

    def kill_vote(self, n, op, reg):
        """Registers of the mask whose emptiness the branch ending block `n` tests (SCC or VCC = mask != 0), or None
        when that is not plain: the last SCC writer must set SCC = (its 64-bit mask destination != 0), and nothing
        after it in the block may write that mask."""
        if op in ("s_cbranch_vccz", "s_cbranch_vccnz"):
            return ("vcc",) if reg == "vcc" else None
        body = self.blocks[n][:-1]
        for k in range(len(body) - 1, -1, -1):
            ins = body[k]
            o, a = base_op(ins["op"]), ins["args"]
            dests, _, defs, _ = operands(ins)
            if "scc" not in defs and not o.startswith(("s_cmp", "s_bitcmp")):
                continue
            if o in ZERO_SCC and len(a) >= 2 and _mask64(a[0]):
                m = tuple(_regs(a[0]))
            elif o == "s_cmp_lg_u64" and len(a) == 2 and _mask64(a[0]) and a[1].get("k") == "imm" and a[1]["v"] == 0:
                m = tuple(_regs(a[0]))
            else:
                return None
            if m[0] != reg or m == ("exec",):
                return None
            for later in body[k + 1:]:
                d, _, df, _ = operands(later)
                if set(m) & ({r for x in d for r in _regs(x)} | set(df)):
                    return None
            return m
        return None

    def vm_export_before(self, n):
        """Whether an `exp ... vm` is in a block from which block `n` can be reached (`n` itself included)."""
        pred = {}
        for b, ss in enumerate(self.succ):
            for s_ in ss:
                pred.setdefault(s_, []).append(b)
        seen, work = set(), [n]
        while work:
            b = work.pop()
            if b in seen:
                continue
            seen.add(b)
            if any(base_op(i["op"]) == "exp" and "vm" in (i.get("mods") or []) for i in self.blocks[b]):
                return True
            work.extend(pred.get(b, []))
        return False

    def inert_exit(self, blk, m):
        """Whether `blk` (through plain jumps) only sets EXEC to a subset of mask `m` (or to 0), then may export
        with that EXEC, and ends: nothing else, no other successor."""
        seen = set()
        narrowed = False
        while blk not in seen:
            seen.add(blk)
            for ins in self.blocks[blk]:
                op, a = base_op(ins["op"]), ins["args"]
                if op in ("s_nop", "s_waitcnt", "s_branch"):
                    continue
                if op == "s_endpgm":
                    return narrowed
                if op == "exp" and narrowed:
                    continue
                if a and _regs(a[0]) == ["exec"] and not _half(a[0]) and (
                        (op == "s_mov_b64" and len(a) == 2 and (tuple(_regs(a[1])) == m or _zero(a[1]))) or
                        (op == "s_and_b64" and len(a) == 3 and any(tuple(_regs(x)) == m or _zero(x) for x in a[1:]))):
                    narrowed = True
                    continue
                return False
            if len(self.succ[blk]) != 1:
                return False
            blk = self.succ[blk][0]
        return False

    def confined(self, run, m):
        """Whether, on every path from block `run` (loops included), EXEC is narrowed into the whole quads of mask
        `m` before anything but mask arithmetic runs, and stays inside them: then a lane of a half whose `m` is empty
        is off for the rest of the wave. A must-dataflow over the blocks `run` reaches: (EXEC inside, set of 64-bit
        registers that hold a subset); a write to EXEC keeps it inside only when it is provably a subset (a narrowing,
        or a copy / union of registers known to be inside)."""
        region, work = set(), [run]
        while work:
            b = work.pop()
            if b not in region:
                region.add(b)
                work.extend(self.succ[b])
        entry = {run: (False, frozenset({m}))}
        work = [run]
        while work:
            b = work.pop()
            inside, sub = entry[b]
            for ins in self.blocks[b]:
                if not inside and base_op(ins["op"]) not in OFF_OK:
                    return False
                inside, sub = self.confine_step(ins, inside, sub)
            for s in self.succ[b]:
                new = (inside, sub) if s not in entry else (entry[s][0] and inside, entry[s][1] & sub)
                if new != entry.get(s):
                    entry[s] = new
                    work.append(s)
        return True

    @staticmethod
    def confine_step(ins, inside, sub):
        op, a = base_op(ins["op"]), ins["args"]
        dests, srcs, defs, _ = operands(ins)

        def ok(x):
            return _zero(x) or (inside and _regs(x) == ["exec"] and not _half(x)) or tuple(_regs(x)) in sub

        def combine(o, xs):
            if o in ("s_mov_b64", "s_wqm_b64") and len(xs) == 1:
                return ok(xs[0])
            if o == "s_and_b64" and len(xs) == 2:
                return ok(xs[0]) or ok(xs[1])
            if o == "s_andn2_b64" and len(xs) == 2:
                return ok(xs[0])
            if o in ("s_or_b64", "s_xor_b64") and len(xs) == 2:
                return ok(xs[0]) and ok(xs[1])
            return False

        written = {r for x in dests for r in _regs(x)} | set(defs)
        new_sub = {k for k in sub if not set(k) & written}
        old_inside = inside
        if "exec" in written:
            if op.startswith("v_cmpx"):
                pass                                            # EXEC &= compare: a narrowing
            elif op.startswith(SAVEEXEC) and len(a) == 2 and not _half(a[1]):
                x = a[1]
                if op.startswith(("s_and_saveexec_b64", "s_and_wrexec_b64")):
                    inside = old_inside or ok(x)
                elif op.startswith(("s_andn1_saveexec_b64", "s_andn1_wrexec_b64")):
                    inside = old_inside
                elif op.startswith(("s_andn2_saveexec_b64", "s_andn2_wrexec_b64")):
                    inside = ok(x)
                elif op.startswith(("s_or_saveexec_b64", "s_xor_saveexec_b64")):
                    inside = old_inside and ok(x)
                else:
                    inside = False
                if _mask64(a[0]):
                    keep = inside if "_wrexec_" in op else old_inside
                    if keep:
                        new_sub.add(tuple(_regs(a[0])))
            elif dests and _regs(dests[0]) == ["exec"] and not _half(dests[0]):
                inside = combine(op, srcs)
            else:
                inside = False
        elif dests and _mask64(dests[0]) and combine(op, srcs):
            new_sub.add(tuple(_regs(dests[0])))
        return inside, frozenset(new_sub)

    def past_jumps(self, blk):
        """`blk`, or where it leads when it holds nothing but jumps and waits (the layout of a vccnz skip)."""
        seen = set()
        while blk not in seen and len(self.succ[blk]) == 1 and all(
                base_op(i["op"]) in ("s_branch", "s_nop", "s_waitcnt") for i in self.blocks[blk]):
            seen.add(blk)
            blk = self.succ[blk][0]
        return blk

    def reaches(self, start, goal, avoid):
        """Whether `goal` can be reached from `start` without passing through block `avoid` (start == goal counts)."""
        seen, work = set(), [start]
        while work:
            b = work.pop()
            if b == goal:
                return True
            if b in seen or b == avoid:
                continue
            seen.add(b)
            work.extend(self.succ[b])
        return False

    def narrows(self, blk, reg):
        for ins in self.blocks[blk]:
            op = base_op(ins["op"])
            if op in ("s_waitcnt", "s_nop"):
                continue
            a = ins["args"]
            if op.startswith(("s_and_saveexec_b64", "s_and_wrexec_b64")) and len(a) == 2:
                return (_regs(a[1]) or [None])[0] == reg
            if op in ("s_and_b64",) and len(a) == 3 and _regs(a[0]) == ["exec"]:
                return reg in [(_regs(x) or [None])[0] for x in a[1:] if _regs(x) != ["exec"]]
            if op == "s_mov_b64" and len(a) == 2 and _regs(a[0]) == ["exec"]:
                return (_regs(a[1]) or [None])[0] == reg
            return False
        return False

    def within_exec(self, n, reg):
        """Whether the mask a branch ending block `n` votes on is inside EXEC: EXEC itself, or written in block `n` by
        a compare (VOPC clears the bits of the lanes that are off) or an AND with EXEC, and not written again."""
        if reg == "exec":
            return True
        key = reg if reg == "vcc" else reg
        for ins in reversed(self.blocks[n][:-1]):
            op, a = base_op(ins["op"]), ins["args"]
            dests, srcs, defs, _ = operands(ins)
            written = [r for d in dests for r in _regs(d)] + [r for r in defs if r != "scc"]
            if key not in written:
                continue
            if op.startswith("v_cmp") and not op.startswith("v_cmpx"):
                return True
            if op == "s_and_b64" and any(_regs(x) == ["exec"] and not _half(x) for x in srcs):
                return True
            if op == "s_andn2_b64" and srcs and _regs(srcs[0]) == ["exec"] and not _half(srcs[0]):
                return True
            return False
        return False

    def oh_regions(self, entry):
        """Compute/mesh: the blocks only one half of a wave runs. When EXEC is inside one half (a lane-linear id
        compared equal to a wave-uniform value: at most one lane per wave) and an s_cbranch_execz skips the region
        that follows, the other half, whose EXEC is zero, skips it, and the wave (whose active lanes are all in the
        half that runs it) does the same work in it. The region is what the fall-through reaches before the jump's
        target, entered only from the branch and left only to that target, and EXEC stays inside the half there."""
        out = set()
        if not self.compute:
            return frozenset()
        preds = [[] for _ in range(self.n)]
        for b, ss in enumerate(self.succ):
            for s_ in ss:
                preds[s_].append(b)
        for n in range(self.n):
            blk = self.blocks[n]
            if not blk or entry[n] is None or base_op(blk[-1]["op"]) != "s_cbranch_execz" or len(self.succ[n]) != 2:
                continue
            if self.block(n, entry[n], None)["xg"] != ("onehalf",):
                continue
            skip, first = self.succ[n]
            region, work = set(), [first]
            while work:
                b = work.pop()
                if b in region or b == skip:
                    continue
                if b == n:
                    region = None
                    break
                region.add(b)
                work.extend(self.succ[b])
            if not region or preds[first] != [n] or any(p not in region for b in region - {first} for p in preds[b]):
                continue
            if any(entry[b] is None or not self.stays_onehalf(b, entry[b]) for b in region):
                continue
            out |= region
        return frozenset(out)

    def nonempty_exec(self, entry):
        """{exec token: blocks where every half that runs them has an active lane in that EXEC}: the blocks the
        fall-through of an s_cbranch_execz on it dominates, when that fall-through has no other way in. A half whose
        EXEC is empty takes the jump. A compare of uniform values made under such an EXEC (or one it is inside) is
        every active lane or none, so a vote on it is the wave's in every half that gets there."""
        out = {}
        preds = [[] for _ in range(self.n)]
        for b, ss in enumerate(self.succ):
            for s_ in ss:
                preds[s_].append(b)
        dom = None
        for n in range(self.n):
            blk = self.blocks[n]
            if not blk or entry[n] is None or base_op(blk[-1]["op"]) != "s_cbranch_execz" or len(self.succ[n]) != 2:
                continue
            first = self.succ[n][1]
            if preds[first] != [n]:
                continue
            if dom is None:
                dom = self.dominators(preds)
            tok = self.block(n, entry[n], None)["x"]
            out.setdefault(tok, set()).update(b for b in range(self.n) if dom[b] >> first & 1)
        return out

    def dominators(self, preds):
        full = (1 << self.n) - 1
        dom = [full] * self.n
        if self.n:
            dom[0] = 1
        changed = True
        while changed:
            changed = False
            for b in range(1, self.n):
                new = full
                for p in preds[b]:
                    new &= dom[p]
                new = (new if preds[b] else 0) | 1 << b
                if new != dom[b]:
                    dom[b], changed = new, True
        return dom

    def nonempty_vote(self, n, tag, taint, stale):
        """Whether a vote in block `n` on a mask tagged `tag` is the wave's in every half: a compare of uniform values
        under an EXEC that is not empty in any half there (uvote_a, nonempty_exec)."""
        if not tag or tag[0] != "uvote_a" or taint or stale:
            return False
        if tag[1] in self.wide:
            return True             # an EXEC holding every launch lane: not empty in any half that exists
        return any(n in blocks and self.ancestor(tag[1], tok) for tok, blocks in self.nonempty.items())

    def stays_onehalf(self, b, st):
        """Whether EXEC is inside one half at the entry of block `b` and after every EXEC write in it."""
        if st["xg"] != ("onehalf",):
            return False
        st = {"t": dict(st["t"]), "g": dict(st["g"]), "x": st["x"], "xg": st["xg"], "f": st["f"], "u": st["u"],
              "ua": st["ua"], "l": dict(st["l"]), "r": dict(st["r"]), "b": dict(st["b"])}
        for i, ins in enumerate(self.blocks[b]):
            self.step(b, i, ins, st, None)
            if st["xg"] != ("onehalf",):
                return False
        return True

    def control(self):
        """(cd, ctrl): cd[b] = the branch blocks block `b` is control dependent on (post-dominance on the CFG with one
        exit node), ctrl[b] = whether `b` depends, directly or through other branches, on a wave-wide vote."""
        n = self.n
        exit_ = n
        succ = [list(s) for s in self.succ] + [[]]
        for b in range(n):
            if not succ[b]:
                succ[b] = [exit_]
        full = (1 << (n + 1)) - 1
        pdom = [full] * (n + 1)
        pdom[exit_] = 1 << exit_
        changed = True
        while changed:
            changed = False
            for b in range(n - 1, -1, -1):
                new = full
                for s in succ[b]:
                    new &= pdom[s]
                new |= 1 << b
                if new != pdom[b]:
                    pdom[b], changed = new, True

        def ipdom(b):
            strict = pdom[b] & ~(1 << b)
            for c in range(n + 1):
                if strict >> c & 1 and pdom[c] == strict:
                    return c
            return None
        ip = {b: ipdom(b) for b in range(n)}
        cd = {b: set() for b in range(n)}           # b -> branch blocks it is control dependent on
        cd_edge = {b: set() for b in range(n)}      # b -> (branch block, successor) edges it is control dependent on
        for p in range(n):
            if len(succ[p]) < 2:
                continue
            for s in succ[p]:
                y = s
                while y is not None and y != ip[p] and y != exit_:
                    cd[y].add(p)
                    cd_edge[y].add((p, s))
                    y = ip[y]
        ctrl = {b: False for b in range(n)}
        changed = True
        while changed:
            changed = False
            for b in range(n):
                if not ctrl[b] and any(p in self.voted or ctrl[p] for p in cd[b]):
                    ctrl[b], changed = True, True
        # the vote edges (vote block, the successor it takes) each block depends on, directly or through branches
        self.votes_of = {b: frozenset() for b in range(n)}
        changed = True
        while changed:
            changed = False
            for b in range(n):
                new = frozenset(e for e in cd_edge[b] if e[0] in self.voted).union(
                    *(self.votes_of[p] for p in cd[b]))
                if new != self.votes_of[b]:
                    self.votes_of[b], changed = new, True
        return cd, ctrl

    @staticmethod
    def mem_access(ins):
        """(domain, reads, writes, scalar) of a memory instruction: domain "lds" (LDS data, not the cross-lane
        ds_swizzle / ds_(b)permute) or "mem" (buffer, global, flat and image memory, scalar loads); None otherwise.
        Scratch is the lane's own and not counted."""
        op = base_op(ins["op"])
        if op.startswith("ds_"):
            if op.startswith(("ds_swizzle", "ds_bpermute", "ds_permute", "ds_nop")):
                return None
            if op.startswith(("ds_read", "ds_load")):
                return ("lds", True, False, False)
            if op.startswith(("ds_write", "ds_store")):
                return ("lds", False, True, False)
            return ("lds", True, True, False)              # atomics, cmpst, wrxchg, mskor, append/consume
        if op.startswith(("s_load", "s_buffer_load")):
            return ("mem", True, False, True)
        if op.startswith(("s_store", "s_buffer_store", "s_atomic", "s_buffer_atomic")):
            return ("mem", False, True, True)
        if op.startswith(("buffer_load", "tbuffer_load", "global_load", "flat_load", "image_load", "image_sample",
                          "image_gather", "image_get", "image_bvh")):
            return ("mem", True, False, False)
        if op.startswith(("buffer_store", "tbuffer_store", "global_store", "flat_store", "image_store")):
            return ("mem", False, True, False)
        if op.startswith(("buffer_atomic", "global_atomic", "flat_atomic", "image_atomic")):
            return ("mem", True, True, False)
        return None

    def after_store(self, ins):
        """Whether a memory write (vector or scalar store or atomic, not LDS or scratch) can run before `ins` with no
        s_barrier in between (compute and mesh; there is none in the other stages)."""
        if not hasattr(self, "_after_store"):
            self._after_store = set()
            entry = [None] * self.n
            if self.n:
                entry[0] = False
            work = [0] if self.n else []

            def through(b, dirty, record):
                for x in self.blocks[b]:
                    if record and dirty:
                        self._after_store.add(id(x))
                    if base_op(x["op"]) == "s_barrier" and self.compute:
                        dirty = False
                        continue
                    m = self.mem_access(x)
                    if m and m[0] == "mem" and m[2]:
                        dirty = True
                return dirty
            while work:
                b = work.pop()
                out = through(b, entry[b], False)
                for s in self.succ[b]:
                    new = out if entry[s] is None else (entry[s] or out)
                    if new != entry[s]:
                        entry[s] = new
                        work.append(s)
            for b in range(self.n):
                if entry[b] is not None:
                    through(b, entry[b], True)
        return id(ins) in self._after_store

    def compute_checks(self, ev):
        """Compute and mesh: what the two halves of a wave, run as two subgroups, must also agree on besides their
        values (docs/design.md, "Wave64 compute and mesh: partition invariance").

        * Barriers. With no memory write in the program (a mesh shader's exports are not memory) an s_barrier
          orders nothing the program can observe, and natively it is left out (`drop_barriers`). Otherwise both
          halves must reach the same s_barriers: none may be in a block a vote decides (vote-barrier; a half whose
          lanes all voted no skips it, the wave does not), and a mesh shader, which has no barrier rounds, keeps
          them where every invocation runs (nested-barrier).
        * Memory order inside the wave. The lanes of a wave run each instruction together, so an LDS write is seen
          by every later LDS read of the wave without a barrier (the compiler leaves them out in a one-wave group).
          Two subgroups give no such order: an LDS access after a conflicting one (RAW, WAR, WAW) on a path with no
          s_barrier between them needs a workgroup barrier right before it (`sync`). That is only possible in a
          block every invocation runs once (not control dependent on any branch), else lds-in-wave. The same for a
          scalar load after a vector store (mem-in-wave): every lane reads the one value, whichever lane wrote it.
          Vector accesses with per-lane addresses, and a store after a scalar load, are taken to touch no word
          another lane of the wave reads or writes without a barrier between (an assumption, as for ADD_TID_ENABLE;
          VIctory orders LDS only).
        * The group size: a group over 32 invocations whose size is not a multiple of 32 could have a partial
          subgroup in the middle, which breaks the relabelling (group-size)."""
        viol = ev["viol"]
        cd, ctrl = self.control()
        writes = any(m and m[2] for blk in self.blocks for m in map(self.mem_access, blk))
        group = self.group
        if group is not None and group > 32 and group % 32:
            viol.append(("group-size", None, "group"))
        ev["drop_barriers"] = not writes
        if writes:
            for b, blk in enumerate(self.blocks):
                for ins in blk:
                    if base_op(ins["op"]) != "s_barrier":
                        continue
                    if ctrl[b]:
                        viol.append(("vote-barrier", ins.get("pc"), "s_barrier"))
                    elif self.stage == "ms" and cd[b]:
                        viol.append(("nested-barrier", ins.get("pc"), "s_barrier"))
        if group is not None and group <= 32:
            return                                      # one subgroup holds the whole wave: lockstep as on hardware
        # barrier-free reachability of earlier accesses, per domain; a sync point acts as a barrier for later ones
        sync = set()
        for _ in range(4 * self.n + 8):
            new = self.order_hazards(sync)
            if new <= sync:
                break
            sync |= new
        for b, blk in enumerate(self.blocks):
            for ins in blk:
                if id(ins) in sync and cd[b]:
                    m = self.mem_access(ins)
                    viol.append(("lds-in-wave" if m and m[0] == "lds" else "mem-in-wave", ins.get("pc"),
                                 base_op(ins["op"])))
        ev["sync"] = sync

    def order_hazards(self, sync):
        """The accesses (id(ins)) that follow a conflicting access of the other half on a path with no barrier: in
        LDS any two of which one writes, in memory a scalar load after a write (and anything around a scalar store).
        `sync` holds accesses a barrier is already put in front of."""
        empty = frozenset()
        entry = [None] * self.n
        if self.n:
            entry[0] = empty
        work = [0] if self.n else []
        hazards = set()

        def through(b, pend, record):
            for ins in self.blocks[b]:
                if base_op(ins["op"]) == "s_barrier":
                    pend = empty
                    continue
                m = self.mem_access(ins)
                if not m:
                    continue
                dom, rd, wr, sc = m
                if dom == "lds":
                    clash = any(p[0] == "lds" and (wr or p[2]) for p in pend)
                else:
                    # a scalar load after a write: every lane reads the one word, whichever lane wrote it; scalar
                    # stores order against everything
                    clash = any(p[0] == "mem" and ((sc and rd and p[2]) or (sc and wr) or (p[3] and p[2]))
                                for p in pend)
                if id(ins) in sync or clash:
                    if record and clash:
                        hazards.add(id(ins))
                    pend = empty                        # the barrier put in front of it orders LDS and memory
                pend = pend | {m}
            return pend
        while work:
            b = work.pop()
            out = through(b, entry[b], False)
            for s in self.succ[b]:
                new = out if entry[s] is None else entry[s] | out
                if new != entry[s]:
                    entry[s] = new
                    work.append(s)
        for b in range(self.n):
            if entry[b] is not None:
                through(b, entry[b], True)
        return hazards

    # ---- fixpoint ----
    def run(self, stale):
        self.stale = stale
        self.voted = {}
        entry = [None] * self.n
        if self.n:
            entry[0] = self.entry()
        work = [0] if self.n else []
        while work:
            n = work.pop()
            out = self.block(n, entry[n], None)
            for s in self.succ[n]:
                new = out if entry[s] is None else self.join(entry[s], out, s)
                if entry[s] is None or new != entry[s]:
                    entry[s] = new
                    work.append(s)
        self.oh_region = self.oh_regions(entry)
        self.nonempty = self.nonempty_exec(entry)
        ev = {"viol": [], "idioms": set(), "uniform_readlane": set(), "scalar_spills": set(), "sync": set(),
              "drop_barriers": False}
        self.voted = {}
        for n in range(self.n):
            if entry[n] is not None:
                self.block(n, entry[n], ev)
        return ev

    def waterfall_remaining(self):
        """{id(ins): registers} of the waterfall form that keeps the lanes still to do in a mask register: a loop
        block that starts with v_readfirstlane_b32 s, v and v_cmpx_eq (s, v) (or v_cmp_eq into VCC and
        s_and_saveexec_b64 of it) and ends with s_andn2_wrexec_b64 m, m and s_cbranch_execnz back to itself, m set to
        EXEC before it (s_mov_b64 m, exec, or a saveexec's destination). The loop removes each lane once, when the
        value read is its own, so a lane's bit of m is the same in the wave and in a half at every pass that lane
        sees; the read of m by the wrexec is not a stale scalar, though m is redefined in the loop a vote closes."""
        out = {}
        for b, blk in enumerate(self.blocks):
            body = [x for x in blk if base_op(x["op"]) not in ("s_nop", "s_waitcnt")]
            if len(body) < 4 or base_op(body[-1]["op"]) != "s_cbranch_execnz" or self.succ[b][:1] != [b]:
                continue
            w, rfl = body[-2], body[0]
            wa, ra = w["args"], rfl["args"]
            if base_op(w["op"]) != "s_andn2_wrexec_b64" or len(wa) != 2 or not (_mask64(wa[0]) and wa[0] == wa[1]):
                continue
            if base_op(rfl["op"]) != "v_readfirstlane_b32" or len(ra) != 2 or ra[1].get("k") != "vgpr":
                continue
            sreg, vreg = _regs(ra[0]), _regs(ra[1])
            c = body[1]
            co, ca = base_op(c["op"]), c["args"]
            pair = lambda xs: {tuple(_regs(x)) for x in xs} == {tuple(sreg), tuple(vreg)}   # noqa: E731
            if co.startswith("v_cmpx_eq_") and len(ca) == 2 and pair(ca):
                pass
            elif co.startswith("v_cmp_eq_") and len(ca) == 3 and pair(ca[1:]) and base_op(body[2]["op"]) == \
                    "s_and_saveexec_b64" and _regs(body[2]["args"][1]) == _regs(ca[0]):
                pass
            else:
                continue
            m = tuple(_regs(wa[0]))
            if any(set(m) & {r for d in operands(x)[0] for r in _regs(d)} for x in body[:-2]):
                continue                            # m is the wrexec's alone in the loop
            preds = [p for p in range(self.n) if b in self.succ[p] and p != b]
            ok = bool(preds)
            for p in preds:
                last = next((x for x in reversed(self.blocks[p]) if set(m) & {r for d in operands(x)[0]
                                                                             for r in _regs(d)}), None)
                lo, la = (base_op(last["op"]), last["args"]) if last else (None, [])
                if not (last and ((lo == "s_mov_b64" and len(la) == 2 and _regs(la[1]) == ["exec"]) or
                                  lo.startswith(SAVEEXEC))):
                    ok = False
            if ok:
                out[id(w)] = set(m)
        return out

    # ---- control dependence on wave-wide votes ----
    def stale_uses(self):
        n = self.n
        if not n or not self.voted:
            return set()
        _, ctrl = self.control()
        if not any(ctrl.values()):
            return set()
        # reaching definitions of the scalar registers
        defs_of = {}
        for b, blk in enumerate(self.blocks):
            for i, ins in enumerate(blk):
                dests, srcs, d2, uses = operands(ins)
                regs = [r for d in dests for r in _regs(d) if r != "exec" and r[0] != "v"] if True else []
                regs = [r for d in dests for r in _regs(d) if r != "exec" and not (isinstance(r, tuple) and r[0] == "v")]
                regs += [r for r in d2 if r not in ("exec",)]
                if any(_half(d) and d["t"].startswith("vcc") for d in dests):
                    regs.append("vcc")
                if base_op(ins["op"]) == "v_writelane_b32" and len(ins["args"]) == 3 \
                        and ins["args"][0].get("k") == "vgpr" and ins["args"][0]["r"] in self.pure_spill_vgprs \
                        and ins["args"][2].get("k") == "imm":
                    regs.append(("spill", ins["args"][0]["r"], ins["args"][2]["v"] & 63))
                defs_of[(b, i)] = regs

        def sregs(x):
            return [r for r in _regs(x) if r != "exec" and not (isinstance(r, tuple) and r[0] == "v")]
        entry = [None] * n
        entry[0] = {}
        work = [0]
        while work:
            b = work.pop()
            st = dict(entry[b])
            for i, ins in enumerate(self.blocks[b]):
                for r in defs_of[(b, i)]:
                    st[r] = frozenset({(b, i)})
            for s in self.succ[b]:
                if entry[s] is None:
                    entry[s] = dict(st)
                    work.append(s)
                else:
                    merged, grew = dict(entry[s]), False
                    for r, v in st.items():
                        old = merged.get(r, frozenset({(-1, 0)}))
                        new = old | v
                        if new != merged.get(r):
                            merged[r], grew = new, True
                    for r in entry[s]:
                        if r not in st:
                            old = merged[r]
                            new = old | frozenset({(-1, 0)})
                            if new != old:
                                merged[r], grew = new, True
                    if grew:
                        entry[s] = merged
                        work.append(s)
        stale = set()
        carried = self.waterfall_remaining()
        loops = frozenset(v for v in self.voted if any(self.reaches(s_, v, -1) for s_ in self.succ[v]))
        preds = [[] for _ in range(n)]
        for p_, ss in enumerate(self.succ):
            for s_ in ss:
                preds[s_].append(p_)
        dom = self.dominators(preds)

        def behind(e, b):
            """Whether every way to block b takes vote edge e = (vote block, successor), a vote that closes no loop."""
            v, s_ = e
            return v not in loops and preds[s_] == [v] and dom[b] >> s_ & 1
        for b in range(n):
            if entry[b] is None:
                continue
            st = dict(entry[b])
            for i, ins in enumerate(self.blocks[b]):
                dests, srcs, d2, uses = operands(ins)
                read = {r for x in srcs for r in sregs(x)} | {r for r in uses if r != "exec"}
                if id(ins) in carried:
                    read -= carried[id(ins)]
                if base_op(ins["op"]).startswith(("buffer_atomic", "global_atomic")) or dests and False:
                    read |= {r for x in ins["args"] for r in sregs(x)}
                if base_op(ins["op"]) == "v_readlane_b32" and len(ins["args"]) == 3 \
                        and ins["args"][1].get("k") == "vgpr" and ins["args"][1]["r"] in self.pure_spill_vgprs \
                        and ins["args"][2].get("k") == "imm":
                    read.add(("spill", ins["args"][1]["r"], ins["args"][2]["v"] & 63))
                for r in read:
                    rd = st.get(r, frozenset({(-1, 0)}))
                    # stale: a def behind a vote edge the read does not depend on may not have run in one half
                    # (where a vote that closes no loop went one way for both, every half that reads ran that side's
                    # code as the wave did; in a loop a vote closes, the halves run different numbers of passes)
                    if len(rd) > 1 and any(d[0] >= 0 and ctrl[d[0]] and
                                           not all(behind(e, b) for e in self.votes_of[d[0]]) for d in rd):
                        stale.add((id(ins), r))
                if base_op(ins["op"]) in ("s_movrels_b32", "s_movrels_b64") and any(
                        isinstance(r, tuple) and r[0] == "s" and len(rd) > 1 and any(d[0] >= 0 and ctrl[d[0]] for d in rd)
                        for r, rd in st.items()):
                    stale.add((id(ins), "m0"))
                for r in defs_of[(b, i)]:
                    st[r] = frozenset({(b, i)})
        return stale


def analyse(blocks, succ, masks, stage, group=None, lin=()):
    """Tier of one wave64 program (see the module docstring). VS/PS/GS: `stage` alone. CS/MS also take `group`, the
    invocations per workgroup, and `lin`, the VGPRs that hold D + lane at entry with D a multiple of 64 (a mesh
    shader's v5, a compute shader's v0 when the group's x size is a multiple of 64); the result then also has `sync`
    (id(ins) of the accesses a workgroup barrier goes in front of) and `drop_barriers`."""
    an = Analysis(blocks, succ, masks, stage, group, lin)
    stale = set()
    ev = an.run(stale)
    for _ in range(3):
        new = an.stale_uses()
        if new == stale:
            break
        stale = new
        ev = an.run(stale)
    if an.compute:
        an.compute_checks(ev)
    reasons = sorted({c for c, _, _ in ev["viol"]})
    idioms = sorted(ev["idioms"])
    if ev["sync"]:
        idioms = sorted(set(idioms) | {"wave-memory-order"})
    tier = "C" if reasons else ("B" if idioms else "A")
    return {"tier": tier, "reasons": reasons, "idioms": idioms, "uniform_readlane": ev["uniform_readlane"],
            "scalar_spills": sorted(ev["scalar_spills"]), "sites": ev["viol"][:8], "sync": ev["sync"],
            "drop_barriers": ev["drop_barriers"]}


# ---- census over a shader index ----
def census(index):
    """Counts only: {stage: {tier: n}} and the reason / idiom counts of the wave64 entries of an index.json: VS/PS/GS
    from `wave64_graphics`, CS/MS from `wave64_compute` (written under --wave64-emulate only: a CS/MS without it is
    left out, not counted as no_analysis)."""
    tiers, reasons, idioms, unmarked = {}, Counter(), Counter(), 0
    for name, m in index.items():
        if not isinstance(m, dict) or not m.get("wave64") or m.get("stage") not in ("vs", "ps", "gs", "cs", "ms"):
            continue
        key = "wave64_compute" if m["stage"] in ("cs", "ms") else "wave64_graphics"
        t = m.get(key)
        if t is None:
            unmarked += m["stage"] not in ("cs", "ms")
            continue
        tiers.setdefault(m["stage"], Counter())[t] += 1
        reasons.update(m.get(key + "_reasons", []))
        idioms.update(m.get(key + "_idioms", []))
    return {"tiers": {s: dict(sorted(c.items())) for s, c in sorted(tiers.items())},
            "reasons": dict(reasons.most_common()), "idioms": dict(idioms.most_common()), "no_analysis": unmarked}


def main(argv=None):
    ap = argparse.ArgumentParser(description="Tier census of the wave64 VS/PS/GS (and, translated with "
                                             "--wave64-emulate, CS/MS) shaders of a translator index.json (counts "
                                             "only, no shader names)")
    ap.add_argument("--census", required=True, metavar="INDEX_JSON", help="<spirv>/index.json from rdna2spirv.py")
    args = ap.parse_args(argv)
    # One line per bad input, as everywhere else in tools/: an unreadable path, a folder, a file that is not JSON and a
    # JSON document that is not the index object used to end in a traceback (FileNotFoundError, PermissionError,
    # JSONDecodeError, AttributeError).
    try:
        with open(args.census, encoding="utf-8-sig") as f:
            index = json.load(f)
    except OSError as exc:
        print(f"ERROR: cannot read {args.census}: {exc.strerror or exc}", file=sys.stderr)
        return 2
    except ValueError as exc:  # json.JSONDecodeError
        print(f"ERROR: {args.census} is not JSON: {exc}", file=sys.stderr)
        return 2
    if not isinstance(index, dict):
        print(f"ERROR: {args.census} is not an index object (a mapping of shader name to entry), got "
              f"{type(index).__name__}. Give the <spirv>/index.json rdna2spirv.py writes.", file=sys.stderr)
        return 2
    c = census(index)
    total = Counter()
    for stage, counts in c["tiers"].items():
        print(f"{stage}: " + ", ".join(f"{t}={n}" for t, n in counts.items()))
        total.update(counts)
    n = sum(total.values())
    if n:
        print("all: " + ", ".join(f"{t}={k} ({100 * k / n:.1f}%)" for t, k in sorted(total.items())))
    else:
        print("no wave64 VS/PS/GS entry with a wave64_graphics tier")
    if c["no_analysis"]:
        print(f"{c['no_analysis']} wave64 VS/PS/GS entries without a tier (translated before SHADER-10)")
    if c["reasons"]:
        print("tier C reasons: " + ", ".join(f"{k} x{v}" for k, v in c["reasons"].items()))
    if c["idioms"]:
        print("tier B idioms: " + ", ".join(f"{k} x{v}" for k, v in c["idioms"].items()))
    return 0


if __name__ == "__main__":
    try:  # a redirected stdout is strict ANSI code page under -I; write '?' for what it cannot encode
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError, OSError):
        pass
    sys.exit(main())
