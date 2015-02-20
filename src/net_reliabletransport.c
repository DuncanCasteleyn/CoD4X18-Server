#include "q_shared.h"
#include "qcommon.h"

#define MAX_PACKETLEN           1400        // max size of a network packet
#define MAX_FRAGMENT_SIZE         ( MAX_PACKETLEN - 200 )
#define DEFAULT_BUFFER_SIZE		32

#include "sys_net.h"
#include "msg.h"
#include "net_reliabletransport.h"
#include <string.h>
#include <stdlib.h>



int ReliableMessageGetUsedSendBufferSize(netreliablemsg_t *chan)
{	
	int usedfragmentcnt;

	if(chan == NULL)
	{
		return 0;
	}
	usedfragmentcnt = chan->txwindow.sequence - chan->txwindow.acknowledge;
	return usedfragmentcnt;
}



int ReliableMessageChangeSendBufferSize(netreliablemsg_t *chan, int newfragmentcount)
{	
	fragment_t *newfrags, *oldfrags;
	int i, sindex, dindex;

	if(chan == NULL)
	{
		return -1;
	}
	int inusefragments = ReliableMessageGetUsedSendBufferSize(chan);

	if(newfragmentcount <= DEFAULT_BUFFER_SIZE)
	{
		newfragmentcount = DEFAULT_BUFFER_SIZE;
		if(chan->txwindow.bufferlen == DEFAULT_BUFFER_SIZE)
		{
			return DEFAULT_BUFFER_SIZE;
		}
	}

	if(inusefragments >= newfragmentcount)
	{
		return -1;
	}
	newfrags = malloc(newfragmentcount * sizeof(fragment_t));
	if(newfrags == NULL)
	{
		return -1;
	}
	
	oldfrags = chan->txwindow.fragments;

	if(chan->txwindow.acknowledge == 0)
	{
		for(i = 0; i < newfragmentcount; ++i)
		{
			newfrags[i].ack = -1;
		}
	}
	
	for(i = chan->txwindow.acknowledge; i < chan->txwindow.sequence; ++i)
	{	
		sindex = i % chan->txwindow.bufferlen;
		dindex = i % newfragmentcount;
		
		if(oldfrags[sindex].len > MAX_FRAGMENT_SIZE)
		{
			oldfrags[sindex].len = MAX_FRAGMENT_SIZE;
		}else if(oldfrags[sindex].len < 0){
			oldfrags[sindex].len = 1;
		}	
		memcpy(newfrags[dindex].data, oldfrags[sindex].data, oldfrags[sindex].len);
		newfrags[dindex].len = oldfrags[sindex].len;
		newfrags[dindex].ack = oldfrags[sindex].ack;
		newfrags[dindex].packetnum = oldfrags[sindex].packetnum;
		newfrags[dindex].senttime = oldfrags[sindex].senttime;
	}
	
	free(oldfrags);
	chan->txwindow.fragments = newfrags;	
	
	chan->txwindow.bufferlen = newfragmentcount;
	Com_Printf("^2New Fragmentcount: %d\n", chan->txwindow.bufferlen);
	return chan->txwindow.bufferlen;
}


int ReliableMessagesGetAcknowledge(framedata_t *frame)
{
	int i;
	
	for(i = 0; i < frame->windowsize; ++i){
		if(frame->fragments[(i + frame->sequence) % frame->bufferlen].ack != i + frame->sequence)
		{
			break;
		}
	}
	return i + frame->sequence;
}


void ReliableMessageWriteSelectiveAcklist(framedata_t *frame, msg_t* msg)
{
	int i;
	int inrange = 0;
	int count = 0;
	int lengthcnt;
	int numbytepos;
	//Function can write up to 9 bytes (count >= 2)
	numbytepos = msg->cursize;
	//0 elements
	MSG_WriteByte(msg, 0);
	
	for(i = frame->selackoffset; i < frame->windowsize; ++i){
		if(frame->fragments[(i + frame->sequence) % frame->bufferlen].ack == i + frame->sequence)
		{
			if(inrange == 0)
			{
				MSG_WriteShort(msg, i);
				count++;
				lengthcnt = 0;
			}
			inrange = 1;
			lengthcnt++;
		}else{
			if(inrange == 1)
			{
				MSG_WriteShort(msg, lengthcnt);
				if(count >= 3)
				{
					break;
				}
			}
			inrange = 0;
		}
	}
	if(inrange == 1)
	{
		MSG_WriteShort(msg, lengthcnt);
	}
	if(i < frame->windowsize){
		frame->selackoffset = i;
	}else{
		frame->selackoffset = 1;
	}
	msg->data[numbytepos] = count;
}

