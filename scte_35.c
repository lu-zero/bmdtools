#include <SCTE_35.h>
#include <sys/time.h>
#include <iostream>
#include <libavutil/intreadwrite.h>
#include <ctime>
#define LINE 14
#define SCTE_35_DID 0x41
#define SCTE_35_SDID 0x07

using namespace std;
SCTE_35::SCTE_35(void)
{
	pkt_buff = NULL;
	pkt_buff_len = 0;
	vnc.set_param(LINE, SCTE_35_DID, SCTE_35_SDID);
	file.open ("scte.dat",ios::binary |ios::out);
	log.open ("scte.log", ios::out);
	hexdump_trunc = fopen("scte_trunc.hex", "w");
}
SCTE_35::~SCTE_35(void)
{
	if (pkt_buff)
		free(pkt_buff);
  	file.close();
	fclose(hexdump);
	fclose(hexdump_trunc);
}
int SCTE_35::parse_scte104message(const uint8_t *buf)
{
	const uint8_t *buf_pivot = buf;
	int mSize = 0;
	int dataSize;
	time_t now = time(0);
	char* dt = ctime(&now);
	log<<dt<<endl;
	log<<"payload_desc "<<(int)*buf++<<endl;
	log<<"opID Ox"<<std::hex<<(int)AV_RB16(buf)<<endl;
	buf +=2;
	mSize = AV_RB16(buf);
	log<<"messageSize  "<<std::dec<<(int)mSize<<endl;
	buf += 2;
	log<<"result "<<(int)AV_RB16(buf)<<endl;
	buf += 2;
	log<<"result Extension "<<(int)AV_RB16(buf)<<endl;
	buf += 2;
	log<<"Protocol version "<<(int)AV_RB8(buf)<<endl;
	buf++;
	log<<"AS_index "<<(int)AV_RB8(buf)<<endl;
	buf++;
	log<<"message Number "<<(int)AV_RB8(buf)<<endl;
	buf++;
	log<<"DPI_PID_index "<<(int)AV_RB16(buf)<<endl;
	buf++;
	dataSize = mSize - (buf - buf_pivot);
	log<<"Data size"<<dataSize<<endl;
	buf += dataSize;
	return buf - buf_pivot;
}

int SCTE_35::extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt)
{
	uint16_t *data = NULL;
	int len;
	int ret;

	ret = vnc.extract(arrivedFrame, data);
	if(ret < 0 )
		return ret;

	if( pkt_buff_len < ret )
	{
		pkt_buff_len = ret;
		pkt_buff = (unsigned char*)realloc(pkt_buff, ret);
		if (pkt_buff == NULL)
			std::cerr << "Unable to allocate Memory\n";

	}
	//convert data in 8 bit format
	//for (int i = 0; i < ret;i++)
	//	pkt_buff[i] = data[i] & 0xff;
	len = ret;
	file.write((char*)data, ret*2);
        for(int i = 0; i < ret; i ++)
	{
                pkt_buff[i] = data[i] & 0xff;
        }
	av_hex_dump(hexdump_trunc,(uint8_t*) pkt_buff, ret);
	for(int i = 0; i < len; i ++)
	{
                ret = parse_scte104message(pkt_buff + i);
                i += ret;
		log<<endl;
        }

	pkt.size = 0;
	pkt.data = NULL;
	return ret;
}
