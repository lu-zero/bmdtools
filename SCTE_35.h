#ifndef SCTE_35_H
#define SCTE_35_H

extern "C" {
#include "libavformat/avformat.h"
}

#include "VncPacket.h"
#include <fstream>
#include "scte_35_enc.h"
class SCTE_35:public scte_35_enc
{
	uint8_t command;
	bool command_set_flag;
	unsigned char *pkt_buff;
	int pkt_buff_len;
	class VncPacket vnc;
	uint8_t *output;
	int parse_multi_operation_message(const uint8_t *buf, int len);
	int parse_single_operation_message(const uint8_t *buf, int len);
	int parse_timestamp(const uint8_t *buf, int len);
	int parse_splice_request_data(const uint8_t *buf, int len);
	public:
	SCTE_35(void);
	~SCTE_35(void);
	int extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt);
	int parse_scte104message(const uint8_t *buf, int len);
};

#endif
