#ifndef SCTE_35_H
#define SCTE_35_H

extern "C" {
#include "libavformat/avformat.h"
}

#include "VncPacket.h"
#include <fstream>
class SCTE_35
{
	unsigned char *pkt_buff;
	int pkt_buff_len;
	class VncPacket vnc;
	std::ofstream file;
	std::ofstream log;
	FILE *hexdump;
	FILE *hexdump_trunc;
	public:
	SCTE_35(void);
	~SCTE_35(void);
	int extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt);
	int parse_scte104message(const uint8_t *buf);
};

#endif
