#ifndef CLOSED_CAPTION_H
#define CLOSED_CAPTION_H

extern "C" {
#include "libavformat/avformat.h"
}

#include "VncPacket.h"
#define CCLINE 9
class ClosedCaption
{
	int opt_verbosity;
	int opt_cc_ntsc_field;
	unsigned char *pkt_buff;
	int pkt_buff_len;
	class VncPacket vnc;
	int parse_cdp (uint16_t *data, long len, AVPacket &pkt);
	public:
	ClosedCaption(void);
	~ClosedCaption(void);
	int extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt);
};

#endif