//This function sends one new sequence
void ReliableMessagesTransmitNextFragment(netreliablemsg_t *chan)
{
	int sequence;
	msg_t buf;
	byte data[MAX_PACKETLEN];

	if(chan == NULL)
	{
		return;
	}
	
	MSG_Init(&buf, data, sizeof(data));

	if(chan->txwindow.acknowledge == chan->txwindow.sequence)
	{
		//Let the remote end still know about the current acknowledge state even when nothing is going to be sent 
		if(chan->nextacktime < chan->time)
		{
			MSG_WriteLong(&buf, 0xfffffff0);
			//Writing -1 as sequence means this is only an ACK packet
			MSG_WriteShort(&buf, chan->qport);
			MSG_WriteLong(&buf, -1);
			MSG_WriteLong(&buf, chan->rxwindow.sequence);
			ReliableMessageWriteSelectiveAcklist(&chan->rxwindow, &buf);
			MSG_WriteShort(&buf, chan->txwindow.windowsize);
			MSG_WriteShort(&buf, 0);
			NET_SendPacket( chan->sock, buf.cursize, buf.data, &chan->remoteAddress );	
			chan->txwindow.packets++;
			chan->nextacktime = chan->time + 350;
		}
		return;
	}
	if(chan->txwindow.frame < chan->txwindow.acknowledge)
	{
		chan->txwindow.frame = chan->txwindow.acknowledge;
	}
	
	sequence = chan->txwindow.frame;

	if(chan->txwindow.frame < chan->txwindow.sequence)
	{
	    if(chan->txwindow.fragments[sequence % chan->txwindow.bufferlen].ack == sequence)
	    {
		//Already received by the remote end
		Com_Printf("Send: Skip over %d\n", sequence);
	    }else{
		MSG_WriteLong(&buf, 0xfffffff0);
		MSG_WriteShort(&buf, chan->qport);
		MSG_WriteLong(&buf, sequence);
		MSG_WriteLong(&buf, chan->rxwindow.sequence); //Acknowledge for the other end
		ReliableMessageWriteSelectiveAcklist(&chan->rxwindow, &buf);
		MSG_WriteShort(&buf, chan->txwindow.windowsize);
		MSG_WriteShort(&buf, chan->txwindow.fragments[sequence % chan->txwindow.bufferlen].len); //Fragment size
		MSG_WriteData(&buf, chan->txwindow.fragments[sequence % chan->txwindow.bufferlen].data, 
							chan->txwindow.fragments[sequence % chan->txwindow.bufferlen].len);
							
		NET_SendPacket( chan->sock, buf.cursize, buf.data, &chan->remoteAddress );
		chan->txwindow.packets++;
		chan->nextacktime = chan->time + 350;
		Com_Printf("Sending SEQ: %d ACK: %d\n", sequence, chan->rxwindow.sequence);
	    }
	}

	++chan->txwindow.frame;
	if(chan->txwindow.frame >= chan->txwindow.acknowledge + chan->txwindow.windowsize)
	{
		chan->txwindow.frame = chan->txwindow.acknowledge;
	}
	
}

