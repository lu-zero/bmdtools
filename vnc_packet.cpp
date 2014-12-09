#include "VncPacket.h"
#include <iostream>
#include <cstring>
VncPacket::VncPacket(void)
{
	vanc_row = NULL;
	num_bytes_row = 0;
	num_words_row = 0;
}
VncPacket::~VncPacket(void)
{
	if(vanc_row) {
		delete vanc_row;
		vanc_row = NULL;
	}
}
int VncPacket::set_param(int vline, int16_t did, int16_t sdid)
{
	if (vline < 1 || vline > 20)
		return -1;
	vertical_line = vline;
	this->did = did;
	this->sdid = sdid;
	return 0;
}
int VncPacket::extract(IDeckLinkVideoInputFrame* arrivedFrame, uint16_t* &data)
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
		vanc_row = new uint16_t[num_words_row]();
		/* May bw useless and redundant to memset */
		memset (vanc_row, 0, sizeof (uint16_t) * num_words_row);
	}

	result = arrivedFrame->GetAncillaryData (&ancillary);
	if (result != S_OK)
	{ 
		std::cerr << "failed to get ancillary\n";
		ret  = -1;
		goto bail;
	}

	result = ancillary->GetBufferForVerticalBlankingLine( vertical_line, &buffer);
	if (result == E_INVALIDARG)
	{
		std::cerr << "Error: line number " << vertical_line <<  " invalid\n";
		ret  = -1;
		goto bail;
	}
	else if (result == E_FAIL)
	{
		std::cerr << "failed to retrieve line " << vertical_line << " data\n";
		ret  = -1;
		goto bail;
	}
	else if (result != S_OK)
	{
		std::cerr << "failed to retrieve line " << vertical_line << " data (unspecified error)\n";
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
	for (int i = 0; i < num_bytes_row; i += 16)
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

		ret = parse_vanc_packet (vanc_row + i + 3, num_words_row - i - 3, data);
		break;
	}

bail:
	ancillary->Release ();
	return ret;
}

int VncPacket::parse_vanc_packet (uint16_t *vanc_packet, long words_remaining, uint16_t* &data)
{
	uint16_t did = vanc_packet[0] & 0x3ff;
	uint16_t sdid = vanc_packet[1] & 0x3ff;
	uint16_t data_count = vanc_packet[2] & 0xff;

	// validate the DID and the SDID (before stripping off parity bits)
	if (!(( did == this->did) && (sdid == this->sdid )))
	{
		return -1;
	}


	// subtract the DID, SDID, DC, and Checksum words
	//uint available_words = words_remaining - 4;
	if (data_count > words_remaining - 4)
		return -1;

	data = vanc_packet + 3;
	return data_count;

	// @fixme:  validate the checksum
}

