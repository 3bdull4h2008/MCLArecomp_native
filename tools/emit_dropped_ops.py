"""Build-time overlay for instructions XenonRecomp left as comments only (F-833 D2, F-921).
usage: emit_dropped_ops.py <generated_dir> <overlay_dir> | --self-test

generated/** stays untouched (AGENTS.md rule 2). Each TU holding a dropped op is copied to
<overlay_dir> with the missing C++ line added, and CMake compiles the copy instead. Prints the
overlaid TU numbers, one per line, on stdout. Forms follow recompiler.cpp's own templates, with
operand order and semantics from Xenia's ppc_emit_altivec.cc.
Left out: `mullhwu.` (one site, not a Xenon opcode, so it needs its own look) and the no-op family
(dcbt, dcbf, lwsync, sync, eieio, traps).
"""
import glob, os, re, sys

EXPECTED = {"frsqrte": 97, "vsel128": 71, "vspltish": 12, "vpkswss128": 11, "vcfpuxws128": 9,
            "vpkuwus128": 4, "vrfip128": 4, "vpkuwum128": 3, "vpkuhum128": 3, "eqv": 1, "mulhdu": 1}
INSN = re.compile(r"^\s*// (" + "|".join(EXPECTED) + r") (\S+)\s*$")
FUNC = re.compile(r"PPC_FUNC_IMPL\(__imp__")
# vpkd3d128 D3DCOLOR clamp: recompiler.cpp emits `X < 3.0f ? 3.0f : (...)`, which lets NaN through (NaN fails both
# compares) and packs 0xFF. The hardware packs NaN as 0 (Xenia x64_seq_vector.cc "pack NaN as zero"); the guest uses
# vpkd3d128 of an all-ones (NaN) vector to build a 0 mask, so the player car collider position became NaN.
# `!(X >= 3.0f)` sends NaN to 3.0f, whose low byte is 0.
BFP = re.compile(r"\t// vcmpbfp128 (v\d+),(v\d+),(v\d+)\r?\n\t__builtin_debugtrap\(\);\r?\n")
MULLHWU = re.compile(r"\t// mullhwu\. r19,r0,r28\r?\n")
UPK8 = re.compile(r"\t// vupkd3d128 (v\d+),(v\d+),8\r?\n\t__builtin_debugtrap\(\);\r?\n")
PK3 = re.compile(r"\t// vpkd3d128 (v\d+),(v\d+),3,1,3\r?\n(?:\tctx\.fpscr\.enableFlushMode(?:Unconditional)?\(\);\r?\n)?\t__builtin_debugtrap\(\);\r?\n")
VUPK20 = re.compile(
    r"(\t// vupkd3d128 [^\r\n]+,20\r?\n"
    r"\t\{ uint32_t h = [\w.]+\.u16\[)4(\];[^\r\n]+\r?\n(?:[^\r\n]+\r?\n){4}"
    r"\t\{ uint32_t h = [\w.]+\.u16\[)5(\];[^\r\n]+\r?\n(?:[^\r\n]+\r?\n){4}"
    r"\t\{ uint32_t h = [\w.]+\.u16\[)6(\];[^\r\n]+\r?\n(?:[^\r\n]+\r?\n){4}"
    r"\t\{ uint32_t h = [\w.]+\.u16\[)7(\];)"
)
PKD3D_22 = re.compile(
    r"(\t// vpkd3d128 (v\d+),v\d+,0,2,2\r?\n"
    r"(?:[^\r\n]+\r?\n){12}"
    r"\t[\w.]+\.u32\[2\] = temp\.u32;\r?\n)"
)
PKD3D_30 = re.compile(
    r"(\t// vpkd3d128 (v\d+),v\d+,0,3,0\r?\n"
    r"(?:[^\r\n]+\r?\n){12}"
    r"\t[\w.]+\.u32\[0\] = temp\.u32;\r?\n)"
)
PKD3D = re.compile(r"([\w.]+\.f32\[\d\]) < 3\.0f \? 3\.0f :")
FCTID = re.compile(r"([\w.]+\.s64 = \([\w.]+\.f64 )> (double\(LLONG_MAX\)\) \? LLONG_MAX : simde_mm_cvtt?sd_si64)")
SRAW = re.compile(
    r"(\t// sraw\.? \S+\r?\n)"
    r"\ttemp\.u32 = ([\w.]+)\.u32 & 0x3F;\r?\n"
    r"\tif \(temp\.u32 > 0x1F\) temp\.u32 = 0x1F;\r?\n"
    r"\txer\.ca = \(([\w.]+)\.s32 < 0\) & \(\(\(\3\.s32 >> temp\.u32\) << temp\.u32\) != \3\.s32\);\r?\n"
    r"\t([\w.]+)\.s64 = \3\.s32 >> temp\.u32;\r?\n"
)
SRAW_REPL = (
    r"\1"
    r"\ttemp.u32 = \2.u32 & 0x3Fu;\n"
    r"\txer.ca = (\3.s32 < 0) && (temp.u32 > 0x1Fu || ((\3.u32 & ((1u << temp.u32) - 1u)) != 0u));\n"
    r"\t\4.s64 = (temp.u32 > 0x1Fu) ? (\3.s32 >> 31) : (\3.s32 >> temp.u32);\n"
)
SRAD = re.compile(
    r"(\t// srad \S+\r?\n)"
    r"\ttemp\.u64 = ([\w.]+)\.u64 & 0x7F;\r?\n"
    r"\tif \(temp\.u64 > 0x3F\) temp\.u64 = 0x3F;\r?\n"
    r"\txer\.ca = \(([\w.]+)\.s64 < 0\) & \(\(\(\3\.s64 >> temp\.u64\) << temp\.u64\) != \3\.s64\);\r?\n"
    r"\t([\w.]+)\.s64 = \3\.s64 >> temp\.u64;\r?\n"
)
SRAD_REPL = (
    r"\1"
    r"\ttemp.u64 = \2.u64 & 0x7Fu;\n"
    r"\txer.ca = (\3.s64 < 0) && (temp.u64 > 0x3Fu || ((\3.u64 & ((1ULL << temp.u64) - 1ULL)) != 0ULL));\n"
    r"\t\4.s64 = (temp.u64 > 0x3Fu) ? (\3.s64 >> 63) : (\3.s64 >> temp.u64);\n"
)
RLWIMI_WRAP = re.compile(r"__builtin_rotateleft32\(([\w.]+)\.u32, (\d+)\) & (0xFFFFFFFF[0-9A-F]{8})\b")