//Assuming you have already read the port
void ReliableMessagesReceiveNextFragment(netreliablemsg_t *chan, msg_t* buf)
{	
	int sequence, acknowledge;
	unsigned int numselectiveack, windowsize, fragmentsize, length, startack;
	int i, j;
	int usedfragmentcnt;

	if(chan == NULL)
	{
		return;
	}
	
	sequence = MSG_ReadLong(buf);
	acknowledge = MSG_ReadLong(buf);

	chan->rxwindow.packets++;
	
	//if fragment out of window size?
	if(sequence >= chan->rxwindow.sequence + chan->rxwindow.windowsize)
	{
		return;
	}
	if(acknowledge > chan->txwindow.acknowledge + chan->txwindow.windowsize)
	{
		Com_PrintError("Illegible reliable acknowledge - got: %d current: %d\n", acknowledge, chan->txwindow.acknowledge);
		return;
	}
	if(acknowledge < chan->txwindow.acknowledge)
	{
		Com_PrintError("Too old reliable acknowledge %d > %d\n", chan->txwindow.acknowledge, acknowledge);
		return;
	}
	if(acknowledge > chan->txwindow.sequence){
		Com_PrintError("Invalid reliable acknowledge. acknowledge(%d) > sequence(%d)\n", acknowledge, chan->txwindow.sequence);
		return;
	}
	
	numselectiveack = MSG_ReadByte(buf);
	if(numselectiveack > 3 )
	{
		Com_PrintError("Bad selective acknowledge count: %d\n", numselectiveack);	
		return;
	}
	for(i = 0; i < numselectiveack; ++i)
	{
		startack = MSG_ReadShort(buf) + acknowledge;
		length = MSG_ReadShort(buf);
		if(startack + length > acknowledge + chan->txwindow.windowsize){
			Com_PrintError("Selective acknowledge %d is out of windowsize acknowledge %d\n", startack + length, acknowledge);
			return;
		}
		Com_Printf("SACK: %d %d\n", startack, length);
		for(j = 0; j < length; ++j)
		{
			chan->txwindow.fragments[(startack +j) % chan->txwindow.bufferlen].ack = startack +j;
		}
	}

	windowsize = MSG_ReadShort(buf);
	fragmentsize = MSG_ReadShort(buf);
	
	Com_Printf("^5Received ACK %d SEQ: %d\n", acknowledge, sequence);
	
	if(fragmentsize > MAX_FRAGMENT_SIZE){
		Com_PrintError("Invalid fragmentsize (%d)\n", fragmentsize);
		return;
	}
	
	if(chan->txwindow.acknowledge < acknowledge){
		//Acknowledge all received data
		chan->txwindow.acknowledge = acknowledge;
		Com_Printf("^5Acknowledge is now %d Top is now: %d Remaining fragments are %d\n", chan->txwindow.acknowledge, chan->txwindow.sequence, chan->txwindow.sequence - chan->txwindow.acknowledge);
		/* Purpos: reducing the dynamic send buffer size when it is used only partially */
		usedfragmentcnt = chan->txwindow.sequence - chan->txwindow.acknowledge;

		if(usedfragmentcnt < DEFAULT_BUFFER_SIZE && chan->txwindow.bufferlen > DEFAULT_BUFFER_SIZE)
		{
			ReliableMessageChangeSendBufferSize(chan, (DEFAULT_BUFFER_SIZE));
		}
	}

	if(sequence == -1){
		return;
	}
		//if old fragment?
	if(sequence < chan->rxwindow.sequence)
	{
		return;
	}
	
	chan->rxwindow.fragments[sequence % chan->rxwindow.bufferlen].len = fragmentsize;
	MSG_ReadData(buf, chan->rxwindow.fragments[sequence % chan->rxwindow.bufferlen].data, 
					chan->rxwindow.fragments[sequence % chan->rxwindow.bufferlen].len);
	chan->rxwindow.fragments[sequence % chan->rxwindow.bufferlen].ack = sequence;


}


