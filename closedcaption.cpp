#include <ClosedCaption.h>
#include <sys/time.h>
#include <iostream>

#define CC_DID 0x161
#define EIA608_SID 0x101

ClosedCaption::ClosedCaption(void)
{
	pkt_buff = NULL;
	pkt_buff_len = 0;
	opt_verbosity = 0;
	opt_cc_ntsc_field = 1;
	vnc.set_param(9, CC_DID, EIA608_SID);
}
ClosedCaption::~ClosedCaption(void)
{
	if (pkt_buff)
		free(pkt_buff);
}
int ClosedCaption::extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt)
{
	uint16_t *data = NULL;
	int ret;

	ret = vnc.extract(arrivedFrame, data);
	if(ret < 0 )
		return ret;

	ret = parse_cdp(data, ret, pkt);

	return ret;
	
}
int ClosedCaption::parse_cdp (uint16_t *cdp, long num_words, AVPacket &pkt)
{
	if (!((cdp[0] == 0x296) && (cdp[1] == 0x269)))
	{
		std::cout << "[parse_cdp] could not find CDP identifier of 0x296 0x269\n";
		return -1;
	}

	uint16_t cdp_length = cdp[2] & 0xff;

	uint16_t cdp_framerate = (cdp[3] & 0xf0) >> 4 ;
	uint16_t cc_data_present = (cdp[4] & 0x40) >> 6;
	uint16_t caption_service_active = (cdp[4] & 0x02) >> 1;
	uint16_t cdp_header_sequence_counter = ((cdp[5] & 0xff) << 8) + (cdp[6] & 0xff);

	if (opt_verbosity > 2)
	{
		std::cout << "[parse_cdp] CDP length: " << std::dec << cdp_length << " words\n";
		std::cout << "[parse_cdp] CDP frame rate: 0x" << std::hex << cdp_framerate << "\n";
		std::cout << "[parse_cdp] CC data present: " << std::dec << cc_data_present << "\n";
		std::cout << "[parse_cdp] caption service active: " << caption_service_active << "\n";
		std::cout << "[parse_cdp] header sequence counter: " 
			<< std::dec << cdp_header_sequence_counter 
			<< " (0x" << std::hex << cdp_header_sequence_counter << ")\n";
	}

	if (cdp[7] != 0x272)
	{
		// @fixme: deal witih the following types of data sections:
		//   - timecode (0x71, 5 words, including section id)
		//   - service information (0x73, 9 words, including section id)
		//   - footer (0x74, 4 words, including section id)
		if (opt_verbosity > 2)
		{
			std::cout << "[parse_cdp] could not find CC data section id of 0x272\n";
		}
		return -1;
	}

	uint16_t cc_count = cdp[8] & 0x1f;

	if (opt_verbosity > 2)
	{
		std::cout << "[parse_cdp] cc_count: " << std::dec << cc_count << "\n";
	}
	if( pkt_buff_len < cc_count * 3 )
	{
		pkt_buff_len = cc_count * 3;
		pkt_buff = (unsigned char*)realloc(pkt_buff,cc_count * 3);
		if (pkt_buff == NULL)
			std::cerr << "Unable to allocate Memory\n";

	}

	for (int i = 0; i < cc_count * 3;i++)
	{
		pkt_buff[i] = cdp[9 + i] & 0xff;
	}
	pkt.size = cc_count * 3;
	pkt.data = pkt_buff;

	return pkt.size;
}
