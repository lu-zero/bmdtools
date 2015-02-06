#include "scte_35_enc.h"
extern "C" {
#include "libavutil/intreadwrite.h"
#include "libavutil/crc.h"
}
#include <stdio.h>
scte_35_enc::scte_35_enc(void)
{
	table_id = 0xfc;
	section_syntax_indicator = 0;

	private_indicator = 0;
	protocol_version = 0;
	encrypted_packet = 0;
	encryption_algorithm = 0;
	pts_adjustment = 0;
	cw_index = 0;
	tier = 0xfff;
	/* initialized with NUll command */
	splice_command_type = 0;
	descriptor_loop_length = 0;
	insert_param.program_splice_flag = 1;
	enc_hack = 1;

}

int scte_35_enc::set_event_param(uint8_t insert_type,
			uint32_t event_id,
			uint16_t unique_program_id,
			uint16_t pre_roll_time,
			uint16_t break_duration,
			uint8_t  avail_num,
			uint8_t  avails_expected,
			uint8_t  auto_return_flag,
			uint64_t pts)
{
	set_insert_type(insert_type);
	if ( insert_type == INSERT_TYPE_SPLICE_START_NORMAL ||
		insert_type == INSERT_TYPE_SPLICE_END_NORMAL) {
		insert_param.pts_time = pts + pre_roll_time;
	} else {
		insert_param.pts_time = pts;
	}
	insert_param.event_id = event_id;
	insert_param.unique_program_id = unique_program_id;
	insert_param.avail_num = avail_num;
	insert_param.avails_expected = avails_expected;
	insert_param.auto_return = auto_return_flag;
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

int scte_35_enc::encode_splice_schedule(uint8_t *q,int len)
{
	uint8_t *q_pivot = q;
	*q = SCTE_35_CMD_SCHEDULE;
	q++;
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
void scte_35_enc::set_scte35_protocol_version(uint8_t protocol_version)
{
	this->protocol_version = protocol_version;
}
int scte_35_enc::set_insert_type(uint8_t type)
{
	if ( this->enc_hack) {
		if(type == 0x01)
			type = 0x02;
		if(type == 0x03 )
			type = 0x04;
	}
	if (type == 0x00) {
	/* DO nothing */
	} else if (type == 0x01) {
		insert_param.event_cancel_indicator = 0;
		insert_param.out_of_network_indicator = 1;
		insert_param.splice_immediate_flag = 0;
	} else if (type == 0x02) {
		insert_param.event_cancel_indicator = 0;
		insert_param.out_of_network_indicator = 1;
		insert_param.splice_immediate_flag = 1;
	} else if (type == 0x03) {
		insert_param.event_cancel_indicator = 0;
		insert_param.out_of_network_indicator = 0;
		insert_param.duration_flag = 0;
		insert_param.splice_immediate_flag = 0;
		insert_param.auto_return = 0;
	} else if (type == 0x04) {
		insert_param.event_cancel_indicator = 0;
		insert_param.out_of_network_indicator = 0;
		insert_param.duration_flag = 0;
		insert_param.splice_immediate_flag = 1;
		insert_param.auto_return = 0;
	} else if (type == 0x05) {
		insert_param.event_cancel_indicator = 1;
		insert_param.out_of_network_indicator = 0;
		insert_param.duration_flag = 0;
		insert_param.splice_immediate_flag = 0;
		insert_param.auto_return = 0;
	} else {
		return -1;
	}

	return 0;

}
static unsigned crc32(const uint8_t *data, unsigned size)
{
	return av_crc(av_crc_get_table(AV_CRC_32_IEEE), -1, data, size);
}

int scte_35_enc::encode_cmd(uint8_t *q, int len, uint8_t command)
{
	int ret = 0;
	switch(command) {
		case SCTE_35_CMD_NULL:
			/* NULL packet do nothing */
			break;
		case SCTE_35_CMD_SCHEDULE:
			ret = encode_splice_schedule(q, len);
			break;
		case SCTE_35_CMD_INSERT:
			ret = encode_splice_insert(q, len);
			break;
		case SCTE_35_CMD_TIME_SIGNAL:
			ret = encode_time_signal(q, len);
			break;
		case SCTE_35_CMD_BW_RESERVE:
			ret = encode_bandwidth_reservation(q, len);
			break;
		case SCTE_35_CMD_PRIVATE:
			ret = encode_private_command(q, len);
			break;
	}
	return ret;
}
int scte_35_enc::encode( uint8_t *q, int &len, uint8_t command)
{
	uint64_t bitbuf = 0;
	const uint8_t *q_pivot = q;
	uint32_t crc;

	int cmd_len;
	int section_len;

	cmd_len = encode_cmd (q + 14, len - 13, command);
	/* Adding 4 bytes for crc
	 * Ignoring Encrtpted and stuff things*/
	section_len = 11 + cmd_len + 6;

	*q++ = 0xfc;

	bitbuf <<= 1;
	bitbuf |= section_syntax_indicator;
	bitbuf <<= 1;
	bitbuf |= private_indicator;
	bitbuf <<= 2;
	bitbuf |= 0x3;
	bitbuf <<= 12;
	bitbuf |= section_len;
	bitbuf <<= 8;
	bitbuf |= protocol_version;

	bitbuf <<= 1;
	bitbuf |= encrypted_packet;
	bitbuf <<= 6;
	bitbuf |= encryption_algorithm;
	bitbuf <<= 33;
	bitbuf |= pts_adjustment;
	AV_WB64(q, bitbuf);
	q += 8;

	bitbuf = 0;
	bitbuf |= cw_index;
	bitbuf <<= 12;
	bitbuf |= tier;
	bitbuf <<= 12;
	bitbuf |= cmd_len;
	AV_WB32(q, bitbuf);
	q += 4;

	*q = command;
	q++;

	q += cmd_len;

	AV_WB16(q, descriptor_loop_length);
	q += 2;

	crc = crc32(q_pivot, q - q_pivot);
	AV_WB32(q, crc);
	q += 4;
	len = q - q_pivot;
	return q - q_pivot;
}
