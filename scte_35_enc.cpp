#include "scte_35_enc.h"
extern "C" {
#include "libavutil/intreadwrite.h"
#include "libavutil/crc.h"
}
scte_35_enc::scte_35_enc(void)
{
	table_id = 0xfc;
	section_syntax_indicator = 0;

	private_indicator = 0;
	section_length = 17;
	protocol_version = 0;
	encrypted_packet = 0;
	encryption_algorithm = 0;
	pts_adjustment = 0;
	cw_index = 0;
	tier = 0xfff;
	splice_command_length = 0xfff;
	/* initialized with NUll command */
	splice_command_type = 0;
	descriptor_loop_length = 0;

}

int scte_35_enc::set_event_param(uint32_t event_id,
			uint16_t unique_program_id,
			uint16_t pre_roll_time,
			uint16_t break_duration,
			uint8_t avail_num,
			uint8_t  avails_expected,
			uint8_t auto_return_flag)
{
	insert_param.event_id = event_id;
	return 0;
}
int scte_35_enc::encode_break_duration(uint8_t *q, int len)
{
	uint8_t *q_pivot = q;
	uint8_t byte_8 = 0;

	byte_8 |= insert_param.auto_return;
	byte_8 <<= 6;
	byte_8 |= 0x7F;
	byte_8 <<= 1;
	byte_8 |= insert_param.duration >> 32;
	*q = byte_8;
	q++;
	AV_WB32(q, insert_param.duration & 0xFFFFFFFF);
	q += 4;
	return q - q_pivot;
}
int scte_35_enc::encode_splice_time(uint8_t *q, int len)
{
	uint8_t *q_pivot = q;
	uint8_t byte_8 = 0;

	byte_8 |= insert_param.time_specified_flag;

	if(insert_param.time_specified_flag) {
		byte_8 <<= 6;
		byte_8 |= 0x7F;
		byte_8 <<= 1;
		byte_8 |= insert_param.pts_time >> 32;
		*q = byte_8;
		q++;
		AV_WB32(q, insert_param.pts_time & 0xFFFFFFFF);
		q += 4;
	} else {
		byte_8 <<= 7;
		byte_8 |= 0xEF;
		*q = byte_8;
		q++;
	}

	return q - q_pivot;
}

int scte_35_enc::encode_splice_schedule(uint8_t *out_buf,int len)
{
	return 0;
}
int scte_35_enc::encode_splice_insert(uint8_t *q, int len)
{
	uint8_t *q_pivot = q;
	uint8_t byte_8;
	int i = 0;

	/* splice_event_id */
	AV_WB32(q, insert_param.event_id);
	q += 4;

	byte_8 = 0xFF;
	byte_8 &= 0x7F | (insert_param.event_cancel_indicator << 7);
	*q++ = byte_8;

	if (!insert_param.event_cancel_indicator) {
		byte_8 = 0;

		byte_8 |= insert_param.out_of_network_indicator;
		byte_8 <<= 1;
		byte_8 |= insert_param.program_splice_flag;
		byte_8 <<= 1;
		byte_8 |= insert_param.duration_flag;
		byte_8 <<= 1;
		byte_8 |= insert_param.splice_immediate_flag;
		byte_8 <<= 4;
		byte_8 |= 0x0F;
		*q++ = byte_8;
		if(insert_param.program_splice_flag == 1 && insert_param.splice_immediate_flag == 0) {
			q += encode_splice_time(q, len);
		}
		if(insert_param.program_splice_flag == 0) {
			*q = insert_param.component_count;
			q++;
			for ( i = 0; i < insert_param.component_count; i++) {
				*q = insert_param.component_tag[i];
				q++;
				if(insert_param.splice_immediate_flag == 0)
					q += encode_splice_time(q, len);

			}
		}
		if (insert_param.duration_flag == 0) {
			q += encode_break_duration(q, len);
		}
		AV_WB16(q, insert_param.unique_program_id);
		q += 2;
		*q = insert_param.avail_num;
		q++;
		*q = insert_param.avails_expected;
		q++;
	}

	return q - q_pivot;
}
int scte_35_enc::encode_time_signal(uint8_t *out_buf, int len)
{
	return 0;
}
int scte_35_enc::encode_bandwidth_reservation(uint8_t *out_buf, int len)
{
	return 0;
}
int scte_35_enc::encode_private_command(uint8_t *out_buf, int len)
{
	return 0;
}
void scte_35_enc::set_command(unsigned int cmd)
{
	splice_command_type = cmd;
}
static unsigned crc32(const uint8_t *data, unsigned size)
{
	return av_crc(av_crc_get_table(AV_CRC_32_IEEE), -1, data, size);
}

int scte_35_enc::encode( unsigned char* out_buf, int &len)
{
	uint64_t bitbuf = 0;
	unsigned char *buf_pivot = out_buf;
	int ret = 0;
	uint32_t crc;

	*out_buf = 0xfc;
	out_buf++;
	bitbuf <<= 1;
	bitbuf |= section_syntax_indicator;
	bitbuf <<= 1;
	bitbuf |= private_indicator;
	bitbuf <<= 2;
	bitbuf |= 0x3;
	bitbuf <<= 12;
	bitbuf |= section_length;
	bitbuf <<= 8;
	bitbuf |= protocol_version;

	bitbuf <<= 1;
	bitbuf |= encrypted_packet;
	bitbuf <<= 6;
	bitbuf |= encryption_algorithm;
	bitbuf <<= 33;
	bitbuf |= pts_adjustment;
	AV_WB64(out_buf, bitbuf);
	out_buf += 8;

	bitbuf = 0;
	bitbuf |= cw_index;
	bitbuf <<= 12;
	bitbuf |= tier;
	bitbuf <<= 12;
	bitbuf |= splice_command_length;
	AV_WB32(out_buf, bitbuf);
	out_buf += 4;

	*out_buf = splice_command_type;
	out_buf++;
	switch(splice_command_type) {
		case 0x00:
			/* NULL packet do nothing */
			break;
		case 0x04:
			ret = encode_splice_schedule(out_buf, len);
			break;
		case 0x05:
			ret = encode_splice_insert(out_buf, len);
			break;
		case 0x06:
			ret = encode_time_signal(out_buf, len);
			break;
		case 0x07:
			ret = encode_bandwidth_reservation(out_buf, len);
			break;
		case 0xff:
			ret = encode_private_command(out_buf, len);
			break;
	}
	out_buf += ret;
	AV_WB16(out_buf, descriptor_loop_length);
	out_buf += 2;
	crc = crc32(buf_pivot, out_buf - buf_pivot);
	AV_WB32(out_buf, crc);
	out_buf += 4;
	len = out_buf - buf_pivot;
	return out_buf - buf_pivot;
}
