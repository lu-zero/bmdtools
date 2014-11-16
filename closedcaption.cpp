#include <ClosedCaption.h>
#include <iostream>
#include <sys/time.h>

extern enum PixelFormat pix_fmt;
char cc_specialchar[] = {
    0xae, // 'reserved' symbol
    0xb0, // degree symbol
    0xbd, // 1/2
    0xbf, // upside-down question mark
    ' ',  // FIXME: trademark
    0xa2, // cents symbol
    0xa3, // british pound symbol
    ' ',  // FIXME: musical note
    0xe0, // a with reverse accent
    ' ',  // transparent space
    0xe8, // e with reverse accent
    0xe2, // a-hat
    0xea, // e-hat
    0xee, // i-hat
    0xf4, // o-hat
    0xfb  // u-hat
    };

ClosedCaption::ClosedCaption(void)
{
	build_char_table();
	num_bytes_row = 0;
	num_words_row = 0;
	vanc_row = NULL;
	opt_verbosity = 0;
	opt_cc_ntsc_field = 1;
	cc_line_buffer_len = 0;
	cc_line_buffer[0] = 0;
}
void ClosedCaption::build_char_table(void)
{
    int i;
    /* first the normal ASCII codes */
    for (i = 0; i < 128; i++)
    {
        cc_chartbl[i] = (char) i;
    }

    /* now the codes that deviate from ASCII */
    cc_chartbl[0x2a] = 0xe1;  // a with accent
    cc_chartbl[0x5c] = 0xe9;  // e with accent
    cc_chartbl[0x5e] = 0xed;  // i with accent
    cc_chartbl[0x5f] = 0xf3;  // o with accent
    cc_chartbl[0x60] = 0xfa;  // u with accent
    cc_chartbl[0x7b] = 0xe7;  // cedilla
    cc_chartbl[0x7c] = '/';   /* FIXME: this should be a division symbol */
    cc_chartbl[0x7d] = 0xd1;  // N with tilde
    cc_chartbl[0x7e] = 0xf1;  // n with tilde
    cc_chartbl[0x7f] = ' ';    /* FIXME: this should be a solid block */

}
AVPacket* ClosedCaption::extract(IDeckLinkVideoInputFrame* arrivedFrame)
{
	HRESULT result;
	IDeckLinkVideoFrameAncillary *ancillary = NULL;
	void *buffer = NULL;
	int idx = 0;

	if(!arrivedFrame)
		return NULL;
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
		goto bail;
	}

	result = ancillary->GetBufferForVerticalBlankingLine( CCLINE, &buffer);
	if (result == E_INVALIDARG)
	{
		std::cerr << "Error: line number " << CCLINE <<  " invalid\n";
		goto bail;
	}
	else if (result == E_FAIL)
	{
		std::cerr << "failed to retrieve line " << CCLINE << " data\n";
		goto bail;
	}
	else if (result != S_OK)
	{
		std::cerr << "failed to retrieve line " << CCLINE << " data (unspecified error)\n";
		goto bail;
	}
	else if (buffer == NULL)
	{
		std::cerr << "got NULL vertical blanking line buffer\n";
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
	if(pix_fmt != PIX_FMT_YUV422P10)
		std::cout<<"8 bit pixel format not yet supported";
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

		parse_vanc_packet (vanc_row + i + 3, num_words_row - i - 3);
		break;
	}

bail:
	ancillary->Release ();
	
}
void ClosedCaption::parse_vanc_packet (uint16_t *vanc_packet, long words_remaining)
{
	// validate the DID and the DID (before stripping off parity bits)
	if (!((vanc_packet[0] == 0x161) && (vanc_packet[1] == 0x101)))
	{
		return;
	}

	uint16_t did = vanc_packet[0] & 0xff;
	uint16_t sdid = vanc_packet[1] & 0xff;
	uint16_t data_count = vanc_packet[2] & 0xff;

	// subtract the DID, SDID, DC, and Checksum words
	uint available_words = words_remaining - 4;
	if (data_count > words_remaining - 4)
		return;

	parse_cdp (vanc_packet + 3, data_count);

	// @fixme:  validate the checksum
}