int ReliableMessageReceive(netreliablemsg_t *chan, byte* outdata, int len)
{	
	int hisequence, losequence, wlen;
	int i, index, writepos, availablefragments;
	
	if(chan == NULL)
	{
		return 0;
	}
	
	hisequence = ReliableMessagesGetAcknowledge(&chan->rxwindow);
	losequence = chan->rxwindow.sequence;
	availablefragments = hisequence - losequence;

	writepos = 0;

	//Do we have cached fragments? So send them first
	if(len > chan->rxwindow.fragmentbuffer.cursize - chan->rxwindow.fragmentbuffer.readcount)
	{
		wlen = chan->rxwindow.fragmentbuffer.cursize - chan->rxwindow.fragmentbuffer.readcount;
	}else{
		wlen = len;
	}
	memcpy(outdata, chan->rxwindow.fragmentbuffer.data + chan->rxwindow.fragmentbuffer.readcount, wlen);
	chan->rxwindow.fragmentbuffer.readcount += wlen;
	writepos += wlen;

	for(i = 0; i < availablefragments && writepos < len; ++i)
	{
		index = (chan->rxwindow.sequence + i) % chan->rxwindow.bufferlen;

		if(len - writepos > chan->rxwindow.fragments[index].len)
		{
			wlen = chan->rxwindow.fragments[index].len;
		}else{
			wlen = len - writepos;
			//Store off what we can not put into the buffer
			MSG_Clear(&chan->rxwindow.fragmentbuffer);
			chan->rxwindow.fragmentbuffer.cursize = chan->rxwindow.fragments[index].len - wlen;
			memcpy(chan->rxwindow.fragmentbuffer.data, chan->rxwindow.fragments[index].data + wlen, chan->rxwindow.fragmentbuffer.cursize);

		}
		memcpy(outdata + writepos, chan->rxwindow.fragments[index].data, wlen);
		writepos += wlen;
	}

	chan->rxwindow.sequence += i;
	if(i > 1)
	{
		chan->rxwindow.selackoffset = 1;
	}
	return writepos;
}


int ReliableMessageReceiveSingleFragment(netreliablemsg_t *chan, byte* outdata, int len)
{
	int hisequence, losequence;
	int numfragments;
	int index;
	
	if(chan == NULL)
	{
		return 0;
	}

	if(len < MAX_FRAGMENT_SIZE)
	{
		return 0;
	}

	hisequence = ReliableMessagesGetAcknowledge(&chan->rxwindow);
	losequence = chan->rxwindow.sequence;
	numfragments = hisequence - losequence;
	
	if(numfragments < 1)
	{
		return 0;
	}

	index = (chan->rxwindow.sequence) % chan->rxwindow.bufferlen;

	memcpy(outdata, chan->rxwindow.fragments[index].data, chan->rxwindow.fragments[index].len);

	chan->rxwindow.sequence++;

	return chan->rxwindow.fragments[index].len;
}




int ReliableMessageSend(netreliablemsg_t *chan, byte* indata, int len)
{	
	int usedfragmentcnt, freefragmentcnt, newbuflen, newfreefragmentcnt;
	int sentlen;
	int i, index, slen;
	
	if(chan == NULL)
	{
		return 0;
	}

	usedfragmentcnt = chan->txwindow.sequence - chan->txwindow.acknowledge;
	freefragmentcnt = chan->txwindow.bufferlen - usedfragmentcnt;
	
	if(len < 0){
		Com_Error(ERR_FATAL, "ReliableMessageSend: Invalid length: %d", len);
	}
	sentlen = 0;

	if(freefragmentcnt < (len / MAX_FRAGMENT_SIZE))
	{
		newbuflen = len / MAX_FRAGMENT_SIZE + usedfragmentcnt;
		/* 1.5 * needed size */
		newbuflen += (newbuflen / 2);
		newfreefragmentcnt = ReliableMessageChangeSendBufferSize(chan, newbuflen) - usedfragmentcnt;
		if(newfreefragmentcnt > 0)
		{
			freefragmentcnt = newfreefragmentcnt;
		}
	}

	for(i = 0; i < freefragmentcnt; ++i)
	{
		if(len >= MAX_FRAGMENT_SIZE)
		{
			slen = MAX_FRAGMENT_SIZE;
		}else if(len > 0){
			slen = len;
		}else{
			return sentlen;
		}
		
		index = chan->txwindow.sequence % chan->txwindow.bufferlen;
		memcpy(chan->txwindow.fragments[index].data, indata + i * MAX_FRAGMENT_SIZE, slen);
		chan->txwindow.fragments[index].len = slen;
		
		len -= slen;
		sentlen += slen;
		
		chan->txwindow.sequence++;
	}		
	return sentlen;
}




int ReliableMessageGetAvailableSendBufferSize(netreliablemsg_t *chan)
{
	int usedfragmentcnt, freefragmentcnt;

	if(chan == NULL)
	{
		return 0;
	}

	usedfragmentcnt = chan->txwindow.sequence - chan->txwindow.acknowledge;
	freefragmentcnt = chan->txwindow.bufferlen - usedfragmentcnt;
	return freefragmentcnt;
}