MIDHOOK_SPECS = {
    "ppc_recomp.2.cpp": {
        "decls": [
            'extern "C" bool mcla_poll_wait(uint32_t r31, uint32_t r30, uint32_t r29, uint32_t r26, uint32_t r3);',
        ],
        "hooks": [
            (
                "sub_82148A10",
                "mcla_poll_wait",
                re.compile(
                    r"(\t// li r3,0\r?\n"
                    r"\tctx\.r3\.s64 = 0;\r?\n"
                    r"\t// bl 0x821c91b8\r?\n"
                    r"\tctx\.lr = 0x82148AB8;\r?\n"
                    r"\tsub_821C91B8\(ctx, base\);\r?\n)"
                    r"(\t// lbz r10,199\(r31\)\r?\n)"
                ),
                r"\tif (mcla_poll_wait(r31.u32, r30.u32, r29.u32, r26.u32, ctx.r3.u32)) goto loc_82148AB8;\n\1loc_82148AB8:\n\2",
            ),
        ],
    },
    "ppc_recomp.5.cpp": {
        "decls": [
            'extern "C" void mcla_poll_release(uint32_t r6);',
        ],
        "hooks": [
            (
                "sub_82160088",
                "mcla_poll_release",
                re.compile(
                    r"(\t// stw r11,4\(r6\)\r?\n"
                    r"\tPPC_STORE_U32\(ctx\.r6\.u32 \+ 4, r11\.u32\);\r?\n)"
                ),
                r"\1\tmcla_poll_release(ctx.r6.u32);\n",
            ),
        ],
    },
    "ppc_recomp.52.cpp": {
        "decls": [
            'extern "C" void mcla_cam_boom_scale_outer(double* f13);',
            'extern "C" void mcla_cam_boom_scale_inner(double* f0);',
        ],
        "hooks": [
            (
                "sub_82320298",
                "mcla_cam_boom_scale_outer",
                re.compile(r"(loc_82320468:\r?\n)(\t// addi r9,r1,80\r?\n)"),
                r"\1\tmcla_cam_boom_scale_outer(&ctx.f13.f64);\n\2",
            ),
            (
                "sub_82320298",
                "mcla_cam_boom_scale_inner",
                re.compile(r"(loc_823204F4:\r?\n)(\t// li r9,288\r?\n)"),
                r"\1\tmcla_cam_boom_scale_inner(&f0.f64);\n\2",
            ),
        ],
    },
    "ppc_recomp.56.cpp": {
        "decls": [
            'extern "C" void mcla_bike_gyro_scale_lean(double* f13);',
            'extern "C" void mcla_bike_gyro_scale_damp(double* f0);',
        ],
        "hooks": [
            (
                "sub_823688B8",
                "mcla_bike_gyro_scale_lean",
                re.compile(r"(loc_8236895C:\r?\n)(\t// addi r8,r1,100\r?\n)"),
                r"\1\tmcla_bike_gyro_scale_lean(&ctx.f13.f64);\n\2",
            ),
            (
                "sub_823688B8",
                "mcla_bike_gyro_scale_damp",
                re.compile(r"(loc_823689F0:\r?\n)(\t// addi r9,r1,100\r?\n)"),
                r"\1\tmcla_bike_gyro_scale_damp(&f0.f64);\n\2",
            ),
        ],
    },
    "ppc_recomp.110.cpp": {
        "decls": [
            'extern "C" void mcla_chassis_filter_scale(double* f0);',
        ],
        "hooks": [
            (
                "sub_82563298",
                "mcla_chassis_filter_scale",
                re.compile(r"(loc_82563720:\r?\n)(\t// lis r11,-32256\r?\n)"),
                r"\1\tmcla_chassis_filter_scale(&f0.f64);\n\2",
            ),
        ],
    },
    "ppc_recomp.135.cpp": {
        "decls": [
            'extern "C" void mcla_trigger_line_extend_stack(uint32_t sp, uint32_t trigger, uint32_t racer);',
        ],
        "hooks": [
            (
                "sub_826881A8",
                "mcla_trigger_line_extend_stack",
                re.compile(
                    r"(\t// stvx128 v62,r0,r7\r?\n[^\r\n]+\r?\n)"
                    r"(\t// beq cr6,0x82688218\r?\n)"
                ),
                r"\1\tmcla_trigger_line_extend_stack(ctx.r1.u32, r31.u32, r27.u32);\n\2",
            ),
        ],
    },
    # ---- Wave 3 (2026-11-19) -------------------------------------------------
    # Every pattern below was counted against the actual generated TU with
    # pattern.subn("<probe>", span) == 1 (verify_real_anchors() in self_test).
    # The C++ helpers are defined in src/hooks/wave3_polish.cpp and
    # wave3_guards.cpp; mcla_menu_* belongs to worker 1's wave3_menu.cpp.
    "ppc_recomp.22.cpp": {
        "decls": [
            'extern "C" void mcla_prompts_platform_flag(PPCRegister& node);',
        ],
        "hooks": [
            (
                "sub_821FDED8",
                "mcla_prompts_platform_flag",
                re.compile(
                    r"(\t// bl 0x823dc018\r?\n"
                    r"\tctx\.lr = 0x821FE22C;\r?\n"
                    r"\tsub_823DC018\(ctx, base\);\r?\n)"
                ),
                r"\1\tmcla_prompts_platform_flag(r27);\n",
            ),
        ],
    },
    "ppc_recomp.26.cpp": {
        "decls": [
            'extern "C" void mcla_ui_repeat_start(PPCRegister& dest, PPCRegister& key);',
            'extern "C" void mcla_ui_repeat_gate(PPCRegister& dest, PPCRegister& index, PPCRegister& key, PPCRegister& cur, PPCRegister& prev);',
        ],
        "hooks": [
            (
                "sub_82223CD8",
                "mcla_ui_repeat_start",
                re.compile(
                    r"(\t// stbx r27,r31,r28\r?\n"
                    r"\tPPC_STORE_U8\(r31\.u32 \+ r28\.u32, r27\.u8\);\r?\n"
                    r"\t// cmpwi cr6,r4,-1\r?\n"
                    r"\tcr6\.compare<int32_t>\(ctx\.r4\.s32, -1, xer\);\r?\n"
                    r"\t// stbx r21,r31,r29\r?\n"
                    r"\tPPC_STORE_U8\(r31\.u32 \+ r29\.u32, r21\.u8\);\r?\n"
                    r"\t// stb r22,16\(r26\)\r?\n"
                    r"\tPPC_STORE_U8\(r26\.u32 \+ 16, r22\.u8\);\r?\n)"
                ),
                r"\1\tmcla_ui_repeat_start(r31, ctx.r4);\n",
            ),
            (
                "sub_82223CD8",
                "mcla_ui_repeat_gate",
                re.compile(
                    r"(\t// stbx r8,r31,r29\r?\n"
                    r"\tPPC_STORE_U8\(r31\.u32 \+ r29\.u32, ctx\.r8\.u8\);\r?\n)"
                    r"(\t// cmplw cr6,r10,r8\r?\n"
                    r"\tcr6\.compare<uint32_t>\(ctx\.r10\.u32, ctx\.r8\.u32, xer\);\r?\n)"
                ),
                r"\1\tmcla_ui_repeat_gate(r31, r29, ctx.r4, ctx.r8, ctx.r10);\n\2",
            ),
        ],
    },
    "ppc_recomp.53.cpp": {
        "decls": [
            'extern "C" void mcla_traffic_chassis_bound_8232D048(PPCRegister& root, PPCRegister& child);',
            'extern "C" void mcla_traffic_chassis_bound_8232D900(PPCRegister& root, PPCRegister& child);',
            'extern "C" void mcla_traffic_chassis_bound_8232E274(PPCRegister& root, PPCRegister& child);',
        ],
        "hooks": [
            (
                "sub_8232CFF0",
                "mcla_traffic_chassis_bound_8232D048",
                re.compile(
                    r"(\t// lwz r11,0\(r9\)\r?\n"
                    r"\tr11\.u64 = PPC_LOAD_U32\(ctx\.r9\.u32 \+ 0\);\r?\n)"
                    r"(\t// addi r6,r11,16\r?\n)"
                ),
                r"\1\tmcla_traffic_chassis_bound_8232D048(ctx.r9, r11);\n\2",
            ),
            (
                "sub_8232D8C8",
                "mcla_traffic_chassis_bound_8232D900",
                re.compile(
                    r"(\t// lwz r11,0\(r11\)\r?\n"
                    r"\tr11\.u64 = PPC_LOAD_U32\(r11\.u32 \+ 0\);\r?\n)"
                    r"(\t// li r10,144\r?\n)"
                ),
                r"\1\tmcla_traffic_chassis_bound_8232D900(ctx.r3, r11);\n\2",
            ),
            (
                "sub_8232E238",
                "mcla_traffic_chassis_bound_8232E274",
                re.compile(
                    r"(\t// lwz r31,0\(r10\)\r?\n"
                    r"\tr31\.u64 = PPC_LOAD_U32\(ctx\.r10\.u32 \+ 0\);\r?\n)"
                    r"(\t// lbz r8,2203\(r9\)\r?\n)"
                ),
                r"\1\tmcla_traffic_chassis_bound_8232E274(ctx.r3, r31);\n\2",
            ),
        ],
    },
    "ppc_recomp.120.cpp": {
        "decls": [
            'extern "C" bool mcla_ui_movie_advance(PPCRegister& r25, PPCRegister& r31, PPCRegister& f30, PPCRegister& f31, PPCCRRegister& cr6);',
        ],
        "hooks": [
            (
                "sub_825EDC08",
                "mcla_ui_movie_advance",
                re.compile(
                    r"(\tf31\.f64 = double\(float\(f0\.f64 / ctx\.f11\.f64\)\);\r?\n)"
                    r"(\t// beq cr6,0x825edc78\r?\n\tif \(cr6\.eq\) goto loc_825EDC78;\r?\n)"
                ),
                r"\1\tif (mcla_ui_movie_advance(r25, r31, f30, f31, cr6)) goto loc_825EDD44;\n\2",
            ),
        ],
    },
    "ppc_recomp.125.cpp": {
        "decls": [
            'extern "C" bool mcla_map_cursor_guard(PPCRegister& r31, PPCRegister& r30, PPCRegister& r26);',
        ],
        "hooks": [
            (
                "sub_82625368",
                "mcla_map_cursor_guard_label",
                re.compile(
                    r"(\t// stvx128 v63,r0,r26\r?\n[^\r\n]+\r?\n)"
                    r"(\t// addi r1,r1,240\r?\n)"
                ),
                r"\1loc_826253F4:\n\2",
            ),
            (
                "sub_82625368",
                "mcla_map_cursor_guard",
                re.compile(
                    r"(\t// lwz r31,84\(r1\)\r?\n"
                    r"\tr31\.u64 = PPC_LOAD_U32\(ctx\.r1\.u32 \+ 84\);\r?\n)"
                    r"(\t// lvx128 v57,r0,r29\r?\n)"
                ),
                r"\1\tif (mcla_map_cursor_guard(r31, r30, r26)) goto loc_826253F4;\n\2",
            ),
        ],
    },
    "ppc_recomp.131.cpp": {
        # Worker 1's header owns these six C++-linkage helpers. Including it
        # here (after ppc_recomp_shared.h) keeps one authoritative declaration
        # set; do NOT re-declare them as extern "C" (linkage conflict).
        "decls": [
            '#include "hooks/wave3_midhooks.h"',
        ],
        "hooks": [
            (
                "sub_82660310",
                "mcla_menu_capture_continue",
                re.compile(
                    r"(\tctx\.lr = 0x82660644;\r?\n"
                    r"\tsub_821FA230\(ctx, base\);\r?\n)"
                    r"(\t// lwz r8,0\(r3\)\r?\n)"
                ),
                r"\1\tmcla_menu_capture_continue(ctx.r3);\n\2",
            ),
            (
                "sub_82660310",
                "mcla_menu_enable_save",
                re.compile(
                    r"(\tctx\.lr = 0x82660C28;\r?\n"
                    r"\tsub_821FA230\(ctx, base\);\r?\n"
                    r"\t// lwz r11,0\(r3\)\r?\n"
                    r"\tr11\.u64 = PPC_LOAD_U32\(ctx\.r3\.u32 \+ 0\);\r?\n"
                    r"\t// li r4,0\r?\n)"
                    r"(\tctx\.r4\.s64 = 0;\r?\n)"
                ),
                r"\1\tif (!mcla_menu_enable_save(ctx, base)) ctx.r4.s64 = 0;\n\2",
            ),
            (
                "sub_82661210",
                "mcla_menu_null_table_guard",
                re.compile(
                    r"(loc_82661268:\r?\n\t// lwz r30,192\(r31\)\r?\n"
                    r"\tr30\.u64 = PPC_LOAD_U32\(r31\.u32 \+ 192\);\r?\n)"
                    r"(\t// mr r3,r31\r?\n)"
                ),
                r"\1\tif (mcla_menu_null_table_guard(r30)) { ctx.r3.s64 = 0; ctx.r1.s64 += 112; return; }\n\2",
            ),
            (
                "sub_8265F008",
                "mcla_menu_list_vtable_guard_label",
                re.compile(r"(loc_8265F0F0:\r?\n)(\t// lwz r10,0\(r31\)\r?\n)"),
                r"\1\tif (mcla_menu_list_vtable_guard(r31)) goto loc_8265F108;\n\2",
            ),
            (
                "sub_8265F008",
                "mcla_menu_list_vtable_guard",
                re.compile(
                    r"(\tPPC_CALL_INDIRECT_FUNC\(ctr\.u32\);\r?\n)"
                    r"(\t// lwz r3,192\(r28\)\r?\n)"
                ),
                r"\1loc_8265F108:\n\2",
            ),
        ],
    },
    "ppc_recomp.132.cpp": {
        "decls": [
            '#include "hooks/wave3_midhooks.h"',
        ],
        "hooks": [
            (
                "sub_82668660",
                "mcla_menu_click",
                re.compile(r"(loc_826686B8:\r?\n)(\t// lis r11,-32246\r?\n)"),
                r"\1\tif (mcla_menu_click(ctx, base, r31)) { ctx.r3.s64 = 1; ctx.r1.s64 += 768; return; }\n\2",
            ),
            (
                "sub_82668660",
                "mcla_menu_cancel",
                re.compile(r"(loc_82669864:\r?\n)(\t// lwz r11,0\(r31\)\r?\n)"),
                r"\1\tif (mcla_menu_cancel(ctx, base)) goto loc_826698AC;\n\2",
            ),
        ],
    },
    "ppc_recomp.151.cpp": {
        "decls": [
            'extern "C" bool mcla_ui_lights_guard(PPCRegister& node);',
        ],
        "hooks": [
            (
                "sub_827250A8",
                "mcla_ui_lights_guard_label",
                re.compile(
                    r"(\t// bl 0x825ef9f0\r?\n"
                    r"\tctx\.lr = 0x82725104;\r?\n"
                    r"\tsub_825EF9F0\(ctx, base\);\r?\n)"
                ),
                r"\tif (mcla_ui_lights_guard(ctx.r3)) goto loc_82725144;\n\1",
            ),
            (
                "sub_827250A8",
                "mcla_ui_lights_guard",
                re.compile(
                    r"(\tsub_821800A0\(ctx, base\);\r?\n)"
                    r"(\t// li r3,1\r?\n)"
                ),
                r"\1loc_82725144:\n\2",
            ),
        ],
    },
}


