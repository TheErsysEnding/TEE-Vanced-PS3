/* TEE VP9 building block - made by tools/make_baustein.sh from common/vp9_tables.h; do not edit here: change the
   project's file and run the script again. */
/* TEE VP9 SPU Test: VP9's constants and tables. The enum values are libvpx's (the tables in vp9_tables.c are copied
   from libvpx by tools/extract_tables.py and use these names); the declarations are ours. */
#ifndef VP9_TABLES_H
#define VP9_TABLES_H

#include <stdint.h>

#define TREE_SIZE(leaf_count) (2 * (leaf_count) - 2)

/* prediction modes */
#define DC_PRED      0
#define V_PRED       1
#define H_PRED       2
#define D45_PRED     3
#define D135_PRED    4
#define D117_PRED    5
#define D153_PRED    6
#define D207_PRED    7
#define D63_PRED     8
#define TM_PRED      9
#define NEARESTMV    10
#define NEARMV       11
#define ZEROMV       12
#define NEWMV        13
#define MB_MODE_COUNT 14
#define INTRA_MODES  (TM_PRED + 1)
#define INTER_MODES  (1 + NEWMV - NEARESTMV)
#define INTER_OFFSET(mode) ((mode) - NEARESTMV)

/* block sizes */
enum { BLOCK_4X4, BLOCK_4X8, BLOCK_8X4, BLOCK_8X8, BLOCK_8X16, BLOCK_16X8, BLOCK_16X16, BLOCK_16X32, BLOCK_32X16,
       BLOCK_32X32, BLOCK_32X64, BLOCK_64X32, BLOCK_64X64, BLOCK_SIZES, BLOCK_INVALID = BLOCK_SIZES };
enum { PARTITION_NONE, PARTITION_HORZ, PARTITION_VERT, PARTITION_SPLIT, PARTITION_TYPES };
#define PARTITION_PLOFFSET 4
#define PARTITION_CONTEXTS (4 * PARTITION_PLOFFSET)
#define BLOCK_SIZE_GROUPS  4

/* transforms */
enum { TX_4X4, TX_8X8, TX_16X16, TX_32X32, TX_SIZES };
enum { ONLY_4X4, ALLOW_8X8, ALLOW_16X16, ALLOW_32X32, TX_MODE_SELECT, TX_MODES };
enum { DCT_DCT, ADST_DCT, DCT_ADST, ADST_ADST, TX_TYPES };
#define TX_SIZE_CONTEXTS 2

/* coefficient tokens and their probability model */
#define ZERO_TOKEN       0
#define ONE_TOKEN        1
#define TWO_TOKEN        2
#define THREE_TOKEN      3
#define FOUR_TOKEN       4
#define CATEGORY1_TOKEN  5
#define CATEGORY2_TOKEN  6
#define CATEGORY3_TOKEN  7
#define CATEGORY4_TOKEN  8
#define CATEGORY5_TOKEN  9
#define CATEGORY6_TOKEN  10
#define EOB_TOKEN        11
#define ENTROPY_TOKENS   12
#define ENTROPY_NODES    11
#define UNCONSTRAINED_NODES 3
#define MODEL_NODES      (ENTROPY_NODES - UNCONSTRAINED_NODES)
#define COEFF_PROB_MODELS 255
#define PLANE_TYPES      2
#define REF_TYPES        2
#define COEF_BANDS       6
#define COEFF_CONTEXTS   6
#define MAX_NEIGHBORS    2

/* mode info contexts */
#define INTER_MODE_CONTEXTS  7
#define INTRA_INTER_CONTEXTS 4
#define COMP_INTER_CONTEXTS  5
#define REF_CONTEXTS         5
#define SKIP_CONTEXTS        3

/* interpolation filters */
#define EIGHTTAP           0
#define EIGHTTAP_SMOOTH    1
#define EIGHTTAP_SHARP     2
#define SWITCHABLE_FILTERS 3
#define BILINEAR           3
#define SWITCHABLE         4
#define SWITCHABLE_FILTER_CONTEXTS (SWITCHABLE_FILTERS + 1)
#define SUBPEL_BITS        4
#define SUBPEL_SHIFTS      (1 << SUBPEL_BITS)
#define SUBPEL_TAPS        8

