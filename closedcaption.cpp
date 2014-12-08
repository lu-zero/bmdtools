#include <ClosedCaption.h>
#include <iostream>
#include <sys/time.h>

extern enum PixelFormat pix_fmt;

ClosedCaption::ClosedCaption(void)
{
	num_bytes_row = 0;
	num_words_row = 0;
	vanc_row = NULL;
	pkt_buff = NULL;
	pkt_buff_len = 0;
	opt_verbosity = 0;
	opt_cc_ntsc_field = 1;
}
ClosedCaption::~ClosedCaption(void)
{
	if (pkt_buff)
		free(pkt_buff);
	if(vanc_row)
		free(vanc_row);
}
int ClosedCaption::extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt)
{
	HRESULT result;
	IDeckLinkVideoFrameAncillary *ancillary = NULL;
	void *buffer = NULL;
	int idx = 0;
	int ret = -1;

	if(!arrivedFrame)
		return -1;
	if(!vanc_row)
	{
		num_bytes_row = arrivedFrame->GetRowBytes ();
		num_words_row = arrivedFrame->GetWidth ();
		vanc_row = (uint16_t*)malloc (sizeof (uint16_t) * num_words_row);
		memset (vanc_row, 0, sizeof (uint16_t) * num_words_row);
	}

	result = arrivedFrame->GetAncillaryData (&ancillary);
	if (result != S_OK)
	{ 
		std::cerr << "failed to get ancillary\n";
		ret  = -1;
		goto bail;
	}

	result = ancillary->GetBufferForVerticalBlankingLine( CCLINE, &buffer);
	if (result == E_INVALIDARG)
	{
		std::cerr << "Error: line number " << CCLINE <<  " invalid\n";
		ret  = -1;
		goto bail;
	}
	else if (result == E_FAIL)
	{
		std::cerr << "failed to retrieve line " << CCLINE << " data\n";
		ret  = -1;
		goto bail;
	}
	else if (result != S_OK)
	{
		std::cerr << "failed to retrieve line " << CCLINE << " data (unspecified error)\n";
		ret  = -1;
		goto bail;
	}
	else if (buffer == NULL)
	{
		std::cerr << "got NULL vertical blanking line buffer\n";
		ret = -1;
		goto bail;
	}
	/* 
	 * bit patter of 10bit yuv is as following
	 * xxUUUUUUUUUUYYYYYYYYYYVVVVVVVVVV
	 * xxYYYYYYYYYYUUUUUUUUUUYYYYYYYYYY
	 * xxVVVVVVVVVVYYYYYYYYYYUUUUUUUUUU
	 * xxYYYYYYYYYYVVVVVVVVVVYYYYYYYYYY
	 * Where we are only intrested in Y part
	 */
	for (int i = 0; i < num_bytes_row && pix_fmt == PIX_FMT_YUV422P10; i += 16)
	{
		// extract the luminence data
		vanc_row[idx++] = ((((unsigned char*)buffer)[i + 1] & 252) >> 2)
			+ ((((unsigned char*)buffer)[i + 2] & 15) << 6);

		vanc_row[idx++]  = ((((unsigned char*)buffer)[i + 4]))
			+ ((((unsigned char*)buffer)[i + 5] & 3) << 8);

		vanc_row[idx++] = ((((unsigned char*)buffer)[i + 6] & 240) >> 4)
			+ ((((unsigned char*)buffer)[i + 7] & 63) << 4);

		vanc_row[idx++] = ((((unsigned char*)buffer)[i + 9] & 252) >> 2)
			+ ((((unsigned char*)buffer)[i + 10] & 15) << 6);

		vanc_row[idx++] = ((((unsigned char*)buffer)[i + 12]))
			+ ((((unsigned char*)buffer)[i + 13] & 3) << 8);

		vanc_row[idx++] = ((((unsigned char*)buffer)[i + 14] & 240) >> 4)
			+ ((((unsigned char*)buffer)[i + 15] & 63) << 4);
	}
	for (int i = 0; i < num_words_row - 2; i++)
	{
		// find the VANC packet ADF (ancillary data flag): 0x000 0x3ff 0x3ff;
		// note that the DeckLink Mini Recorder is erroneously clamping the ADF values to
		// 0x004 0x3fb 0x3fb; the rest of the packet seems OK, so if we can match
		// this clamped ADF, we can parse the rest of the packet OK...
		if (!((vanc_row[i] == 0x00)
					&& (vanc_row[i + 1] == 0x3ff)
					&& (vanc_row[i + 2] == 0x3ff))
				&& !((vanc_row[i] == 0x04)
					&& (vanc_row[i + 1] == 0x3fb)
					&& (vanc_row[i + 2] == 0x3fb)))
		{
			continue;
		}

		ret = parse_vanc_packet (vanc_row + i + 3, num_words_row - i - 3, pkt);
		break;
	}

bail:
	ancillary->Release ();
	return ret;
}

int ClosedCaption::parse_vanc_packet (uint16_t *vanc_packet, long words_remaining, AVPacket &pkt)
{
	uint16_t did = vanc_packet[0] & 0x3ff;
	uint16_t sdid = vanc_packet[1] & 0x3ff;
	uint16_t data_count = vanc_packet[2] & 0xff;

#define CC_DID 0x161
#define EIA608_SID 0x101
	// validate the DID and the SDID (before stripping off parity bits)
	if (!(( did == CC_DID) && (sdid == EIA608_SID )))
	{
		return -1;
	}


	// subtract the DID, SDID, DC, and Checksum words
	//uint available_words = words_remaining - 4;
	if (data_count > words_remaining - 4)
		return -1;

	return parse_cdp (vanc_packet + 3, data_count, pkt);

	// @fixme:  validate the checksum
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