def inject_midhooks(tu_name, text, counts):
    spec = MIDHOOK_SPECS.get(tu_name)
    if not spec:
        return text
    inc_lf = '#include "ppc_recomp_shared.h"\n'
    inc_crlf = '#include "ppc_recomp_shared.h"\r\n'
    if inc_crlf in text:
        text = text.replace(inc_crlf, inc_crlf + "\n".join(spec["decls"]) + "\n", 1)
    elif inc_lf in text:
        text = text.replace(inc_lf, inc_lf + "\n".join(spec["decls"]) + "\n", 1)
    else:
        raise RuntimeError(f"{tu_name}: missing #include \"ppc_recomp_shared.h\"")
    for fn, hook_name, pattern, repl in spec["hooks"]:
        fn_hdr = f"PPC_FUNC_IMPL(__imp__{fn})"
        start = text.find(fn_hdr)
        if start < 0:
            raise RuntimeError(f"{tu_name}: missing {fn_hdr}")
        nxt = text.find("\n__attribute__", start)
        end = nxt if nxt >= 0 else len(text)
        span = text[start:end]
        new_span, n = pattern.subn(repl, span)
        if n != 1:
            raise RuntimeError(f"{tu_name} {fn} {hook_name}: expected 1 match, got {n}")
        text = text[:start] + new_span + text[end:]
        counts[hook_name] = counts.get(hook_name, 0) + 1
    return text


