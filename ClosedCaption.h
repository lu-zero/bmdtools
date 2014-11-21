#include <DeckLinkAPI.h>

extern "C" {
#include "libavformat/avformat.h"
}

#define CCLINE 9
#define CC_LINE_BUFFER_MAX 256
class ClosedCaption
{
	/* character translation table - EIA 608 codes are not all the same as ASCII */
	/* use ISO Latin-1 codes for extended, non-ASCII chars */
	char cc_chartbl[128];
	int num_bytes_row;
	int num_words_row;
	/* vertical ancillary data */
	uint16_t *vanc_row;
	int opt_verbosity;
	int opt_cc_ntsc_field;
	unsigned char *pkt_buff;
	int pkt_buff_len;
	void build_char_table(void);
	int parse_vanc_packet (uint16_t *vanc_packet, long words_remaining, AVPacket &pkt);
	int parse_cdp (uint16_t *cdp, long num_words, AVPacket &pkt);
	public:
	ClosedCaption(void);
	~ClosedCaption(void);
	int extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt);
};
