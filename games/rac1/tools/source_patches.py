# Small text fixes applied to the decomp's C before it is compiled for the PC.
# Each is (file name, exact old text, new text). They are for places where the
# C is right for the EE calling convention but not for x86.
PATCHES = [
    # func_003894A0(colour a, colour b, float t): on the EE the float travels in
    # $f12 whatever its position, so these files declare it first. On x86 the
    # order matters; the macro puts the arguments back in definition order.
    ("3A5478.c", "extern s32 func_003894A0(f32, s32, s32);",
     "extern s32 func_003894A0(s32, s32, f32);\n#define func_003894A0(f, a, b) func_003894A0((a), (b), (f))"),
    ("3B5BF8.c", "extern s32 func_003894A0(f32, s32, s32);",
     "extern s32 func_003894A0(s32, s32, f32);\n#define func_003894A0(f, a, b) func_003894A0((a), (b), (f))"),
    # func_0038C3C0(a, unused, c, f) calls func_00389D18(str, len, scale) with
    # only the float: on the EE, str and len are still a and unused in $a0/$a1.
    ("3896E8.c", "t = ((s32 (*)(f32))func_00389D18)(1.0f);",
     "t = ((s32 (*)(u8 *, s32, f32))func_00389D18)((u8 *)a, unused, 1.0f);"),
    # func_0038C9B8's second and seventh arguments are 64-bit on the EE (one
    # register each) and are passed on as such to func_0038B1E8; its only
    # caller (3E5378.c) passes them as 64-bit values.
    ("3896E8.c", "void func_0038C9B8(s32 a, s32 b, s32 c, s32 d, s32 e, s32 g, s32 h, f32 x) {",
     "void func_0038C9B8(s32 a, long b, s32 c, s32 d, s32 e, s32 g, unsigned long h, f32 x) {"),
    # RAC1. func_L00_001FE940(u128 *v, ...) copies *v with a 16-byte load; its
    # caller (func_L00_002A7078) passes a float[4] member of a local union, which
    # the EE stack frame keeps 16-byte aligned and the x86 one does not: copy it
    # as four words (the pointer goes through an integer so that the compiler
    # does not assume the vector type's alignment for it).
    ("drawquad_001FD1D8.c", "    *tp = *v;\n",
     "    { const unsigned int *s_ = (const unsigned int *)(unsigned int)v; unsigned int *d_ = (unsigned int *)(unsigned int)tp;\n"
     "      d_[0] = s_[0]; d_[1] = s_[1]; d_[2] = s_[2]; d_[3] = s_[3]; }\n"),
    # func_L00_002EABD0 (a camera rig's aim) divides by distances that are 0
    # on the first frame; the EE gives +-MAX there and FastArcSin a finite
    # angle, x86 gives inf/NaN and the camera's matrices stay NaN (nothing of
    # Veldin is drawn once the gameplay camera takes over)
    ("vendor_002E1660.c", "    y = func_001F9FC0(e1 / x);\n", "    y = func_001F9FC0(ee_fdiv(e1, x));\n"),
    ("vendor_002E1660.c", "    r = func_001F9FC0(h / bc);\n", "    r = func_001F9FC0(ee_fdiv(h, bc));\n"),
    ("vendor_002E1660.c", "    r = 1.5707964f - func_001F9FC0(e / x);\n", "    r = 1.5707964f - func_001F9FC0(ee_fdiv(e, x));\n"),
    # func_L00_001EB6A8 is void in the C but ends with FastAddRots(a, *p), whose
    # result stays in $f0 on the EE; every caller (the camera rigs) uses it as
    # the function's float result. On x86 a void function returns nothing (the
    # callers read an empty x87 register: NaN in the camera's matrices).
    ("camera_001EB508.c", "void func_L00_001EB6A8(float *p, float a, float b, float c, float d, float lim) {",
     "float func_L00_001EB6A8(float *p, float a, float b, float c, float d, float lim) {"),
    ("camera_001EB508.c", "        *p = -func_001F9B88(r);\n    }\n    func_001FA748(a, *p);\n}",
     "        *p = -func_001F9B88(r);\n    }\n    return func_001FA748(a, *p);\n}"),
    # func_001F7EF8 (drawquad, hand-written) reads its caller's quad as one
    # block: 4 positions at +0, the colours at +64, the UVs at +80 and the GS
    # words at +112 -- on the EE these callers' locals sit in that order in the
    # frame. On x86 separate locals are laid out as the compiler likes, so they
    # are put in one struct here.
    ("draw.c", "    FRow m[4];\n    int colors[4];\n    float uv[4][2];\n    unsigned long pkt[4];\n",
     "    struct { FRow m[4]; int colors[4]; float uv[4][2]; unsigned long pkt[4]; } q4c30_;\n"
     "#define m q4c30_.m\n#define colors q4c30_.colors\n#define uv q4c30_.uv\n#define pkt q4c30_.pkt\n"),
    ("draw.c", "        func_001F7EF8(m, 0, 0);\n    }\n}\n",
     "        func_001F7EF8(m, 0, 0);\n    }\n}\n#undef m\n#undef colors\n#undef uv\n#undef pkt\n"),
    ("space.c", "    Vec4 quad_positions[4];\n    int colors[4];\n    float texture_coordinates[4][2];\n    u64 quad_state[4];\n",
     "    struct { Vec4 quad_positions[4]; int colors[4]; float texture_coordinates[4][2]; u64 quad_state[4]; } q33ab8_;\n"
     "#define quad_positions q33ab8_.quad_positions\n#define colors q33ab8_.colors\n#define texture_coordinates q33ab8_.texture_coordinates\n#define quad_state q33ab8_.quad_state\n"),
    ("space.c", "        func_001F7EF8(quad_positions, 0, 0);\n    }\n}\n",
     "        func_001F7EF8(quad_positions, 0, 0);\n    }\n}\n#undef quad_positions\n#undef colors\n#undef texture_coordinates\n#undef quad_state\n"),
]