def is_local(reg):
    """Mirrors the r()/f()/v() lambdas at recompiler.cpp:420-445 with this project's config."""
    kind, n = reg[0], int(reg[1:])
    if kind == "r":
        return n in (0, 2, 11, 12) or n >= 14
    if kind == "f":
        return n == 0 or n >= 14
    return n >= 14


def name(reg):
    return reg if is_local(reg) else "ctx." + reg


def li(reg, f="u8"):
    return f"simde_mm_load_si128((simde__m128i*){name(reg)}.{f})"


def emit(op, ops):
    d = name(ops[0])
    store = f"simde_mm_store_si128((simde__m128i*){d}"
    if op == "frsqrte":
        return "ctx.fpscr.disableFlushMode(); " + f"{d}.f64 = 1.0 / sqrt({name(ops[1])}.f64);"
    if op == "vsel128":
        a, b, m = ops[1], ops[2], ops[3]
        return (f"{store}.u8, simde_mm_or_si128(simde_mm_andnot_si128({li(m)}, {li(a)}), "
                f"simde_mm_and_si128({li(m)}, {li(b)})));")
    if op == "vspltish":
        return f"{store}.u16, simde_mm_set1_epi16(short({ops[1]})));"
    if op == "vpkswss128":
        return f"{store}.s16, simde_mm_packs_epi32({li(ops[2], 's32')}, {li(ops[1], 's32')}));"
    if op == "vcfpuxws128":
        assert ops[2] == "0", "scaled form not measured at any site"
        s = name(ops[1])
        return (f"{{ ctx.fpscr.enableFlushMode(); "
                f"simde__m128 x_ = simde_mm_load_ps({s}.f32); "
                f"simde__m128 c_ = simde_mm_max_ps(x_, simde_mm_setzero_ps()); "
                f"simde__m128 m_ = simde_mm_cmpge_ps(c_, simde_mm_set1_ps(2147483648.0f)); "
                f"simde__m128 s_ = simde_mm_blendv_ps(c_, simde_mm_sub_ps(c_, simde_mm_set1_ps(2147483648.0f)), m_); "
                f"simde__m128i r_ = simde_mm_cvttps_epi32(s_); "
                f"simde__m128i ov_ = simde_mm_cmpeq_epi32(r_, simde_mm_set1_epi32(int(0x80000000))); "
                f"r_ = simde_mm_or_si128(simde_mm_add_epi32(r_, simde_mm_and_si128(simde_mm_castps_si128(m_), simde_mm_set1_epi32(int(0x80000000)))), ov_); "
                f"{store}.u32, simde_mm_andnot_si128(simde_mm_castps_si128(simde_mm_cmpunord_ps(x_, x_)), r_)); }}")
    if op == "vrfip128":
        return ("ctx.fpscr.enableFlushMode(); "
                f"simde_mm_store_ps({d}.f32, simde_mm_round_ps(simde_mm_load_ps({name(ops[1])}.f32), "
                "SIMDE_MM_FROUND_TO_POS_INF | SIMDE_MM_FROUND_NO_EXC));")
    if op == "vpkuwus128":
        cap = "simde_mm_set1_epi32(0xFFFF)"
        return (f"{store}.u16, simde_mm_packus_epi32(simde_mm_min_epu32({li(ops[2], 'u32')}, {cap}), "
                f"simde_mm_min_epu32({li(ops[1], 'u32')}, {cap})));")
    if op == "vpkuwum128":
        low = "simde_mm_set1_epi32(0xFFFF)"
        return (f"{store}.u16, simde_mm_packus_epi32(simde_mm_and_si128({li(ops[2], 'u32')}, {low}), "
                f"simde_mm_and_si128({li(ops[1], 'u32')}, {low})));")
    if op == "vpkuhum128":
        low = "simde_mm_set1_epi16(0xFF)"
        return (f"{store}.u8, simde_mm_packus_epi16(simde_mm_and_si128({li(ops[2], 'u16')}, {low}), "
                f"simde_mm_and_si128({li(ops[1], 'u16')}, {low})));")
    if op == "eqv":
        return f"{d}.u64 = ~({name(ops[1])}.u64 ^ {name(ops[2])}.u64);"
    if op == "mulhdu":
        return f"{d}.u64 = uint64_t((unsigned __int128)({name(ops[1])}.u64) * {name(ops[2])}.u64 >> 64);"
    raise ValueError(op)


def decl(reg):
    return {"r": "PPCRegister", "f": "PPCRegister", "v": "PPCVRegister"}[reg[0]] + f" {reg}{{}};"


def registers(ops):
    return [o for o in ops if re.fullmatch(r"[rfv]\d+", o)]


def patch(lines, counts):
    """Adds one line per dropped op; a local the tool never declared goes after the prologue."""
    out, prologue, declared = [], None, set()
    missing = set()

    def close():
        nonlocal missing
        if prologue is not None and missing:
            out[prologue:prologue] = ["\t" + decl(r) + "\n" for r in sorted(missing)]
        missing = set()

    for line in lines:
        if FUNC.search(line):
            close()
            prologue, declared = None, set()
        out.append(line)
        if "PPC_FUNC_PROLOGUE();" in line:
            prologue = len(out)
        m = re.match(r"\s*PPCV?Register ([rfv]\d+)\{\};", line)
        if m:
            declared.add(m[1])
        m = INSN.match(line)
        if m:
            ops = m[2].split(",")
            out.append("\t" + emit(m[1], ops) + "\n")
            counts[m[1]] = counts.get(m[1], 0) + 1
            missing |= {r for r in registers(ops) if is_local(r) and r not in declared}
    close()
    return out


