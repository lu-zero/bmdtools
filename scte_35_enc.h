#ifndef __scte_35_enc_H
#define __scte_35_enc_H
#include <stdint.h>

struct insertParam
{
	int32_t event_id;
	uint8_t auto_return;
	uint64_t duration;
	uint8_t time_specified_flag:1;
	uint64_t pts_time;
	uint8_t event_cancel_indicator:1;
	uint8_t out_of_network_indicator:1;
	uint8_t program_splice_flag:1;
	uint8_t duration_flag:1;
	uint8_t splice_immediate_flag:1;
	uint8_t component_count;
	uint8_t component_tag[10];
	uint16_t unique_program_id;
	uint8_t avail_num;
	uint16_t avails_expected;
};
class scte_35_enc
{
	int table_id:8;
	int section_syntax_indicator:1;
	int private_indicator:1;
	int reserved:2;
	int section_length:12;
	int protocol_version:8;
	int encrypted_packet:1;
	int encryption_algorithm:6;
	long long pts_adjustment:33;
	unsigned int cw_index:8;
	unsigned int tier:12;
	unsigned int splice_command_length:12;
	unsigned int splice_command_type:8;
	unsigned int descriptor_loop_length:16;
	unsigned int alignment_stuffing:8;
	unsigned int E_CRC_32:32;
	unsigned int CRC_32:32;

	struct insertParam insert_param;
	int encode_break_duration(uint8_t *q, int len);
	int encode_splice_time(uint8_t *q, int len);
	int encode_splice_schedule(uint8_t *out_buf,int len);
	int encode_splice_insert(uint8_t *out_buf, int len);
	int encode_time_signal(uint8_t *out_buf, int len);
	int encode_bandwidth_reservation(uint8_t *out_buf, int len);
	int encode_private_command(uint8_t *out_buf,int len);
	public:
	scte_35_enc(void);
	int encode( uint8_t *q, int &len, uint8_t commandi);
	int set_insert_type(uint8_t type);
	int set_event_param(uint32_t event_id,
			uint16_t unique_program_id,
			uint16_t pre_roll_time,
			uint16_t break_duration,
			uint8_t avail_num,
			uint8_t  avails_expected,
			uint8_t auto_return_flag);
	void set_scte35_protocol_version(uint8_t protocol_version);

};


#endif