/* motion vectors */
enum { MV_JOINT_ZERO, MV_JOINT_HNZVZ, MV_JOINT_HZVNZ, MV_JOINT_HNZVNZ, MV_JOINTS };
enum { MV_CLASS_0, MV_CLASS_1, MV_CLASS_2, MV_CLASS_3, MV_CLASS_4, MV_CLASS_5, MV_CLASS_6, MV_CLASS_7, MV_CLASS_8,
       MV_CLASS_9, MV_CLASS_10, MV_CLASSES };
#define CLASS0_BITS     1
#define CLASS0_SIZE     (1 << CLASS0_BITS)
#define MV_OFFSET_BITS  (MV_CLASSES + CLASS0_BITS - 2)
#define MV_FP_SIZE      4
#define MV_IN_USE_BITS  14
#define MV_LOW          (-(1 << MV_IN_USE_BITS))
#define MV_UPP          ((1 << MV_IN_USE_BITS) - 1)
#define MV_BORDER       (16 << 3)
#define COMPANDED_MVREF_THRESH 8
#define MVREF_NEIGHBOURS 8
#define MAX_MV_REF_CANDIDATES 2
enum { BOTH_ZERO = 0, ZERO_PLUS_PREDICTED = 1, BOTH_PREDICTED = 2, NEW_PLUS_NON_INTRA = 3, BOTH_NEW = 4,
       INTRA_PLUS_NON_INTRA = 5, BOTH_INTRA = 6, INVALID_CASE = 9 };

/* frames, segments, quantizer, probabilities */
#define INTRA_FRAME   0
#define LAST_FRAME    1
#define GOLDEN_FRAME  2
#define ALTREF_FRAME  3
#define MAX_REF_FRAMES 4
#define NUM_REF_FRAMES 8
#define MAX_SEGMENTS  8
#define SEG_TREE_PROBS (MAX_SEGMENTS - 1)
#define PREDICTION_PROBS 3
#define SEG_LVL_ALT_Q  0
#define SEG_LVL_ALT_LF 1
#define SEG_LVL_REF_FRAME 2
#define SEG_LVL_SKIP   3
#define SEG_LVL_MAX    4
#define MAX_LOOP_FILTER 63
#define QINDEX_RANGE  256
#define MAX_PROB      255
#define DIFF_UPDATE_PROB 252

typedef struct {
	uint8_t p32x32[TX_SIZE_CONTEXTS][TX_SIZES - 1];
	uint8_t p16x16[TX_SIZE_CONTEXTS][TX_SIZES - 2];
	uint8_t p8x8[TX_SIZE_CONTEXTS][TX_SIZES - 3];
} vp9_tx_probs;

typedef struct {
	uint8_t sign;
	uint8_t classes[MV_CLASSES - 1];
	uint8_t class0[CLASS0_SIZE - 1];
	uint8_t bits[MV_OFFSET_BITS];
	uint8_t class0_fp[CLASS0_SIZE][MV_FP_SIZE - 1];
	uint8_t fp[MV_FP_SIZE - 1];
	uint8_t class0_hp;
	uint8_t hp;
} vp9_nmv_component;

typedef struct {
	uint8_t joints[MV_JOINTS - 1];
	vp9_nmv_component comps[2];
} vp9_nmv_context;

typedef struct {
	int row, col;
} vp9_position;

