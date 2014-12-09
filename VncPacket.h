#ifndef VNC_PACKET_H
#define VNC_PACKET_H

#include <DeckLinkAPI.h>
class VncPacket
{
	/* vertical ancillary data */
	uint16_t *vanc_row;
	int num_bytes_row;
	int num_words_row;
	int vertical_line;
	int16_t did;
	int16_t sdid;
	public:
	VncPacket(void);
	~VncPacket(void);
	int extract(IDeckLinkVideoInputFrame* arrivedFrame, uint16_t* &data);
	int set_param(int vline, int16_t did, int16_t sdid);
	int parse_vanc_packet (uint16_t *vanc_packet, long words_remaining, uint16_t* &data);
};

#endif
