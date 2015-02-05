#include <SCTE_35.h>
#include <sys/time.h>
#include <iostream>
#include <libavutil/intreadwrite.h>
#include <ctime>
#define LINE 14
#define SCTE_35_DID 0x41
#define SCTE_35_SDID 0x07

using namespace std;
#ifdef TIME_ANALYSIS
void print_time(time_t tt) {
    char buf[80];
    struct tm* st = localtime(&tt);
    strftime(buf, 80, "%c", st);
    fprintf(stderr, "%s\n", buf);
}
#endif
SCTE_35::SCTE_35(void)
{
	pkt_buff = NULL;
	pkt_buff_len = 0;
	vnc.set_param(LINE, SCTE_35_DID, SCTE_35_SDID);
	output = new uint8_t[512]; 
}
SCTE_35::~SCTE_35(void)
{
	if (pkt_buff)
		free(pkt_buff);
	delete output;
}

int SCTE_35::parse_splice_request_data(const uint8_t *buf, int len)
{
        const uint8_t *buf_pivot = buf;

        uint8_t insert_type = *buf++;
        uint32_t event_id;
        uint16_t unique_program_id;
        uint16_t pre_roll_time;
        uint16_t break_duration;
	uint8_t avail_num;
        uint8_t avails_expected;
        uint8_t auto_return_flag;

        fprintf(stderr, "insert_type = %d\n",insert_type);

	if(len < 14)
		return -1;
        event_id = AV_RB32(buf);
        buf += 4;
        unique_program_id = AV_RB16(buf);
        buf += 2;
        pre_roll_time = AV_RB16(buf);
        buf += 2;
        break_duration = AV_RB16(buf);
        buf += 2;
        avail_num = AV_RB8(buf);
        buf++;
        avails_expected = AV_RB8(buf);
        buf++;
        auto_return_flag = AV_RB8(buf);
        buf++;

        fprintf(stderr, "break_duration = %d\n",break_duration);
	set_event_param(insert_type, event_id, unique_program_id,
		pre_roll_time, break_duration, avail_num,
		avails_expected, auto_return_flag, this->current_pts);

        return buf - buf_pivot;
}

int SCTE_35::parse_timestamp(const uint8_t *buf, int len)
{
        const uint8_t *buf_pivot = buf;
        int type = *buf++;
        if (type == 0) {
		/* use pkt pts */
        } else if(type == 1) {

                fprintf(stderr, "UTC_seconds %x = %d\n",AV_RB32(buf), AV_RB32(buf));
                buf +=4;
                fprintf(stderr, "UTC_microseconds %hx = %hd\n",AV_RB16(buf), AV_RB16(buf));
                buf +=2;
        } else if (type == 2) {
                fprintf(stderr, "hours  %hhd\n",*buf++);
                fprintf(stderr, "minute %hhd\n",*buf++);
                fprintf(stderr, "second %hhd\n",*buf++);
                fprintf(stderr, "frames %hhd\n",*buf++);
        } else if (type == 3) {
                fprintf(stderr, "second %hhd\n",*buf++);
                fprintf(stderr, "frames %hhd\n",*buf++);
        } else {
                fprintf(stderr, "check CRC packet corrupted or updated version\n");
        }

        return buf - buf_pivot;

}

int SCTE_35::parse_multi_operation_message(const uint8_t *buf, int len)
{
        const uint8_t *buf_pivot = buf;
        int mSize = 0;
        int nb_op;
        int i,data_length,opId;
	uint8_t protocol_version;
	int ret = 0;

        /* reserved */
        buf += 2;

        mSize = AV_RB16(buf);
        buf += 2;

        fprintf(stderr, "messageSize 0x%hx %hd  %dbytes\n",mSize,mSize,len);
        if (len < mSize) {
                return -1;
        }

        //fprintf(log, "Protocol version %hhx = %hhd\n",AV_RB8(buf), AV_RB8(buf));
        buf++;
        //printf("AS_index %hhx = %hhd\n",AV_RB8(buf), AV_RB8(buf));
        buf++;
        //fprintf(log, "message Number %hhx = %hhu\n",AV_RB8(buf), AV_RB8(buf));
        buf++;

        //printf("DPI_PID_index %hx = %hd\n",AV_RB16(buf), AV_RB16(buf));
        buf += 2;

	protocol_version = AV_RB8(buf);
	set_scte35_protocol_version(protocol_version);
        buf++;

        buf += parse_timestamp(buf, buf - buf_pivot);

        nb_op = *buf++;
        for (i = 0; i < nb_op; i++ ) {
                opId= AV_RB16(buf);
                buf +=2;
                data_length= AV_RB16(buf);
                buf +=2;
                switch(opId){
                case 0x0101:
                        ret = parse_splice_request_data(buf, data_length);
			command = 0x05;
			command_set_flag  = 1;
                        break;
                default:
                        break;
                }
		if (ret < 0) {
			fprintf(stderr, "Invalid length of buf for opID%d", opId);
			break;
		}
                buf += data_length;
        }

	if (ret < 0)
		return -1;
	else
		return buf - buf_pivot;
}

