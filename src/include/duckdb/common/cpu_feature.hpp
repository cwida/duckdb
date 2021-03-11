//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/cpu_feature.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {
enum CPUFeature {
	DUCKDB_CPU_FALLBACK = 0,

	DUCKDB_CPU_FEATURE_CPU_MASK = 0x1f000000,
	DUCKDB_CPU_FEATURE_X86 = 0x01000000,
	DUCKDB_CPU_FEATURE_ARM = 0x04000000,

	/* x86 CPU features are constructed as:
     *
     *   (DUCKDB_CPU_FEATURE_X86 | (eax << 16) | (ret_reg << 8) | (bit_position)
     *
     * For example, SSE3 is determined by the fist bit in the ECX
     * register for a CPUID call with EAX=1, so we get:
     *
     *   DUCKDB_CPU_FEATURE_X86 | (1 << 16) | (2 << 8) | (0) = 0x01010200
     *
     * We should have information for inputs of EAX=0-7 w/ ECX=0.
	 */
	DUCKDB_CPU_FEATURE_X86_FPU = 0x01010300,
	DUCKDB_CPU_FEATURE_X86_VME = 0x01010301,
	DUCKDB_CPU_FEATURE_X86_DE = 0x01010302,
	DUCKDB_CPU_FEATURE_X86_PSE = 0x01010303,
	DUCKDB_CPU_FEATURE_X86_TSC = 0x01010304,
	DUCKDB_CPU_FEATURE_X86_MSR = 0x01010305,
	DUCKDB_CPU_FEATURE_X86_PAE = 0x01010306,
	DUCKDB_CPU_FEATURE_X86_MCE = 0x01010307,
	DUCKDB_CPU_FEATURE_X86_CX8 = 0x01010308,
	DUCKDB_CPU_FEATURE_X86_APIC = 0x01010309,
	DUCKDB_CPU_FEATURE_X86_SEP = 0x0101030b,
	DUCKDB_CPU_FEATURE_X86_MTRR = 0x0101030c,
	DUCKDB_CPU_FEATURE_X86_PGE = 0x0101030d,
	DUCKDB_CPU_FEATURE_X86_MCA = 0x0101030e,
	DUCKDB_CPU_FEATURE_X86_CMOV = 0x0101030f,
	DUCKDB_CPU_FEATURE_X86_PAT = 0x01010310,
	DUCKDB_CPU_FEATURE_X86_PSE_36 = 0x01010311,
	DUCKDB_CPU_FEATURE_X86_PSN = 0x01010312,
	DUCKDB_CPU_FEATURE_X86_CLFSH = 0x01010313,
	DUCKDB_CPU_FEATURE_X86_DS = 0x01010314,
	DUCKDB_CPU_FEATURE_X86_ACPI = 0x01010316,
	DUCKDB_CPU_FEATURE_X86_MMX = 0x01010317,
	DUCKDB_CPU_FEATURE_X86_FXSR = 0x01010318,
	DUCKDB_CPU_FEATURE_X86_SSE = 0x01010319,
	DUCKDB_CPU_FEATURE_X86_SSE2 = 0x0101031a,
	DUCKDB_CPU_FEATURE_X86_SS = 0x0101031b,
	DUCKDB_CPU_FEATURE_X86_HTT = 0x0101031c,
	DUCKDB_CPU_FEATURE_X86_TM = 0x0101031d,
	DUCKDB_CPU_FEATURE_X86_IA64 = 0x0101031e,
	DUCKDB_CPU_FEATURE_X86_PBE = 0x0101031f,