def _bfp(m):
    d, a, b = (name(x) for x in m.groups())
    nl, t = "\n", "\t"
    return (f"{t}// vcmpbfp128 {m.group(1)},{m.group(2)},{m.group(3)}{nl}"
            f"{t}{{ ctx.fpscr.enableFlushMode();{nl}"
            f"{t}  simde__m128 a_ = simde_mm_load_ps({a}.f32), b_ = simde_mm_load_ps({b}.f32);{nl}"
            f"{t}  simde__m128 nb_ = simde_mm_xor_ps(b_, simde_mm_castsi128_ps(simde_mm_set1_epi32(int32_t(0x80000000))));{nl}"
            f"{t}  simde__m128i le_ = simde_mm_castps_si128(simde_mm_cmple_ps(a_, b_)), ge_ = simde_mm_castps_si128(simde_mm_cmpge_ps(a_, nb_));{nl}"
            f"{t}  simde_mm_store_si128((simde__m128i*){d}.u32, simde_mm_or_si128(simde_mm_andnot_si128(le_, simde_mm_set1_epi32(int32_t(0x80000000))),{nl}"
            f"{t}      simde_mm_andnot_si128(ge_, simde_mm_set1_epi32(0x40000000)))); }}{nl}")


def _upk8(m):
    d, s = name(m.group(1)), name(m.group(2))
    return (f"\t// vupkd3d128 {m.group(1)},{m.group(2)},8\n"
            f"\t{{ uint32_t p_ = {s}.u32[0];\n"
            f"\t  auto u10_ = [](int32_t f_) {{ uint32_t b_ = 0x40400000u + uint32_t(f_); return b_ == 0x403FFE00u ? 0x7FC00000u : b_; }};\n"
            f"\t  {d}.u32[3] = u10_(int32_t(p_ << 22) >> 22);\n"
            f"\t  {d}.u32[2] = u10_(int32_t(p_ << 12) >> 22);\n"
            f"\t  {d}.u32[1] = u10_(int32_t(p_ << 2) >> 22);\n"
            f"\t  {d}.u32[0] = 0x3F800000u | ((p_ >> 30) & 3u); }}\n")


def _pk3(m):
    d, s = name(m.group(1)), name(m.group(2))
    return (f"\t// vpkd3d128 {m.group(1)},{m.group(2)},3,1,3\n"
            f"\tctx.fpscr.enableFlushMode();\n"
            f"\t{{ auto f2h_ = [](uint32_t u_) -> uint16_t {{\n"
            f"\t    uint32_t s_ = (u_ >> 16) & 0x8000u, a_ = u_ & 0x7FFFFFFFu;\n"
            f"\t    if (a_ > 0x7F800000u) return uint16_t(s_ | 0x7E00u | ((u_ >> 13) & 0x3FFu));\n"
            f"\t    if (a_ == 0x7F800000u) return uint16_t(s_ | 0x7C00u);\n"
            f"\t    if (a_ >= 0x47800000u) return uint16_t(s_ | 0x7BFFu);\n"
            f"\t    if (a_ < 0x38800000u) {{ uint32_t sh_ = 113u - (a_ >> 23); return uint16_t(s_ | (sh_ > 24u ? 0u : ((0x800000u | (a_ & 0x7FFFFFu)) >> sh_))); }}\n"
            f"\t    return uint16_t(s_ | (((a_ >> 23) - 112u) << 10) | ((a_ >> 13) & 0x3FFu)); }};\n"
            f"\t  {d}.u16[7] = f2h_({s}.u32[3]);\n"
            f"\t  {d}.u16[6] = f2h_({s}.u32[2]); }}\n")