int SCTE_35::parse_single_operation_message(const uint8_t *buf, int len)
{
        int16_t opId;
        const uint8_t *buf_pivot = buf;
        int mSize = 0;
        int dataSize;
#ifdef TIME_ANALYSIS
	time_t t = time(NULL);
	print_time(t);
#endif
        opId = AV_RB16(buf);
        buf += 2;

	switch (opId) {
	case 0x03:
		/* set NULL scte-35 cmd */
		command = 0x00;
		command_set_flag  = 1;
		break;
	default:
		break;
	}
        mSize = AV_RB16(buf);
        buf += 2;
        if (len < mSize) {
                return -1;
        }
        //printf("result %hx = %hd\n",AV_RB16(buf), AV_RB16(buf));
        buf += 2;
        //printf("result Extension %hx = %hd\n",AV_RB16(buf), AV_RB16(buf));
        buf += 2;
        //printf("Protocol version %hhx = %hhd\n",AV_RB8(buf), AV_RB8(buf));
        buf++;
        //printf("AS_index %hhx = %hhd\n",AV_RB8(buf), AV_RB8(buf));
        buf++;
        //fprintf(log, "message Number %hhx = %hhu\n",AV_RB8(buf), AV_RB8(buf));
        buf++;
        //printf("DPI_PID_index %hx = %hd\n",AV_RB16(buf), AV_RB16(buf));
        buf += 2;
        dataSize = mSize - (buf - buf_pivot);
        //printf("Data size %d bytes\n", dataSize);
        buf += dataSize;

        return buf - buf_pivot;

}

int SCTE_35::parse_scte104message(const uint8_t *buf, int len)
{
        const uint8_t *buf_pivot = buf;
        int ret;
        if (len < 5) {
                return -1;
        }
        //fprintf(log, "payload_desc %hhd\n",*buf++);
	buf++;
        if(AV_RB16(buf) == 0xffff)
                ret = parse_multi_operation_message(buf, len - 1);
        else
                ret = parse_single_operation_message(buf, len - 1);

        return ret + (buf -  buf_pivot);

}

int SCTE_35::extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt)
{
	uint16_t *data = NULL;
	int len;
	int ret;

	ret = vnc.extract(arrivedFrame, data);
	if(ret < 0 ) {
		ret = encode(output, pkt.size, 0x00);
		pkt.data = output;
		return ret;
        }

	if( pkt_buff_len < ret )
	{
		pkt_buff = (unsigned char*)realloc(pkt_buff, ret);
		if (pkt_buff == NULL)
			std::cerr << "Unable to allocate Memory\n";
		else
			pkt_buff_len = ret;

	}
        for(int i = 0; i < ret; i ++)
	{
                pkt_buff[i] = data[i] & 0xff;
        }
	this->current_pts = pkt.pts;
	len = ret;
	for(int i = 0; i < len; i ++)
	{
                ret = parse_scte104message(pkt_buff + i, len - i);
                i += ret;
		if (command_set_flag)
			ret = encode(output, pkt.size, command);
		else
			ret =  0;
		command_set_flag = 0;
        }

	pkt.data = output;
	return ret;
}

#ifdef TEST_SCTE_PARSER
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char**argv)
{
	int ret,len,i = 0;
	int infd = 0;
	int stripfd = 0;
	uint8_t buf[1024];
	uint8_t sbuf[1024];
	uint8_t encoded_buf[1024];
	int index = 0;
	int index_len = 0;
	int temclass SCTE_35 parser;
	class SCTE_35 parser;
	if (argc < 2) {
		fprintf(stderr, "%s <filename>\n", argv[0]);
		return -1;
	}
        infd = open(argv[1], O_RDONLY);
        stripfd = open("scte_skipped.dat", O_RDWR|O_CREAT, 0644);
        if(infd < 0) {
		perror("open scte raw data");
		return -1;
        }

        while ( (len = read(infd, buf, 1024))) {
                if(len < 0) {
                        perror("open scte.dat");
                        return -1;
                }
                len = len/2 + index_len;
                for(i = 0; i < len; i ++) {
                        if(i < index_len)
                                sbuf[i] = sbuf[i+index];
                        else
                                sbuf[i] = buf[(i-index_len)*2];
                }
                index_len = 0;
                index = 0;
                stripfd = write(stripfd, sbuf,len);
                for(i = 0, ret = 0; i < len;) {
                        ret = parser.parse_scte104message(sbuf + i , len-i);
                        if (ret > 0)
                                i += ret;
                        else {
                                index = i;
                                index_len = len - i;
                                break;
                        }
                }
        }


	return 0;
}
#endif
