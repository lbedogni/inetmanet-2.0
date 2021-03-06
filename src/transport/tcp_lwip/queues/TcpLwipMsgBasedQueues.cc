//
// Copyright (C) 2004 Andras Varga
//               2010 Zoltan Bojthe
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//


#include "TcpLwipMsgBasedQueues.h"

#include "TCPCommand_m.h"
#include "TcpLwipConnection.h"
#include "TCPSerializer.h"
#include "TCP_lwIP.h"


Register_Class(TcpLwipMsgBasedSendQueue);

Register_Class(TcpLwipMsgBasedReceiveQueue);


TcpLwipMsgBasedSendQueue::TcpLwipMsgBasedSendQueue()
    :
    beginM(0),
    endM(0),
    unsentTcpLayerBytesM(0)
{
}

TcpLwipMsgBasedSendQueue::~TcpLwipMsgBasedSendQueue()
{
    while (! payloadQueueM.empty())
    {
        EV << "SendQueue Destructor: Drop msg from " << connM->tcpLwipM.getFullPath()
                << " Queue: seqno=" << payloadQueueM.front().endSequenceNo
                << ", length=" << payloadQueueM.front().msg->getByteLength() << endl;
        delete payloadQueueM.front().msg;
        payloadQueueM.pop_front();
    }
}

void TcpLwipMsgBasedSendQueue::setConnection(TcpLwipConnection *connP)
{
    TcpLwipSendQueue::setConnection(connP);
    endM = beginM = 0;
    isValidSeqNoM = false;
    unsentTcpLayerBytesM = 0;
}

void TcpLwipMsgBasedSendQueue::enqueueAppData(cPacket *msgP)
{
    ASSERT(msgP);

    uint32 bytes = msgP->getByteLength();
    endM += bytes;
    unsentTcpLayerBytesM += bytes;

    Payload payload;
    payload.endSequenceNo = endM;
    payload.msg = msgP;
    payloadQueueM.push_back(payload);
}

unsigned int TcpLwipMsgBasedSendQueue::getBytesForTcpLayer(
        void* bufferP, unsigned int bufferLengthP) const
{
    ASSERT(bufferP);

    return (unsentTcpLayerBytesM > bufferLengthP) ? bufferLengthP : unsentTcpLayerBytesM;
}

void TcpLwipMsgBasedSendQueue::dequeueTcpLayerMsg(unsigned int msgLengthP)
{
    ASSERT(msgLengthP <= unsentTcpLayerBytesM);

    unsentTcpLayerBytesM -= msgLengthP;
}

unsigned long TcpLwipMsgBasedSendQueue::getBytesAvailable() const
{
    return unsentTcpLayerBytesM;
}

TCPSegment* TcpLwipMsgBasedSendQueue::createSegmentWithBytes(
        const void* tcpDataP, unsigned int tcpLengthP)
{
    ASSERT(tcpDataP);

    PayloadQueue::iterator i;

    TCPSegment *tcpseg = new TCPSegment("tcp-segment");

    TCPSerializer().parse((const unsigned char *)tcpDataP, tcpLengthP, tcpseg, false);

    uint32 fromSeq = tcpseg->getSequenceNo();
    uint32 numBytes = tcpseg->getPayloadLength();
    uint32 toSeq = fromSeq+numBytes;

    if ((! isValidSeqNoM) && (numBytes > 0))
    {
        for (i = payloadQueueM.begin(); i != payloadQueueM.end(); ++i)
        {
            i->endSequenceNo += fromSeq;
        }

        beginM += fromSeq;
        endM += fromSeq;
        isValidSeqNoM = true;
    }

    if (numBytes && !seqLE(toSeq, endM))
        throw cRuntimeError("Implementation bug");

    EV << "sendQueue: " << connM->connIdM << ": [" << fromSeq << ":" << toSeq
       << ",l=" << numBytes << "] (unsent bytes:" << unsentTcpLayerBytesM << "\n";

#ifdef DEBUG_LWIP
    for (i = payloadQueueM.begin(); i != payloadQueueM.end(); ++i)
    {
        EV << "  buffered msg: endseq=" << i->endSequenceNo
           << ", length=" << i->msg->getByteLength() << endl;
    }
#endif

    const char *payloadName = NULL;

    if (numBytes > 0)
    {
        // add payload messages whose endSequenceNo is between fromSeq and fromSeq+numBytes
        i = payloadQueueM.begin();

        while (i != payloadQueueM.end() && seqLE(i->endSequenceNo, fromSeq))
            ++i;

        while (i != payloadQueueM.end() && seqLE(i->endSequenceNo, toSeq))
        {
            if (!payloadName)
                payloadName = i->msg->getName();

            cPacket* msg = i->msg->dup();
            tcpseg->addPayloadMessage(msg, i->endSequenceNo);
            ++i;
        }
    }

    // give segment a name
    char msgname[80];
    sprintf(msgname, "%.10s%s%s%s(l=%lu,%dmsg)",
            (payloadName ? payloadName : "tcpseg"),
            tcpseg->getSynBit() ? " SYN":"",
            tcpseg->getFinBit() ? " FIN":"",
            (tcpseg->getAckBit() && 0==numBytes) ? " ACK":"",
            (unsigned long)numBytes,
            tcpseg->getPayloadArraySize());
    tcpseg->setName(msgname);

    discardAckedBytes();

    return tcpseg;
}