	DUCKDB_CPU_FEATURE_X86_SSE3 = 0x01010200,
	DUCKDB_CPU_FEATURE_X86_PCLMULQDQ = 0x01010201,
	DUCKDB_CPU_FEATURE_X86_DTES64 = 0x01010202,
	DUCKDB_CPU_FEATURE_X86_MONITOR = 0x01010203,
	DUCKDB_CPU_FEATURE_X86_DS_CPL = 0x01010204,
	DUCKDB_CPU_FEATURE_X86_VMX = 0x01010205,
	DUCKDB_CPU_FEATURE_X86_SMX = 0x01010206,
	DUCKDB_CPU_FEATURE_X86_EST = 0x01010207,
	DUCKDB_CPU_FEATURE_X86_TM2 = 0x01010208,
	DUCKDB_CPU_FEATURE_X86_SSSE3 = 0x01010209,
	DUCKDB_CPU_FEATURE_X86_CNXT_ID = 0x0101020a,
	DUCKDB_CPU_FEATURE_X86_SDBG = 0x0101020b,
	DUCKDB_CPU_FEATURE_X86_FMA = 0x0101020c,
	DUCKDB_CPU_FEATURE_X86_CX16 = 0x0101020d,
	DUCKDB_CPU_FEATURE_X86_XTPR = 0x0101020e,
	DUCKDB_CPU_FEATURE_X86_PDCM = 0x0101020f,
	DUCKDB_CPU_FEATURE_X86_PCID = 0x01010211,
	DUCKDB_CPU_FEATURE_X86_DCA = 0x01010212,
	DUCKDB_CPU_FEATURE_X86_SSE4_1 = 0x01010213,
	DUCKDB_CPU_FEATURE_X86_SSE4_2 = 0x01010214,
	DUCKDB_CPU_FEATURE_X86_X2APIC = 0x01010215,
	DUCKDB_CPU_FEATURE_X86_MOVBE = 0x01010216,
	DUCKDB_CPU_FEATURE_X86_POPCNT = 0x01010217,
	DUCKDB_CPU_FEATURE_X86_TSC_DEADLINE = 0x01010218,
	DUCKDB_CPU_FEATURE_X86_AES = 0x01010219,
	DUCKDB_CPU_FEATURE_X86_XSAVE = 0x0101021a,
	DUCKDB_CPU_FEATURE_X86_OSXSAVE = 0x0101021b,
	DUCKDB_CPU_FEATURE_X86_AVX = 0x0101021c,
	DUCKDB_CPU_FEATURE_X86_F16C = 0x0101021d,
	DUCKDB_CPU_FEATURE_X86_RDRND = 0x0101021e,
	DUCKDB_CPU_FEATURE_X86_HYPERVISOR = 0x0101021f,

	DUCKDB_CPU_FEATURE_X86_FSGSBASE = 0x01070100,
	DUCKDB_CPU_FEATURE_X86_TSC_ADJ = 0x01070101,
	DUCKDB_CPU_FEATURE_X86_SGX = 0x01070102,
	DUCKDB_CPU_FEATURE_X86_BMI1 = 0x01070103,
	DUCKDB_CPU_FEATURE_X86_HLE = 0x01070104,
	DUCKDB_CPU_FEATURE_X86_AVX2 = 0x01070105,
	DUCKDB_CPU_FEATURE_X86_SMEP = 0x01070107,
	DUCKDB_CPU_FEATURE_X86_BMI2 = 0x01070108,
	DUCKDB_CPU_FEATURE_X86_ERMS = 0x01070109,
	DUCKDB_CPU_FEATURE_X86_INVPCID = 0x0107010a,
	DUCKDB_CPU_FEATURE_X86_RTM = 0x0107010b,
	DUCKDB_CPU_FEATURE_X86_PQM = 0x0107010c,
	DUCKDB_CPU_FEATURE_X86_MPX = 0x0107010e,
	DUCKDB_CPU_FEATURE_X86_PQE = 0x0107010f,
	DUCKDB_CPU_FEATURE_X86_AVX512F = 0x01070110,
	DUCKDB_CPU_FEATURE_X86_AVX512DQ = 0x01070111,
	DUCKDB_CPU_FEATURE_X86_RDSEED = 0x01070112,
	DUCKDB_CPU_FEATURE_X86_ADX = 0x01070113,
	DUCKDB_CPU_FEATURE_X86_SMAP = 0x01070114,
	DUCKDB_CPU_FEATURE_X86_AVX512IFMA = 0x01070115,
	DUCKDB_CPU_FEATURE_X86_PCOMMIT = 0x01070116,
	DUCKDB_CPU_FEATURE_X86_CLFLUSHOPT = 0x01070117,
	DUCKDB_CPU_FEATURE_X86_CLWB = 0x01070118,
	DUCKDB_CPU_FEATURE_X86_INTEL_PT = 0x01070119,
	DUCKDB_CPU_FEATURE_X86_AVX512PF = 0x0107011a,
	DUCKDB_CPU_FEATURE_X86_AVX512ER = 0x0107011b,
	DUCKDB_CPU_FEATURE_X86_AVX512CD = 0x0107011c,
	DUCKDB_CPU_FEATURE_X86_SHA = 0x0107011d,
	DUCKDB_CPU_FEATURE_X86_AVX512BW = 0x0107011e,
	DUCKDB_CPU_FEATURE_X86_AVX512VL = 0x0107011f,

	DUCKDB_CPU_FEATURE_X86_PREFETCHWT1 = 0x01070200,
	DUCKDB_CPU_FEATURE_X86_AVX512VBMI = 0x01070201,
	DUCKDB_CPU_FEATURE_X86_UMIP = 0x01070202,
	DUCKDB_CPU_FEATURE_X86_PKU = 0x01070203,
	DUCKDB_CPU_FEATURE_X86_OSPKE = 0x01070204,
	DUCKDB_CPU_FEATURE_X86_AVX512VPOPCNTDQ = 0x0107020e,
	DUCKDB_CPU_FEATURE_X86_RDPID = 0x01070215,
	DUCKDB_CPU_FEATURE_X86_SGX_LC = 0x0107021e,