extern const uint8_t vp9_cat6_prob[14];
extern const uint8_t vp9_coefband_trans_8x8plus[1024];
extern const uint8_t vp9_coefband_trans_4x4[16];
extern const uint8_t vp9_pt_energy_class[ENTROPY_TOKENS];
extern const uint8_t vp9_pareto8_full[COEFF_PROB_MODELS][MODEL_NODES];
extern const uint8_t vp9_default_coef_probs_4x4[2][2][6][6][3];
extern const uint8_t vp9_default_coef_probs_8x8[2][2][6][6][3];
extern const uint8_t vp9_default_coef_probs_16x16[2][2][6][6][3];
extern const uint8_t vp9_default_coef_probs_32x32[2][2][6][6][3];
extern const int8_t vp9_coef_con_tree[TREE_SIZE(ENTROPY_TOKENS)];
extern const uint8_t vp9_kf_y_mode_prob[INTRA_MODES][INTRA_MODES][INTRA_MODES - 1];
extern const uint8_t vp9_kf_uv_mode_prob[INTRA_MODES][INTRA_MODES - 1];
extern const uint8_t vp9_default_if_y_probs[BLOCK_SIZE_GROUPS][INTRA_MODES - 1];
extern const uint8_t vp9_default_if_uv_probs[INTRA_MODES][INTRA_MODES - 1];
extern const uint8_t vp9_kf_partition_probs[PARTITION_CONTEXTS][PARTITION_TYPES - 1];
extern const uint8_t vp9_default_partition_probs[PARTITION_CONTEXTS][PARTITION_TYPES - 1];
extern const uint8_t vp9_default_inter_mode_probs[INTER_MODE_CONTEXTS][INTER_MODES - 1];
extern const int8_t vp9_intra_mode_tree[TREE_SIZE(INTRA_MODES)];
extern const int8_t vp9_inter_mode_tree[TREE_SIZE(INTER_MODES)];
extern const int8_t vp9_partition_tree[TREE_SIZE(PARTITION_TYPES)];
extern const uint8_t vp9_default_intra_inter_p[INTRA_INTER_CONTEXTS];
extern const uint8_t vp9_default_comp_inter_p[COMP_INTER_CONTEXTS];
extern const uint8_t vp9_default_comp_ref_p[REF_CONTEXTS];
extern const uint8_t vp9_default_single_ref_p[REF_CONTEXTS][2];
extern const vp9_tx_probs vp9_default_tx_probs;
extern const uint8_t vp9_default_skip_probs[SKIP_CONTEXTS];
extern const int8_t vp9_switchable_interp_tree[TREE_SIZE(SWITCHABLE_FILTERS)];
extern const uint8_t vp9_default_switchable_interp_prob[SWITCHABLE_FILTER_CONTEXTS][SWITCHABLE_FILTERS - 1];
extern const int8_t vp9_mv_joint_tree[TREE_SIZE(MV_JOINTS)];
extern const int8_t vp9_mv_class_tree[TREE_SIZE(MV_CLASSES)];
extern const int8_t vp9_mv_class0_tree[TREE_SIZE(CLASS0_SIZE)];
extern const int8_t vp9_mv_fp_tree[TREE_SIZE(MV_FP_SIZE)];
extern const vp9_nmv_context vp9_default_nmv_context;
extern const int16_t vp9_default_scan_4x4[16], vp9_col_scan_4x4[16], vp9_row_scan_4x4[16];
extern const int16_t vp9_default_scan_8x8[64], vp9_col_scan_8x8[64], vp9_row_scan_8x8[64];
extern const int16_t vp9_default_scan_16x16[256], vp9_col_scan_16x16[256], vp9_row_scan_16x16[256];
extern const int16_t vp9_default_scan_32x32[1024];
extern const int16_t vp9_default_scan_4x4_neighbors[17 * MAX_NEIGHBORS], vp9_col_scan_4x4_neighbors[17 * MAX_NEIGHBORS],
	vp9_row_scan_4x4_neighbors[17 * MAX_NEIGHBORS];
extern const int16_t vp9_default_scan_8x8_neighbors[65 * MAX_NEIGHBORS], vp9_col_scan_8x8_neighbors[65 * MAX_NEIGHBORS],
	vp9_row_scan_8x8_neighbors[65 * MAX_NEIGHBORS];
extern const int16_t vp9_default_scan_16x16_neighbors[257 * MAX_NEIGHBORS], vp9_col_scan_16x16_neighbors[257 * MAX_NEIGHBORS],
	vp9_row_scan_16x16_neighbors[257 * MAX_NEIGHBORS];
extern const int16_t vp9_default_scan_32x32_neighbors[1025 * MAX_NEIGHBORS];
extern const int16_t vp9_bilinear_filters[SUBPEL_SHIFTS][SUBPEL_TAPS];
extern const int16_t vp9_sub_pel_filters_8[SUBPEL_SHIFTS][SUBPEL_TAPS];
extern const int16_t vp9_sub_pel_filters_8s[SUBPEL_SHIFTS][SUBPEL_TAPS];
extern const int16_t vp9_sub_pel_filters_8lp[SUBPEL_SHIFTS][SUBPEL_TAPS];
extern const int16_t vp9_dc_qlookup[QINDEX_RANGE];
extern const int16_t vp9_ac_qlookup[QINDEX_RANGE];
extern const uint8_t vp9_inv_map_table[MAX_PROB];
extern const int8_t vp9_mode_2_counter[MB_MODE_COUNT];
extern const int8_t vp9_counter_to_context[19];
extern const vp9_position vp9_mv_ref_blocks[BLOCK_SIZES][MVREF_NEIGHBOURS];

#endif