void ClosedCaption::parse_cdp (uint16_t *cdp, long num_words)
{
	if (!((cdp[0] == 0x296) && (cdp[1] == 0x269)))
	{
		std::cout << "[parse_cdp] could not find CDP identifier of 0x296 0x269\n";
		return;
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
		return;
	}

	uint16_t cc_count = cdp[8] & 0x1f;

	if (opt_verbosity > 2)
	{
		std::cout << "[parse_cdp] cc_count: " << std::dec << cc_count << "\n";
	}
	std::cout<<"cc_count"<< cc_count<<"\n";
	std::cout<<"num_words"<< num_words;
#if 0
	for (int i = 0; i < cc_count; i++)
	{
		int offset = 9 + i * 3;

		if (opt_verbosity > 2)
		{
			std::cout << "[parse_cdp] reading cc " << std::dec << i 
				<< ", offset = " << offset 
				<< ", words: 0x" << std::hex << cdp[offset] 
				<< " 0x" << std::hex << cdp[offset + 1] 
				<< " 0x" << std::hex << cdp[offset + 2] 
				<< "\n";
		}

		uint16_t marker = cdp[offset] & 0xf8;
		if (marker != 0xf8)
		{
			continue;
		}

		uint16_t cc_valid = (cdp[offset] & 0x04) >> 2;
		if (cc_valid != 1)
		{
			continue;
		}

		uint16_t cc_type = cdp[offset] & 0x03;

		// @todo: we're only reading EIA-608 data for now
		if ((cc_type == 0x2) || (cc_type == 0x3))
		{
			// data fields are CEA-708, not EIA-608, so ignore (we're only decoding
			// the EIA-608 captions for now;
			continue;
		}

		uint16_t cc_data_1 = cdp[offset + 1] & 0xff;
		uint16_t cc_data_2 = cdp[offset + 2] & 0xff;

		// @fixme: we're only reading NTSC line 21 field 1 CC; ignoring field 2
		if (opt_cc_ntsc_field == 1)
		{
			if (cc_type != 0x0)
			{
				continue;
			}
		}
		else
		{
			if (cc_type != 0x1)
			{
				continue;
			}
		}

		if (opt_verbosity > 2)
		{
			std::cout << "[parse_cdp] reading cc " << std::dec << i 
				<< ", offset = " << offset 
				<< ", words: 0x" << std::hex << cdp[offset] 
				<< " 0x" << std::hex << cdp[offset + 1] 
				<< " 0x" << std::hex << cdp[offset + 2] 
				<< "\n";
		}

		// note -- cc_data_1 and cc_data_2 are each 8 bits, but are often 
		// considered as one big 16-bit command (disregard the fact that we
		// store them each as 16-bit unsigned ints; they're really only
		// 8-bit values each).  So when you see me referring to bits > 7
		// (like "bit 12"), I mean "bit 4 of cc_data_1", since cc_data_1
		// would represent the 8 most significant bits of the aggregated
		// 16-bit command.

		// also note that bits 15 and 7 are always odd parity bits

		// SEE http://cdaw.gsfc.nasa.gov/meetings/2005_gmstorm/data/cdaw1/xine/src/xine-lib-1.0/src/libspucc/cc_decoder.c

		static char cmdstr[64];

		if (opt_verbosity > 1)
		{
			std::cout << "        <cc> ";
		}

		if ((cc_data_1 & 0x60) > 0)
		{
			// bits 14 or 13 on means that we display two characters
			char c1 = cc_chartbl[cc_data_1 & 0x7f];
			if (c1 > 0)
			{
				if (opt_verbosity > 1)
				{
					std::cout << c1 << " (0x" << std::hex << (int)c1 << ") ";
				}

				if (cc_line_buffer_len > CC_LINE_BUFFER_MAX - 2)
				{
					std::cerr << "Error -- cc buffer length exceeded (buffer: " << cc_line_buffer << ")\n";
				}
				else
				{
					cc_line_buffer[cc_line_buffer_len++] = c1;
					cc_line_buffer[cc_line_buffer_len] = 0;
				}

				put_raw_char (c1);
			}

			char c2 = cc_chartbl[cc_data_2 & 0x7f];
			if (c2 > 0)
			{
				if (opt_verbosity > 1)
				{
					std::cout << c2 << " (0x" << std::hex << (int)c2 << ") ";
				}

				if (cc_line_buffer_len > CC_LINE_BUFFER_MAX - 2)
				{
					std::cerr << "Error -- cc buffer length exceeded (buffer: " << cc_line_buffer << ")\n";
				}
				else
				{
					cc_line_buffer[cc_line_buffer_len++] = c2;
					cc_line_buffer[cc_line_buffer_len] = 0;
				}

				put_raw_char (c2);
			}


			continue;
		}

		if ((cc_data_1 == 0x11) && (cc_data_1 == 0x19))
		{
			// special character
			//
			// cc_data_2 should be a code point in the range 0x30-0x3f; its
			// four least significant bits are an offset into the special character
			// table
			char c1 = cc_specialchar[cc_data_2 & 0x0f];

			if (c1 > 0)
			{
				if (opt_verbosity > 1)
				{
					std::cout << c1 << " (0x" << std::hex << (int)c1 << ") ";
				}

				if (cc_line_buffer_len > CC_LINE_BUFFER_MAX - 2)
				{
					std::cerr << "Error -- cc buffer length exceeded (buffer: " << cc_line_buffer << ")\n";
				}
				else
				{
					cc_line_buffer[cc_line_buffer_len++] = c1;
					cc_line_buffer[cc_line_buffer_len] = 0;
				}

				put_raw_char (c1);
			}

			continue;
		}

		/*
		   if (((cc_data_1 & 0x1e) == 0x12) && ((cc_data_2 & 0x60) == 0x20))
		   {
		// extended character: bits 12, 9, 5 on, bits 11, 10, 6 off;
		// code point for character is bits 8 and 4-0
		// @fixme -- don't know how to translate these...
		}
		 */

		if (((cc_data_1 & 0x70) == 0x10) && ((cc_data_2 & 0x40) == 0x40))
		{
			// preamble address code

			// bits 10, 9, 8, 5: row position
			uint8_t row = ((cc_data_1 & 0x07) << 1) + ((cc_data_2 & 0x20) >> 5);

			// bits 4, 3, 2, 1: attributes
			uint8_t attr = ((cc_data_2 & 0x1e) >> 1);

			uint8_t underline = (cc_data_2 & 0x01);

			sprintf (cmdstr, "<<Row(%d)", row);
			switch (attr)
			{
				case 0:
					strcat (cmdstr, ",Color(white)");
					break;
				case 1:
					strcat (cmdstr, ",Color(green)");
					break;
				case 2:
					strcat (cmdstr, ",Color(blue)");
					break;
				case 3:
					strcat (cmdstr, ",Color(cyan)");
					break;
				case 4:
					strcat (cmdstr, ",Color(red)");
					break;
				case 5:
					strcat (cmdstr, ",Color(yellow)");
					break;
				case 6:
					strcat (cmdstr, ",Color(magenta)");
					break;
				case 7:
					strcat (cmdstr, ",Italics");
					break;
				case 8:
					strcat (cmdstr, ",Indent(0)");
					break;
				case 9:
					strcat (cmdstr, ",Indent(4)");
					break;
				case 10:
					strcat (cmdstr, ",Indent(8)");
					break;
				case 11:
					strcat (cmdstr, ",Indent(12)");
					break;
				case 12:
					strcat (cmdstr, ",Indent(16)");
					break;
				case 13:
					strcat (cmdstr, ",Indent(20)");
					break;
				case 14:
					strcat (cmdstr, ",Indent(24)");
					break;
				case 15:
					strcat (cmdstr, ",Indent(28)");
					break;
			}

			if (underline)
			{
				strcat (cmdstr, ",Underline");
			}
			strcat (cmdstr, ">>");

			put_raw_char ('\n');
			//finish_srt_cue ();

			if (opt_verbosity > 1)
			{
				std::cout << cmdstr;
			}

			continue;
		}

		if (((cc_data_1 & 0x77) == 0x11) && ((cc_data_2 & 0x70) == 0x20))
		{
			// mid-row code

			// bits 3, 2, 1: color
			uint8_t color = ((cc_data_2 & 0x0e) >> 1);
			uint8_t underline = (cc_data_2 & 0x01);

			static char cmdstr[64];
			sprintf (cmdstr, "<");

			switch (color)
			{
				case 0:
					strcat (cmdstr, "Color(white)");
					break;
				case 1:
					strcat (cmdstr, "Color(green)");
					break;
				case 2:
					strcat (cmdstr, "Color(blue)");
					break;
				case 3:
					strcat (cmdstr, "Color(cyan)");
					break;
				case 4:
					strcat (cmdstr, "Color(red)");
					break;
				case 5:
					strcat (cmdstr, "Color(yellow)");
					break;
				case 6:
					strcat (cmdstr, "Color(magenta)");
					break;
			}

			if (underline)
			{
				strcat (cmdstr, ",Underline");
			}
			strcat (cmdstr, ">");

			if (opt_verbosity > 1)
			{
				std::cout << cmdstr;
			}
			continue;
		}

		if (((cc_data_1 & 0x76) == 0x14) && ((cc_data_2 & 0x70) == 0x20))
		{
			// other control codes
			uint8_t cmd = cc_data_2 & 0x0f;

			int bufflen = 0;

			sprintf (cmdstr, "<<<");
			switch (cmd)
			{
				case 0:
					strcat (cmdstr, "resume caption loading");
					break;
				case 1:
					strcat (cmdstr, "backspace");
					if (cc_line_buffer_len > 0)
					{
						cc_line_buffer[--cc_line_buffer_len] = 0;
					}
					break;
				case 2:
					strcat (cmdstr, "reserved (formerly alarm off)");
					break;
				case 3:
					strcat (cmdstr, "reserved (formerly alarm on)");
					break;
				case 4:
					strcat (cmdstr, "delete to end of row");
					break;
				case 5:
					strcat (cmdstr, "roll-up captions 2-rows");
					//finish_srt_cue ();
					break;
				case 6:
					strcat (cmdstr, "roll-up captions 3-rows");
					//finish_srt_cue ();
					break;
				case 7:
					strcat (cmdstr, "roll-up captions 4-rows");
					//finish_srt_cue ();
					break;
				case 8:
					strcat (cmdstr, "flash on");
					break;
				case 9:
					strcat (cmdstr, "resume direct captioning");
					break;
				case 10:
					strcat (cmdstr, "text restart");
					break;
				case 11:
					strcat (cmdstr, "resume text display");
					//gettimeofday (&g_cc_cue_start, NULL);
					break;
				case 12:
					strcat (cmdstr, "erase displayed memory");
					break;
				case 13:
					strcat (cmdstr, "carriage return");
					put_raw_char ('\n');
					//finish_srt_cue ();
					break;
				case 14:
					strcat (cmdstr, "erase non-displayed memory");
					break;
				case 15:
					strcat (cmdstr, "end of caption (flip memories)");
					put_raw_char ('\n');
					//finish_srt_cue ();
					break;
			}
			strcat (cmdstr, ">>>");

			if (opt_verbosity > 1)
			{
				std::cout << cmdstr;
			}
		}
#if 0
		if (g_opt_cc_newline_frames > 0)
		{
			// if our captioning system depends on newlines for display,
			// (like if it's reading the raw text with fgets(), we can
			// artificially insert newlines into the text when we have
			// a few frames of "quiet" after a non-newline frame
			if (g_frames_since_non_newline == g_opt_cc_newline_frames)
			{
				put_raw_char ('\n');
			}
		}
#endif
	}
#endif
}
void ClosedCaption::put_raw_char (char c)
{
#if 0
    if (g_fp_cc_raw == NULL)
    {
        return;
    }

    if (c == '\n')
    {
        if (g_ignore_newlines)
        {
            return;
        }

        // we don't want any more newlines until we've gotten some
        // non-newline characters
        g_ignore_newlines = true;
    }
    else
    {
        // reset this as soon as we see a non newline
        g_ignore_newlines = false;

        g_frames_since_non_newline = 0;
    }
#endif
    //fprintf (g_fp_cc_raw, "%c", c);
    //fflush (g_fp_cc_raw);
}
