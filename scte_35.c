#include <SCTE_35.h>
#include <sys/time.h>
#include <iostream>

#define LINE 9
#define SCTE_35_DID 0x161
#define SCTE_35_SDID 0x101

using namespace std;
SCTE_35::SCTE_35(void)
{
	pkt_buff = NULL;
	pkt_buff_len = 0;
	vnc.set_param(LINE, SCTE_35_DID, SCTE_35_SDID);
	file.open ("scte.dat",ios::binary |ios::out);
}
SCTE_35::~SCTE_35(void)
{
	if (pkt_buff)
		free(pkt_buff);
  	file.close();
}
int SCTE_35::extract(IDeckLinkVideoInputFrame* arrivedFrame, AVPacket &pkt)
{
	uint16_t *data = NULL;
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
	for (int i = 0; i < ret;i++)
		pkt_buff[i] = data[i] & 0xff;

	file.write((char*)data, ret*2);
	pkt.size = ret;
	pkt.data = pkt_buff;
	return ret;
}