	DUCKDB_CPU_FEATURE_X86_AVX512_4VNNIW = 0x01070302,
	DUCKDB_CPU_FEATURE_X86_AVX512_4FMAPS = 0x01070303,

	DUCKDB_CPU_FEATURE_ARM_SWP = DUCKDB_CPU_FEATURE_ARM | 1,
	DUCKDB_CPU_FEATURE_ARM_HALF = DUCKDB_CPU_FEATURE_ARM | 2,
	DUCKDB_CPU_FEATURE_ARM_THUMB = DUCKDB_CPU_FEATURE_ARM | 3,
	DUCKDB_CPU_FEATURE_ARM_26BIT = DUCKDB_CPU_FEATURE_ARM | 4,
	DUCKDB_CPU_FEATURE_ARM_FAST_MULT = DUCKDB_CPU_FEATURE_ARM | 5,
	DUCKDB_CPU_FEATURE_ARM_FPA = DUCKDB_CPU_FEATURE_ARM | 6,
	DUCKDB_CPU_FEATURE_ARM_VFP = DUCKDB_CPU_FEATURE_ARM | 7,
	DUCKDB_CPU_FEATURE_ARM_EDSP = DUCKDB_CPU_FEATURE_ARM | 8,
	DUCKDB_CPU_FEATURE_ARM_JAVA = DUCKDB_CPU_FEATURE_ARM | 9,
	DUCKDB_CPU_FEATURE_ARM_IWMMXT = DUCKDB_CPU_FEATURE_ARM | 10,
	DUCKDB_CPU_FEATURE_ARM_CRUNCH = DUCKDB_CPU_FEATURE_ARM | 11,
	DUCKDB_CPU_FEATURE_ARM_THUMBEE = DUCKDB_CPU_FEATURE_ARM | 12,
	DUCKDB_CPU_FEATURE_ARM_NEON = DUCKDB_CPU_FEATURE_ARM | 13,
	DUCKDB_CPU_FEATURE_ARM_VFPV3 = DUCKDB_CPU_FEATURE_ARM | 14,
	DUCKDB_CPU_FEATURE_ARM_VFPV3D16 = DUCKDB_CPU_FEATURE_ARM | 15,
	DUCKDB_CPU_FEATURE_ARM_TLS = DUCKDB_CPU_FEATURE_ARM | 16,
	DUCKDB_CPU_FEATURE_ARM_VFPV4 = DUCKDB_CPU_FEATURE_ARM | 17,
	DUCKDB_CPU_FEATURE_ARM_IDIVA = DUCKDB_CPU_FEATURE_ARM | 18,
	DUCKDB_CPU_FEATURE_ARM_IDIVT = DUCKDB_CPU_FEATURE_ARM | 19,
	DUCKDB_CPU_FEATURE_ARM_VFPD32 = DUCKDB_CPU_FEATURE_ARM | 20,
	DUCKDB_CPU_FEATURE_ARM_LPAE = DUCKDB_CPU_FEATURE_ARM | 21,
	DUCKDB_CPU_FEATURE_ARM_EVTSTRM = DUCKDB_CPU_FEATURE_ARM | 22,

	DUCKDB_CPU_FEATURE_ARM_AES = DUCKDB_CPU_FEATURE_ARM | 0x0100 | 1,
	DUCKDB_CPU_FEATURE_ARM_PMULL = DUCKDB_CPU_FEATURE_ARM | 0x0100 | 2,
	DUCKDB_CPU_FEATURE_ARM_SHA1 = DUCKDB_CPU_FEATURE_ARM | 0x0100 | 3,
	DUCKDB_CPU_FEATURE_ARM_SHA2 = DUCKDB_CPU_FEATURE_ARM | 0x0100 | 4,
	DUCKDB_CPU_FEATURE_ARM_CRC32 = DUCKDB_CPU_FEATURE_ARM | 0x0100 | 5
};

static std::unordered_map<std::string, CPUFeature> const table = {
    {"DUCKDB_CPU_FEATURE_X86_AVX2", DUCKDB_CPU_FEATURE_X86_AVX2},
    {"DUCKDB_CPU_FEATURE_X86_AVX512F", DUCKDB_CPU_FEATURE_X86_AVX512F},
    {"DUCKDB_CPU_FALLBACK", DUCKDB_CPU_FALLBACK}
};

static string CPUFeatureToString(CPUFeature feature) {
    switch (feature) {
	case CPUFeature::DUCKDB_CPU_FALLBACK:
        return "DUCKDB_CPU_FALLBACK";
	case CPUFeature::DUCKDB_CPU_FEATURE_X86_AVX2:
        return "DUCKDB_CPU_FEATURE_X86_AVX2";
    case CPUFeature::DUCKDB_CPU_FEATURE_X86_AVX512F:
        return "DUCKDB_CPU_FEATURE_X86_AVX512F";
    default:
        return "UNDEFINED";
    }
}

} // namespace duckdb