def self_test():
    assert emit("frsqrte", ["f31", "f1"]) == "ctx.fpscr.disableFlushMode(); f31.f64 = 1.0 / sqrt(ctx.f1.f64);"
    assert emit("frsqrte", ["f0", "f14"]) == "ctx.fpscr.disableFlushMode(); f0.f64 = 1.0 / sqrt(f14.f64);"
    assert emit("vrfip128", ["v14", "v1"]).startswith("ctx.fpscr.enableFlushMode(); simde_mm_store_ps(v14.f32,")
    ux = emit("vcfpuxws128", ["v14", "v2", "0"])
    assert "ctx.fpscr.enableFlushMode();" in ux and "2147483648.0f" in ux and "simde_mm_vctuxs" not in ux
    assert "ctx.v12.u8" in emit("vsel128", ["v12", "v13", "v35", "v12"])
    assert emit("eqv", ["r11", "r27", "r28"]) == "r11.u64 = ~(r27.u64 ^ r28.u64);"
    src = ["PPC_FUNC_IMPL(__imp__sub_1) {\n", "\tPPC_FUNC_PROLOGUE();\n", "\t// frsqrte f31,f1\n", "}\n",
           "PPC_FUNC_IMPL(__imp__sub_2) {\n", "\tPPC_FUNC_PROLOGUE();\n", "\tPPCRegister f30{};\n",
           "\t// frsqrte f30,f1\n", "}\n"]
    c = {}
    got = patch(src, c)
    assert c == {"frsqrte": 2} and got[2] == "\tPPCRegister f31{};\n"
    assert got.count("\tPPCRegister f30{};\n") == 1 and "ctx.fpscr.disableFlushMode(); f30.f64 = 1.0 / sqrt(ctx.f1.f64);" in "".join(got)
    bfp_out, kb = BFP.subn(_bfp, "\t// vcmpbfp128 v14,v1,v2\n\t__builtin_debugtrap();\n")
    assert kb == 1 and "ctx.fpscr.enableFlushMode();" in bfp_out
    upk8_out, k8 = UPK8.subn(_upk8, "\t// vupkd3d128 v0,v62,8\n\t__builtin_debugtrap();\n")
    assert k8 == 1 and "ctx.v0.u32[3]" in upk8_out and "v62.u32[0]" in upk8_out
    pk3_out, k3 = PK3.subn(_pk3, "\t// vpkd3d128 v63,v1,3,1,3\n\tctx.fpscr.enableFlushMode();\n\t__builtin_debugtrap();\n")
    assert k3 == 1 and "v63.u16[7] = f2h_(ctx.v1.u32[3]);" in pk3_out
    mull_out, km = MULLHWU.subn(
        "\t// mullhwu. r19,r0,r28 (vsldoi128 v19,v0,v60,12)\n"
        "\tsimde_mm_store_si128((simde__m128i*)v19.u8, simde_mm_alignr_epi8("
        "simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)v60.u8), 4));\n",
        "\t// mullhwu. r19,r0,r28\n",
    )
    assert km == 1 and "vsldoi128 v19,v0,v60,12" in mull_out
    v20_sample = (
        "\t// vupkd3d128 v57,v61,20\n"
        + "".join(f"\t{{ uint32_t h = v61.u16[{i}]; uint32_t s = 0;\n\t  l1\n\t  l2\n\t  l3\n\t  l4 }}\n" for i in range(4, 8))
    )
    v20_out, k20 = VUPK20.subn(r"\g<1>0\g<2>1\g<3>2\g<4>3\g<5>", v20_sample)
    assert k20 == 1 and all(f"v61.u16[{i}]" in v20_out for i in range(4))
    pk22_sample = "\t// vpkd3d128 v12,v50,0,2,2\n" + "\tline\n" * 12 + "\tctx.v12.u32[2] = temp.u32;\n"
    pk22_out, k22 = PKD3D_22.subn(lambda m: f"{m.group(1)}\t{name(m.group(2))}.u32[3] = 0;\n", pk22_sample)
    assert k22 == 1 and "\tctx.v12.u32[3] = 0;\n" in pk22_out
    pk30_sample = "\t// vpkd3d128 v50,v63,0,3,0\n" + "\tline\n" * 12 + "\tv50.u32[0] = temp.u32;\n"
    pk30_out, k30 = PKD3D_30.subn(lambda m: f"{m.group(1)}\t{name(m.group(2))}.u32[1] = 0;\n", pk30_sample)
    assert k30 == 1 and "\tv50.u32[1] = 0;\n" in pk30_out
    fct_out, kfct = FCTID.subn(r"\1>= \2", "\tctx.f13.s64 = (f0.f64 > double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvttsd_si64(simde_mm_load_sd(&f0.f64));\n")
    assert kfct == 1 and ">= double(LLONG_MAX)" in fct_out
    sraw_sample = (
        "\t// sraw. r11,r26,r11\n"
        "\ttemp.u32 = r11.u32 & 0x3F;\n"
        "\tif (temp.u32 > 0x1F) temp.u32 = 0x1F;\n"
        "\txer.ca = (r26.s32 < 0) & (((r26.s32 >> temp.u32) << temp.u32) != r26.s32);\n"
        "\tr11.s64 = r26.s32 >> temp.u32;\n"
    )
    sraw_out, ksraw = SRAW.subn(SRAW_REPL, sraw_sample)
    assert ksraw == 1 and "(r26.s32 >> 31)" in sraw_out and "0x1Fu" in sraw_out
    srad_sample = (
        "\t// srad r10,r8,r7\n"
        "\ttemp.u64 = ctx.r7.u64 & 0x7F;\n"
        "\tif (temp.u64 > 0x3F) temp.u64 = 0x3F;\n"
        "\txer.ca = (ctx.r8.s64 < 0) & (((ctx.r8.s64 >> temp.u64) << temp.u64) != ctx.r8.s64);\n"
        "\tctx.r10.s64 = ctx.r8.s64 >> temp.u64;\n"
    )
    srad_out, ksrad = SRAD.subn(SRAD_REPL, srad_sample)
    assert ksrad == 1 and "(ctx.r8.s64 >> 63)" in srad_out and "1ULL << temp.u64" in srad_out
    rlw_out, krlw = RLWIMI_WRAP.subn(
        r"__builtin_rotateleft64(\1.u32 | (\1.u64 << 32), \2) & \3",
        "\tctx.r8.u64 = (__builtin_rotateleft32(r27.u32, 0) & 0xFFFFFFFFFFFFFFEF) | (ctx.r8.u64 & 0x10);\n",
    )
    assert krlw == 1 and "__builtin_rotateleft64(r27.u32 | (r27.u64 << 32), 0) & 0xFFFFFFFFFFFFFFEF" in rlw_out
    # Wave 2 mid-asm hook self-tests
    mh_counts = {}
    t2 = '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_82148A10) {\n\t// li r3,0\n\tctx.r3.s64 = 0;\n\t// bl 0x821c91b8\n\tctx.lr = 0x82148AB8;\n\tsub_821C91B8(ctx, base);\n\t// lbz r10,199(r31)\n}\n'
    t2_out = inject_midhooks("ppc_recomp.2.cpp", t2, mh_counts)
    assert mh_counts["mcla_poll_wait"] == 1 and "goto loc_82148AB8;" in t2_out and "loc_82148AB8:" in t2_out
    t5 = '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_82160088) {\n\t// stw r11,4(r6)\n\tPPC_STORE_U32(ctx.r6.u32 + 4, r11.u32);\n}\n'
    t5_out = inject_midhooks("ppc_recomp.5.cpp", t5, mh_counts)
    assert mh_counts["mcla_poll_release"] == 1 and "mcla_poll_release(ctx.r6.u32);" in t5_out
    t52 = '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_82320298) {\nloc_82320468:\n\t// addi r9,r1,80\nloc_823204F4:\n\t// li r9,288\n}\n'
    t52_out = inject_midhooks("ppc_recomp.52.cpp", t52, mh_counts)
    assert mh_counts["mcla_cam_boom_scale_outer"] == 1 and mh_counts["mcla_cam_boom_scale_inner"] == 1
    assert "mcla_cam_boom_scale_outer(&ctx.f13.f64);" in t52_out and "mcla_cam_boom_scale_inner(&f0.f64);" in t52_out
    t56 = '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_823688B8) {\nloc_8236895C:\n\t// addi r8,r1,100\nloc_823689F0:\n\t// addi r9,r1,100\n}\n'
    t56_out = inject_midhooks("ppc_recomp.56.cpp", t56, mh_counts)
    assert mh_counts["mcla_bike_gyro_scale_lean"] == 1 and mh_counts["mcla_bike_gyro_scale_damp"] == 1
    assert "mcla_bike_gyro_scale_lean(&ctx.f13.f64);" in t56_out and "mcla_bike_gyro_scale_damp(&f0.f64);" in t56_out
    t110 = '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_82563298) {\nloc_82563720:\n\t// lis r11,-32256\n}\n'
    t110_out = inject_midhooks("ppc_recomp.110.cpp", t110, mh_counts)
    assert mh_counts["mcla_chassis_filter_scale"] == 1 and "mcla_chassis_filter_scale(&f0.f64);" in t110_out
    t135 = '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_826881A8) {\n\t// stvx128 v62,r0,r7\n\tsimde_mm_store_si128(x);\n\t// beq cr6,0x82688218\n}\n'
    t135_out = inject_midhooks("ppc_recomp.135.cpp", t135, mh_counts)
    assert mh_counts["mcla_trigger_line_extend_stack"] == 1 and "mcla_trigger_line_extend_stack(ctx.r1.u32, r31.u32, r27.u32);" in t135_out
    try:
        inject_midhooks("ppc_recomp.110.cpp", '#include "ppc_recomp_shared.h"\nPPC_FUNC_IMPL(__imp__sub_82563298) {}\n', {})
        assert False, "expected RuntimeError on missing anchor"
    except RuntimeError:
        pass
    # ---- Wave 3 mid-asm hook self-tests --------------------------------------
    hdr = '#include "ppc_recomp_shared.h"\n'
    sep = '\n__attribute__((weak))\n'
    t22 = hdr + 'PPC_FUNC_IMPL(__imp__sub_821FDED8) {\n' \
        '\t// bl 0x823dc018\n\tctx.lr = 0x821FE22C;\n\tsub_823DC018(ctx, base);\n}\n'
    t22_out = inject_midhooks("ppc_recomp.22.cpp", t22, mh_counts)
    assert mh_counts["mcla_prompts_platform_flag"] == 1 and "mcla_prompts_platform_flag(r27);" in t22_out
    t26 = hdr + 'PPC_FUNC_IMPL(__imp__sub_82223CD8) {\n' \
        '\t// stbx r27,r31,r28\n\tPPC_STORE_U8(r31.u32 + r28.u32, r27.u8);\n' \
        '\t// cmpwi cr6,r4,-1\n\tcr6.compare<int32_t>(ctx.r4.s32, -1, xer);\n' \
        '\t// stbx r21,r31,r29\n\tPPC_STORE_U8(r31.u32 + r29.u32, r21.u8);\n' \
        '\t// stb r22,16(r26)\n\tPPC_STORE_U8(r26.u32 + 16, r22.u8);\n' \
        '\t// stbx r8,r31,r29\n\tPPC_STORE_U8(r31.u32 + r29.u32, ctx.r8.u8);\n' \
        '\t// cmplw cr6,r10,r8\n\tcr6.compare<uint32_t>(ctx.r10.u32, ctx.r8.u32, xer);\n}\n'
    t26_out = inject_midhooks("ppc_recomp.26.cpp", t26, mh_counts)
    assert mh_counts["mcla_ui_repeat_start"] == 1 and mh_counts["mcla_ui_repeat_gate"] == 1
    assert "mcla_ui_repeat_start(r31, ctx.r4);" in t26_out
    assert "mcla_ui_repeat_gate(r31, r29, ctx.r4, ctx.r8, ctx.r10);" in t26_out
    t53 = hdr + 'PPC_FUNC_IMPL(__imp__sub_8232CFF0) {\n' \
        '\t// lwz r11,0(r9)\n\tr11.u64 = PPC_LOAD_U32(ctx.r9.u32 + 0);\n' \
        '\t// addi r6,r11,16\n\tctx.r6.s64 = r11.s64 + 16;\n}\n' + sep + \
        'PPC_FUNC_IMPL(__imp__sub_8232D8C8) {\n' \
        '\t// lwz r11,0(r11)\n\tr11.u64 = PPC_LOAD_U32(r11.u32 + 0);\n' \
        '\t// li r10,144\n\tctx.r10.s64 = 144;\n}\n' + sep + \
        'PPC_FUNC_IMPL(__imp__sub_8232E238) {\n' \
        '\t// lwz r31,0(r10)\n\tr31.u64 = PPC_LOAD_U32(ctx.r10.u32 + 0);\n' \
        '\t// lbz r8,2203(r9)\n\tctx.r8.u64 = PPC_LOAD_U8(ctx.r9.u32 + 2203);\n}\n'
    t53_out = inject_midhooks("ppc_recomp.53.cpp", t53, mh_counts)
    for h in ("8232D048", "8232D900", "8232E274"):
        assert mh_counts["mcla_traffic_chassis_bound_" + h] == 1
    assert "mcla_traffic_chassis_bound_8232D048(ctx.r9, r11);" in t53_out
    t120 = hdr + 'PPC_FUNC_IMPL(__imp__sub_825EDC08) {\n' \
        '\tf31.f64 = double(float(f0.f64 / ctx.f11.f64));\n' \
        '\t// beq cr6,0x825edc78\n\tif (cr6.eq) goto loc_825EDC78;\n}\n'
    t120_out = inject_midhooks("ppc_recomp.120.cpp", t120, mh_counts)
    assert mh_counts["mcla_ui_movie_advance"] == 1
    assert "goto loc_825EDD44;" in t120_out
    t125 = hdr + 'PPC_FUNC_IMPL(__imp__sub_82625368) {\n' \
        '\t// stvx128 v63,r0,r26\n\tsimde_mm_store_si128(x);\n' \
        '\t// addi r1,r1,240\n\tctx.r1.s64 = ctx.r1.s64 + 240;\n' \
        '\t// lwz r31,84(r1)\n\tr31.u64 = PPC_LOAD_U32(ctx.r1.u32 + 84);\n' \
        '\t// lvx128 v57,r0,r29\n\tsimde_mm_store_si128(y);\n}\n'
    t125_out = inject_midhooks("ppc_recomp.125.cpp", t125, mh_counts)
    assert mh_counts["mcla_map_cursor_guard"] == 1 and mh_counts["mcla_map_cursor_guard_label"] == 1
    assert "loc_826253F4:" in t125_out and "mcla_map_cursor_guard(r31, r30, r26)" in t125_out
    t131 = hdr + 'PPC_FUNC_IMPL(__imp__sub_82660310) {\n' \
        '\tctx.lr = 0x82660644;\n\tsub_821FA230(ctx, base);\n' \
        '\t// lwz r8,0(r3)\n\tctx.r8.u64 = PPC_LOAD_U32(ctx.r3.u32 + 0);\n' \
        '\tctx.lr = 0x82660C28;\n\tsub_821FA230(ctx, base);\n' \
        '\t// lwz r11,0(r3)\n\tr11.u64 = PPC_LOAD_U32(ctx.r3.u32 + 0);\n' \
        '\t// li r4,0\n\tctx.r4.s64 = 0;\n}\n' + sep + \
        'PPC_FUNC_IMPL(__imp__sub_82661210) {\nloc_82661268:\n' \
        '\t// lwz r30,192(r31)\n\tr30.u64 = PPC_LOAD_U32(r31.u32 + 192);\n' \
        '\t// mr r3,r31\n\tctx.r3.u64 = r31.u64;\n}\n' + sep + \
        'PPC_FUNC_IMPL(__imp__sub_8265F008) {\nloc_8265F0F0:\n' \
        '\t// lwz r10,0(r31)\n\tctx.r10.u64 = PPC_LOAD_U32(r31.u32 + 0);\n' \
        '\tPPC_CALL_INDIRECT_FUNC(ctr.u32);\n' \
        '\t// lwz r3,192(r28)\n\tctx.r3.u64 = PPC_LOAD_U32(r28.u32 + 192);\n}\n'
    t131_out = inject_midhooks("ppc_recomp.131.cpp", t131, mh_counts)
    for h in ("mcla_menu_capture_continue", "mcla_menu_enable_save", "mcla_menu_null_table_guard",
              "mcla_menu_list_vtable_guard", "mcla_menu_list_vtable_guard_label"):
        assert mh_counts[h] == 1
    assert "loc_8265F108:" in t131_out
    t132 = hdr + 'PPC_FUNC_IMPL(__imp__sub_82668660) {\n' \
        'loc_826686B8:\n\t// lis r11,-32246\n\tr11.s64 = -2113273856;\n' \
        'loc_82669864:\n\t// lwz r11,0(r31)\n\tr11.u64 = PPC_LOAD_U32(r31.u32 + 0);\n}\n'
    t132_out = inject_midhooks("ppc_recomp.132.cpp", t132, mh_counts)
    assert mh_counts["mcla_menu_click"] == 1 and mh_counts["mcla_menu_cancel"] == 1
    assert "mcla_menu_cancel(ctx, base)" in t132_out
    t151 = hdr + 'PPC_FUNC_IMPL(__imp__sub_827250A8) {\n' \
        '\t// bl 0x825ef9f0\n\tctx.lr = 0x82725104;\n\tsub_825EF9F0(ctx, base);\n' \
        '\tsub_821800A0(ctx, base);\n\t// li r3,1\n\tctx.r3.s64 = 1;\n}\n'
    t151_out = inject_midhooks("ppc_recomp.151.cpp", t151, mh_counts)
    assert mh_counts["mcla_ui_lights_guard"] == 1 and mh_counts["mcla_ui_lights_guard_label"] == 1
    assert "loc_82725144:" in t151_out and "mcla_ui_lights_guard(ctx.r3)" in t151_out
    n_real = verify_real_anchors()
    print("self-test ok")
    print(f"wave3 real anchors verified: {n_real}")


