#include "scte_35_enc.h"
extern "C" {
#include "libavutil/intreadwrite.h"
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
int scte_35_enc::encode_splice_schedule(uint8_t *out_buf,int len)
{
	return 0;
}
int scte_35_enc::encode_splice_insert(uint8_t *out_buf, int len)
{
	return 0;
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
void scte_35_enc::set_insert_param(int32_t event_id)
{
	insert_param.event_id = event_id;
}
int scte_35_enc::encode( unsigned char* out_buf, int &len)
{
	uint64_t bitbuf = 0;
	unsigned char *buf_pivot = out_buf;
	int ret = 0;

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

	len = out_buf - buf_pivot;
	return out_buf - buf_pivot;
}