void TcpLwipMsgBasedSendQueue::discardAckedBytes()
{
    if (isValidSeqNoM)
    {
        uint32 seqNum = connM->pcbM->lastack;

        if (seqLE(beginM, seqNum) && seqLE(seqNum, endM))
        {
            beginM = seqNum;

            // remove payload messages whose endSequenceNo is below seqNum
            while (!payloadQueueM.empty() && seqLE(payloadQueueM.front().endSequenceNo, seqNum))
            {
                delete payloadQueueM.front().msg;
                payloadQueueM.pop_front();
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////

TcpLwipMsgBasedReceiveQueue::TcpLwipMsgBasedReceiveQueue()
    :
    bytesInQueueM(0)
{
}

TcpLwipMsgBasedReceiveQueue::~TcpLwipMsgBasedReceiveQueue()
{
    PayloadList::iterator i;

    while ((i = payloadListM.begin()) != payloadListM.end())
    {
        delete i->second;
        payloadListM.erase(i);
    }
}

void TcpLwipMsgBasedReceiveQueue::setConnection(TcpLwipConnection *connP)
{
    ASSERT(connP);

    bytesInQueueM = 0;
    TcpLwipReceiveQueue::setConnection(connP);
    isValidSeqNoM = false;
    lastExtractedSeqNoM = 0;

    PayloadList::iterator i;

    while ((i = payloadListM.begin()) != payloadListM.end())
    {
        delete i->second;
        payloadListM.erase(i);
    }
}

void TcpLwipMsgBasedReceiveQueue::notifyAboutIncomingSegmentProcessing(
        TCPSegment *tcpsegP, uint32 seqNoP, const void* bufferP, size_t bufferLengthP)
{
    ASSERT(tcpsegP);
    ASSERT(bufferP);
    ASSERT(seqLE(tcpsegP->getSequenceNo(), seqNoP));
    uint32 lastSeqNo = seqNoP + bufferLengthP;
    ASSERT(seqGE(tcpsegP->getSequenceNo() + tcpsegP->getPayloadLength(), lastSeqNo));

    cPacket *msg;
    uint32 endSeqNo;

    while ((msg = tcpsegP->removeFirstPayloadMessage(endSeqNo)) != NULL)
    {
        if (seqLess(seqNoP, endSeqNo) && seqLE(endSeqNo, lastSeqNo)
                && (!isValidSeqNoM || seqLess(lastExtractedSeqNoM, endSeqNo)))
        {
            // insert, avoiding duplicates
            PayloadList::iterator i = payloadListM.find(endSeqNo);

            if (i != payloadListM.end())
            {
                ASSERT(msg->getByteLength() == i->second->getByteLength());
                delete i->second;
            }

            payloadListM[endSeqNo] = msg;
        }
        else
        {
            delete msg;
        }
    }
}

void TcpLwipMsgBasedReceiveQueue::enqueueTcpLayerData(void* dataP, unsigned int dataLengthP)
{
    bytesInQueueM += dataLengthP;
}

cPacket* TcpLwipMsgBasedReceiveQueue::extractBytesUpTo()
{
    ASSERT(connM);

    cPacket *dataMsg = NULL;

    if (!isValidSeqNoM)
    {
        isValidSeqNoM = true;
        lastExtractedSeqNoM = connM->pcbM->rcv_nxt - bytesInQueueM;

        if (connM->pcbM->state >= LwipTcpLayer::CLOSE_WAIT)
            lastExtractedSeqNoM--; // received FIN
    }

    uint32 firstSeqNo = lastExtractedSeqNoM;
    uint32 lastSeqNo = firstSeqNo + bytesInQueueM;

    // remove old messages
    while ( (! payloadListM.empty()) && seqLE(payloadListM.begin()->first, firstSeqNo))
    {
        EV << "Remove old payload MSG: seqno=" << payloadListM.begin()->first
           << ", len=" << payloadListM.begin()->second->getByteLength() << endl;
        delete payloadListM.begin()->second;
        payloadListM.erase(payloadListM.begin());
    }

    // pass up payload messages, in sequence number order
    if (! payloadListM.empty())
    {
        uint32 endSeqNo = payloadListM.begin()->first;

        if (seqLE(endSeqNo, lastSeqNo))
        {
            dataMsg = payloadListM.begin()->second;
            uint32 dataLength = dataMsg->getByteLength();

            ASSERT(endSeqNo - dataLength == firstSeqNo);

            payloadListM.erase(payloadListM.begin());
            lastExtractedSeqNoM += dataLength;
            bytesInQueueM -= dataLength;

            dataMsg->setKind(TCP_I_DATA);
        }
    }

    return dataMsg;
}

uint32 TcpLwipMsgBasedReceiveQueue::getAmountOfBufferedBytes() const
{
    return bytesInQueueM;
}

uint32 TcpLwipMsgBasedReceiveQueue::getQueueLength() const
{
    return payloadListM.size();
}

void TcpLwipMsgBasedReceiveQueue::getQueueStatus() const
{
    // TODO
}

void TcpLwipMsgBasedReceiveQueue::notifyAboutSending(const TCPSegment *tcpsegP)
{
    // nothing to do
}