def verify_real_anchors():
    """For every MIDHOOK_SPECS hook, require exactly one re.subn match in the
    real generated TU's function span (the same count==1 rule patch_midhooks
    enforces). This is the measurable proof that no anchor is ambiguous."""
    gen = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "generated", "ppc_xenon"))
    if not os.path.isdir(gen):
        return 0
    checked = 0
    for tu_name, spec in MIDHOOK_SPECS.items():
        path = os.path.join(gen, tu_name)
        if not os.path.isfile(path):
            raise RuntimeError(f"real-anchor check: {tu_name} missing in {gen}")
        with open(path, newline="", errors="surrogateescape") as f:
            text = f.read()
        for fn, hook_name, pattern, _repl in spec["hooks"]:
            hdr = f"PPC_FUNC_IMPL(__imp__{fn})"
            start = text.find(hdr)
            if start < 0:
                raise RuntimeError(f"real-anchor: {tu_name} missing {hdr}")
            nxt = text.find("\n__attribute__", start)
            span = text[start:(nxt if nxt >= 0 else len(text))]
            _, n = pattern.subn("__ANCHOR__", span)
            if n != 1:
                raise RuntimeError(f"real-anchor: {tu_name} {fn} {hook_name}: count={n}")
            checked += 1
    return checked


def main():
    if sys.argv[1] == "--self-test":
        return self_test()
    gen, overlay = sys.argv[1], sys.argv[2]
    os.makedirs(overlay, exist_ok=True)
    counts = {}
    for path in sorted(glob.glob(os.path.join(gen, "ppc_recomp.*.cpp"))):
        with open(path, newline="", errors="surrogateescape") as f:
            lines = f.readlines()
        has_pk = any(
            "< 3.0f ? 3.0f :" in l
            or "// vcmpbfp128 " in l
            or "// mullhwu. " in l
            or ("// vupkd3d128 " in l and (l.rstrip().endswith(",20") or l.rstrip().endswith(",8")))
            or ("// vpkd3d128 " in l and (l.rstrip().endswith(",3,1,3") or l.rstrip().endswith(",0,2,2") or l.rstrip().endswith(",0,3,0")))
            or "> double(LLONG_MAX)) ? LLONG_MAX : simde_mm_cvt" in l
            or "// sraw " in l
            or "// sraw. " in l
            or "// srad " in l
            or ("__builtin_rotateleft32(" in l and "0xFFFFFFFF" in l)
            for l in lines
        )
        tu_name = os.path.basename(path)
        if not any(INSN.match(l) for l in lines) and not has_pk and tu_name not in MIDHOOK_SPECS:
            continue
        orig = "".join(lines)
        new = "".join(patch(lines, counts))
        new = inject_midhooks(tu_name, new, counts)
        # vcmpbfp128 (bounds compare) was emitted as a trap (1 site, sub_825CE970, reached once the car moves):
        # lane = (A <= B ? 0 : 0x80000000) | (A >= -B ? 0 : 0x40000000); NaN sets both (PPC vcmpbfp).
        new, kb = BFP.subn(_bfp, new)
        counts["vcmpbfp128"] = counts.get("vcmpbfp128", 0) + kb
        new, km = MULLHWU.subn(
            "\t// mullhwu. r19,r0,r28 (vsldoi128 v19,v0,v60,12)\n"
            "\tsimde_mm_store_si128((simde__m128i*)v19.u8, simde_mm_alignr_epi8("
            "simde_mm_load_si128((simde__m128i*)ctx.v0.u8), simde_mm_load_si128((simde__m128i*)v60.u8), 4));\n",
            new,
        )
        counts["mullhwu"] = counts.get("mullhwu", 0) + km
        new, k8 = UPK8.subn(_upk8, new)
        counts["vupkd3d128-8"] = counts.get("vupkd3d128-8", 0) + k8
        new, k3 = PK3.subn(_pk3, new)
        counts["vpkd3d128-3"] = counts.get("vpkd3d128-3", 0) + k3
        new, k20 = VUPK20.subn(r"\g<1>0\g<2>1\g<3>2\g<4>3\g<5>", new)
        counts["vupkd3d128-20"] = counts.get("vupkd3d128-20", 0) + k20
        new, k22 = PKD3D_22.subn(lambda m: f"{m.group(1)}\t{name(m.group(2))}.u32[3] = 0;\n", new)
        new, k30 = PKD3D_30.subn(lambda m: f"{m.group(1)}\t{name(m.group(2))}.u32[1] = 0;\n", new)
        counts["vpkd3d128-d3d-hi"] = counts.get("vpkd3d128-d3d-hi", 0) + k22 + k30
        new, kfct = FCTID.subn(r"\1>= \2", new)
        counts["fctid-ge"] = counts.get("fctid-ge", 0) + kfct
        new, ksraw = SRAW.subn(SRAW_REPL, new)
        new, ksrad = SRAD.subn(SRAD_REPL, new)
        counts["sraw-srad-ca"] = counts.get("sraw-srad-ca", 0) + ksraw + ksrad
        new, krlw = RLWIMI_WRAP.subn(r"__builtin_rotateleft64(\1.u32 | (\1.u64 << 32), \2) & \3", new)
        counts["rlwimi-wrap"] = counts.get("rlwimi-wrap", 0) + krlw
        if has_pk:
            new, k = PKD3D.subn(r"!(\1 >= 3.0f) ? 3.0f :", new)
            counts["vpkd3d128-nan"] = counts.get("vpkd3d128-nan", 0) + k
        if new == orig:
            continue
        target = os.path.join(overlay, os.path.basename(path))
        old = None
        if os.path.exists(target):
            with open(target, newline="", errors="surrogateescape") as f:
                old = f.read()
        if old != new:  # keeps mtimes, so an unchanged TU does not recompile
            with open(target, "w", newline="", errors="surrogateescape") as f:
                f.write(new)
        print(os.path.basename(path).split(".")[1])
    if counts.pop("vcmpbfp128", 0) != 1:
        sys.exit("vcmpbfp128 census changed (expected 1 site)")
    if counts.pop("mullhwu", 0) != 1:
        sys.exit("mullhwu census changed (expected 1 site)")
    if counts.pop("vupkd3d128-8", 0) != 1:
        sys.exit("vupkd3d128-8 census changed (expected 1 site)")
    if counts.pop("vpkd3d128-3", 0) != 1:
        sys.exit("vpkd3d128-3 census changed (expected 1 site)")
    if counts.pop("vupkd3d128-20", 0) != 94:
        sys.exit("vupkd3d128-20 census changed (expected 94 sites)")
    if counts.pop("vpkd3d128-d3d-hi", 0) != 3:
        sys.exit("vpkd3d128-d3d-hi census changed (expected 3 sites)")
    if counts.pop("fctid-ge", 0) != 351:
        sys.exit("fctid-ge census changed (expected 351 sites)")
    if counts.pop("sraw-srad-ca", 0) != 93:
        sys.exit("sraw-srad-ca census changed (expected 93 sites)")
    if counts.pop("rlwimi-wrap", 0) != 23:
        sys.exit("rlwimi-wrap census changed (expected 23 sites)")
    if counts.pop("vpkd3d128-nan", 0) != 248:
        sys.exit("vpkd3d128 clamp census changed (expected 248 sites)")
    for _mh_tu, mh_spec in MIDHOOK_SPECS.items():
        for _fn, hook_name, _pat, _repl in mh_spec["hooks"]:
            if counts.pop(hook_name, 0) != 1:
                sys.exit(f"midhook {hook_name} census changed (expected 1 site)")
    if counts != EXPECTED:
        sys.exit(f"dropped-op census changed: {counts} != {EXPECTED}")


if __name__ == "__main__":
    main()