netreliablemsg_t* ReliableMessageSetup( int qport, int netsrc, netadr_t* remote)
{
	fragment_t* dynrxmem;
	fragment_t* dyntxmem;
	netreliablemsg_t *chan;

	chan = malloc(sizeof(netreliablemsg_t));
	if(chan == NULL)
	{
		Com_PrintError("ReliableMessageSetup(): Out of memory\n");
		return NULL;
	}
	memset(chan, 0, sizeof(netreliablemsg_t));

	dynrxmem = malloc(sizeof(fragment_t) * DEFAULT_BUFFER_SIZE);
	if(dynrxmem == NULL)
	{
		free(chan);
		return NULL;
	}
	dyntxmem = malloc(sizeof(fragment_t) * DEFAULT_BUFFER_SIZE);
	if(dyntxmem == NULL)
	{
		free(chan);
		free(dynrxmem);
		return NULL;
	}

	chan->txwindow.fragments = dyntxmem;
	chan->rxwindow.fragments = dynrxmem;
	chan->txwindow.windowsize = 16;
	chan->rxwindow.windowsize = 16;
	chan->txwindow.bufferlen = DEFAULT_BUFFER_SIZE;
	chan->rxwindow.bufferlen = DEFAULT_BUFFER_SIZE;	
	
	memset(chan->txwindow.fragments, -1, chan->txwindow.bufferlen * sizeof(fragment_t));
	memset(chan->rxwindow.fragments, -1, chan->rxwindow.bufferlen * sizeof(fragment_t));
	memcpy(&chan->remoteAddress, remote, sizeof(netadr_t));

	MSG_Init(&chan->rxwindow.fragmentbuffer, chan->rxwindow.fragmentdata, sizeof(chan->rxwindow.fragmentdata));
	MSG_Init(&chan->txwindow.fragmentbuffer, chan->txwindow.fragmentdata, sizeof(chan->txwindow.fragmentdata));

	chan->sock = netsrc;
	chan->qport = qport;
	return chan;
}

void ReliableMessageDisconnect(netreliablemsg_t *chan)
{	
	if(chan == NULL)
	{
		return;
	}
	if(chan->txwindow.fragments)
	{
		free(chan->txwindow.fragments);
	}
	if(chan->rxwindow.fragments)
	{
		free(chan->rxwindow.fragments);
	}
	free(chan);
}

void ReliableMessageSetCurrentTime(netreliablemsg_t *chan, int ftime)
{
	if(chan == NULL)
	{
		return;
	}
	chan->time = ftime;
}

/* Functions to test the operation. No real use. */
static int testdata[1024*1024*16];
static int sendpos;

void ReliableMessageAddTestData(netreliablemsg_t *chan, int numint)
{
    int i;
    int numbytes;
    numbytes = 4 * numint;
    int sentbytes;

    if(chan == NULL)
    {
        return;
    }


    if(testdata[0] == 0)
    {
	for(i = 0; i < (1024*1024*4); ++i)
	{
		testdata[i] = i +1;
	}

    }

    if(numbytes > sizeof(testdata) - sendpos)
    {
        numbytes = sizeof(testdata) - sendpos;
    }

    sentbytes = ReliableMessageSend(chan, ((byte*)testdata) + sendpos, numbytes);
    sendpos += sentbytes;
    Com_Printf("Sending %d bytes...\n", sentbytes);
}


void ReliableMessageGetTestData(netreliablemsg_t *chan)
{
	int recvdata[8192];
	int i;
	static int verify = 1;

	if(chan == NULL)
	{
		return;
	}

	int numbytes = ReliableMessageReceive(chan, (byte*)recvdata, sizeof(recvdata));

	if(numbytes > 0)
		Com_Printf("Received %d bytes\n", numbytes);

	for(i = 0; i < (numbytes/4); ++i, ++verify){
		if(recvdata[i] != verify)
		{
			Com_Printf("Verify error! Expected: %d Got: %d\n", verify, recvdata[i]);	
		}
	}
	
}
