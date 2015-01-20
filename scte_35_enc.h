#ifndef __scte_35_enc_H
#define __scte_35_enc_H
#include <stdint.h>
struct insertParam
{
	int32_t event_id;
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
	int encode_splice_schedule(uint8_t *out_buf,int len);
	int encode_splice_insert(uint8_t *out_buf, int len);
	int encode_time_signal(uint8_t *out_buf, int len);
	int encode_bandwidth_reservation(uint8_t *out_buf, int len);
	int encode_private_command(uint8_t *out_buf,int len);
	public:
	scte_35_enc(void);
	int encode( unsigned char* out_buf, int &len);
	void set_command(unsigned int cmd);
	void set_insert_param(int32_t evennt_id);

};


#endif
