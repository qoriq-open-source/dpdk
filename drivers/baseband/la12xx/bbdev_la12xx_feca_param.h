/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2020 NXP
 */

#ifndef _BBDEV_LA12XX_FECA_PARAM_H_
#define _BBDEV_LA12XX_FECA_PARAM_H_

/**
 * Convert BBDEV parameters for shared encode to FECA parameters
 *
 * @param base_graph2_input
 *   If base_graph2_input = 0 --> means base graph 1 is used.
 *   base_graph2_input = 1 --> means base graph 2 is used (input)
 * @param Q_m
 *   modulation order, Q_m={2,4,6,8} (input)
 * @param e
 *   array of size C (number of code blocks) where each entry has the number
 *   of encoded bits inside each code block. Note that E_0 + E_1 + ... +
 *   E_(C-1) = G_sch (input)
 * @param rv_id
 *   redundancy version ID (input)
 * @param A
 *   transport block payload size. Amin=24, Amax=1213032.
 *   A has to be multiple of (8*C) (input)
 * @param q
 *   parameter for c_init in scrambler, q = 0 or 1 for downlink,
 *   q = 0 for uplink (input)
 * @param n_ID
 *   parameter for c_init in scrambler, n_ID={0, ..., 1023} (input)
 * @param n_RNTI
 *   parameter for c_init in scrambler, n_RNTI={0, ..., 65535} (input)
 * @param scrambler_bypass
 *   when scrambler_bypass = 1, it will bypass scrambler (input)
 * @param N_cb
 *   circular buffer size for rate matching parameter (input)
 * @param codeblock_mask
 *   binary mask to indicate the number of transmitted code blocks.
 *   codeblock_mask is an array of size 8.  If the code block is transmitted,
 *   the corresponding bit is 1, otherwise, it will be zero (input)
 * @param TBS_VALID
 *   if *TBS_VALID=1 --> A is valid number. if *TBS_VALID=0 --> A
 *   is invalid number (output)
 *
 */
void
la12xx_sch_encode_param_convert(int16_t base_graph2_input,
				int16_t Q_m,
				int16_t *e,
				int16_t rv_id,
				int16_t A,
				int16_t q,
				int16_t n_ID,
				int16_t n_RNTI,
				int16_t scrambler_bypass,
				int16_t N_cb,
				int16_t *codeblock_mask,
				int16_t *TBS_VALID,
				// HW parameters
				int16_t *set_index,
				int16_t *base_graph2,
				int16_t *lifting_index,
				int16_t *mod_order,
				int16_t *tb_24_bit_crc,
				int16_t *num_code_blocks,
				int16_t *num_input_bytes,
				int16_t *e_floor_thresh,
				int16_t *num_output_bits_floor,
				int16_t *num_output_bits_ceiling,
				int16_t *SE_SC_X1_INIT,
				int16_t *SE_SC_X2_INIT,
				int16_t *int_start_ofst_floor,
				int16_t *int_start_ofst_ceiling,
				int16_t *SE_CIRC_BUF);

/**
 * Convert BBDEV parameters for shared encode to FECA parameters
 *
 * @param harq_buffer
 *   HARQ buffer (input/output)
 * @param base_graph2_input
 *   If base_graph2_input = 0 --> means base graph 1 is used.
 *   base_graph2_input = 1 --> means base graph 2 is used (input)
 * @param Q_m
 *   modulation order, Q_m={2,4,6,8} (input)
 * @param e
 *   array of size C (number of code blocks) where each entry has the number
 *   of encoded bits inside each code block. Note that E_0 + E_1 + ... +
 *   E_(C-1) = G_sch (input)
 * @param rv_id
 *   redundancy version ID (input)
 * @param A
 *   transport block payload size. Amin=24, Amax=1213032.
 *   A has to be multiple of (8*C) (input)
 * @param q
 *   parameter for c_init in scrambler, q = 0 or 1 for downlink,
 *   q = 0 for uplink (input)
 * @param n_ID
 *   parameter for c_init in scrambler, n_ID={0, ..., 1023} (input)
 * @param n_RNTI
 *   parameter for c_init in scrambler, n_RNTI={0, ..., 65535} (input)
 * @param scrambler_bypass
 *   when scrambler_bypass = 1, it will bypass scrambler (input)
 * @param N_cb
 *   circular buffer size for rate matching parameter (input)
 * @param remove_tb_crc
 *   If 0, transport block CRC will be attached to the decoded bits.
 *   If 1, transport block CRC will be removed from the decoded bits (input)
 * @param harq_en
 *   HARQ enable (input)
 * @param size_harq_buffer
 *   HARQ buffer size (output)
 * @param pC
 *   number of code blocks per transport block (output)
 * @param codeblock_mask
 *   binary mask to indicate the number of transmitted code blocks.
 *   codeblock_mask is an array of size 8.  If the code block is transmitted,
 *   the corresponding bit is 1, otherwise, it will be zero (input)
 * @param TBS_VALID
 *   if *TBS_VALID=1 --> A is valid number. if *TBS_VALID=0 --> A
 *   is invalid number (output)
 *
 */
void
la12xx_sch_decode_param_convert(int16_t *harq_buffer,
				int16_t base_graph2_input,
				int16_t Q_m,
				int16_t *e,
				int16_t rv_id,
				int16_t A,
				int16_t q,
				int16_t n_ID,
				int16_t n_RNTI,
				int16_t scrambler_bypass,
				int16_t N_cb,
				int16_t remove_tb_crc,
				int16_t harq_en,
				int16_t *size_harq_buffer,
				int16_t *pC,
				int16_t *codeblock_mask,
				int16_t *TBS_VALID,
				// HW parameters
				int16_t *set_index,
				int16_t *base_graph2,
				int16_t *lifting_index,
				int16_t *mod_order,
				int16_t *tb_24_bit_crc,
				int16_t *one_code_block,
				int16_t *e_floor_thresh,
				int16_t *num_output_bytes,
				int16_t *bits_per_cb,
				int16_t *num_filler_bits,
				int16_t *SD_SC_X1_INIT,
				int16_t *SD_SC_X2_INIT,
				int16_t *e_div_qm_floor,
				int16_t *e_div_qm_ceiling,
				int16_t *di_start_ofst_floor,
				int16_t *di_start_ofst_ceiling,
				int16_t *SD_CIRC_BUF,
				int16_t *axi_data_num_bytes);

#endif /* BBDEV_LA12XX_FECA_PARAM_H_ */
