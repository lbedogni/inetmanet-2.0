//
// Copyright (C) 2000 Institut fuer Telematik, Universitaet Karlsruhe
// Copyright (C) 2007 Universidad de Málaga
// Copyright (C) 2011 Zoltan Bojthe
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//


#include "UDPBasicBurst.h"

#include "UDPControlInfo_m.h"
#include "IPvXAddressResolver.h"
#include "InterfaceTable.h"
#include "InterfaceTableAccess.h"


EXECUTE_ON_STARTUP(
    cEnum *e = cEnum::find("ChooseDestAddrMode");
    if (!e) enums.getInstance()->add(e = new cEnum("ChooseDestAddrMode"));
    e->insert(UDPBasicBurst::ONCE, "once");
    e->insert(UDPBasicBurst::PER_BURST, "perBurst");
    e->insert(UDPBasicBurst::PER_SEND, "perSend");
);

Define_Module(UDPBasicBurst);

int UDPBasicBurst::counter;

simsignal_t UDPBasicBurst::sentPkSignal = SIMSIGNAL_NULL;
simsignal_t UDPBasicBurst::rcvdPkSignal = SIMSIGNAL_NULL;
simsignal_t UDPBasicBurst::outOfOrderPkSignal = SIMSIGNAL_NULL;
simsignal_t UDPBasicBurst::dropPkSignal = SIMSIGNAL_NULL;

UDPBasicBurst::UDPBasicBurst()
{
    messageLengthPar = NULL;
    burstDurationPar = NULL;
    sleepDurationPar = NULL;
    sendIntervalPar = NULL;
    timerNext = NULL;
    outputInterface = -1;
    outputInterfaceMulticastBroadcast = -1;
}

UDPBasicBurst::~UDPBasicBurst()
{
    cancelAndDelete(timerNext);
}

void UDPBasicBurst::initialize(int stage)
{
    // because of IPvXAddressResolver, we need to wait until interfaces are registered,
    // address auto-assignment takes place etc.
    if (stage != 3)
        return;

    counter = 0;
    numSent = 0;
    numReceived = 0;
    numDeleted = 0;
    numDuplicated = 0;

    delayLimit = par("delayLimit");
    simtime_t startTime = par("startTime");
    stopTime = par("stopTime");

    messageLengthPar = &par("messageLength");
    burstDurationPar = &par("burstDuration");
    sleepDurationPar = &par("sleepDuration");
    sendIntervalPar = &par("sendInterval");
    nextSleep = startTime;
    nextBurst = startTime;
    nextPkt = startTime;

    destAddrRNG = par("destAddrRNG");
    const char *addrModeStr = par("chooseDestAddrMode").stringValue();
    int addrMode = cEnum::get("ChooseDestAddrMode")->lookup(addrModeStr);
    if (addrMode == -1)
        throw cRuntimeError(this, "Invalid chooseDestAddrMode: '%s'", addrModeStr);
    chooseDestAddrMode = (ChooseDestAddrMode)addrMode;

    WATCH(numSent);
    WATCH(numReceived);
    WATCH(numDeleted);
    WATCH(numDuplicated);

    localPort = par("localPort");
    destPort = par("destPort");

    socket.setOutputGate(gate("udpOut"));
    socket.bind(localPort);
    if (par("setBroadcast").boolValue())
        socket.setBroadcast(true);

    if (strcmp(par("outputInterface").stringValue(),"") != 0)
    {
        IInterfaceTable* ift = InterfaceTableAccess().get();
        InterfaceEntry *ie = ift->getInterfaceByName(par("outputInterface").stringValue());
        if (ie == NULL)
            throw cRuntimeError(this, "Invalid output interface name : %s",par("outputInterface").stringValue());
        outputInterface = ie->getInterfaceId();
    }

    if (strcmp(par("outputInterfaceMulticastBroadcast").stringValue(),"") != 0)
    {
        IInterfaceTable* ift = InterfaceTableAccess().get();
        InterfaceEntry *ie = ift->getInterfaceByName(par("outputInterfaceMulticastBroadcast").stringValue());
        if (ie == NULL)
            throw cRuntimeError(this, "Invalid output interface name : %s",par("outputInterfaceMulticastBroadcast").stringValue());
        outputInterfaceMulticastBroadcast = ie->getInterfaceId();
    }


    const char *destAddrs = par("destAddresses");
    cStringTokenizer tokenizer(destAddrs);
    const char *token;

    IPvXAddress myAddr = IPvXAddressResolver().resolve(this->getParentModule()->getFullPath().c_str());
    while ((token = tokenizer.nextToken()) != NULL)
    {
        if (strstr(token, "Broadcast") != NULL)
            destAddresses.push_back(IPv4Address::ALLONES_ADDRESS);
        else
        {
            IPvXAddress addr = IPvXAddressResolver().resolve(token);
            if (addr != myAddr)
                destAddresses.push_back(addr);
        }
    }

    isSource = !destAddresses.empty();

    if (isSource)
    {
        if (chooseDestAddrMode == ONCE)
            destAddr = chooseDestAddr();

        activeBurst = true;

        timerNext = new cMessage("UDPBasicBurstTimer");
        scheduleAt(startTime, timerNext);
    }

    sentPkSignal = registerSignal("sentPk");
    rcvdPkSignal = registerSignal("rcvdPk");
    outOfOrderPkSignal = registerSignal("outOfOrderPk");
    dropPkSignal = registerSignal("dropPk");
}

IPvXAddress UDPBasicBurst::chooseDestAddr()
{
    if (destAddresses.size() == 1)
        return destAddresses[0];

    int k = genk_intrand(destAddrRNG, destAddresses.size());
    return destAddresses[k];
}

cPacket *UDPBasicBurst::createPacket()
{
    char msgName[32];
    sprintf(msgName, "UDPBasicAppData-%d", counter++);
    long msgByteLength = messageLengthPar->longValue();
    cPacket *payload = new cPacket(msgName);
    payload->setByteLength(msgByteLength);
    payload->addPar("sourceId") = getId();
    payload->addPar("msgId") = numSent;

    return payload;
}

void UDPBasicBurst::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage())
    {
        if (stopTime <= 0 || simTime() < stopTime)
        {
            // send and reschedule next sending
            if (isSource) // if the node is a sink, don't generate messages
                generateBurst();
        }
    }
    else if (msg->getKind() == UDP_I_DATA)
    {
        // process incoming packet
        processPacket(PK(msg));
    }
    else if (msg->getKind() == UDP_I_ERROR)
    {
        EV << "Ignoring UDP error report\n";
        delete msg;
    }
    else
    {
        error("Unrecognized message (%s)%s", msg->getClassName(), msg->getName());
    }

    if (ev.isGUI())
    {
        char buf[40];
        sprintf(buf, "rcvd: %d pks\nsent: %d pks", numReceived, numSent);
        getDisplayString().setTagArg("t", 0, buf);
    }
}

void UDPBasicBurst::processPacket(cPacket *pk)
{
    if (pk->getKind() == UDP_I_ERROR)
    {
        EV << "UDP error received\n";
        delete pk;
        return;
    }

    if (pk->hasPar("sourceId") && pk->hasPar("msgId"))
    {
        // duplicate control
        int moduleId = (int)pk->par("sourceId");
        int msgId = (int)pk->par("msgId");
        SourceSequence::iterator it = sourceSequence.find(moduleId);
        if (it != sourceSequence.end())
        {
            if (it->second >= msgId)
            {
                EV << "Out of order packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
                emit(outOfOrderPkSignal, pk);
                delete pk;
                numDuplicated++;
                return;
            }
            else
                it->second = msgId;
        }
        else
            sourceSequence[moduleId] = msgId;
    }

    if (delayLimit > 0)
    {
        if (simTime() - pk->getTimestamp() > delayLimit)
        {
            EV << "Old packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
            emit(dropPkSignal, pk);
            delete pk;
            numDeleted++;
            return;
        }
    }

    EV << "Received packet: " << UDPSocket::getReceivedPacketInfo(pk) << endl;
    emit(rcvdPkSignal, pk);
    numReceived++;
    delete pk;
}

void UDPBasicBurst::generateBurst()
{
    simtime_t now = simTime();

    if (nextPkt < now)
        nextPkt = now;

    double sendInterval = sendIntervalPar->doubleValue();
    if (sendInterval <= 0.0)
        throw cRuntimeError(this, "The sendInterval parameter must be bigger than 0");
    nextPkt += sendInterval;

    if (activeBurst && nextBurst <= now) // new burst
    {
        double burstDuration = burstDurationPar->doubleValue();
        if (burstDuration < 0.0)
            throw cRuntimeError(this, "The burstDuration parameter mustn't be smaller than 0");
        double sleepDuration = sleepDurationPar->doubleValue();

        if (burstDuration == 0.0)
            activeBurst = false;
        else
        {
            if (sleepDuration < 0.0)
                throw cRuntimeError(this, "The sleepDuration parameter mustn't be smaller than 0");
            nextSleep = now + burstDuration;
            nextBurst = nextSleep + sleepDuration;
        }

        if (chooseDestAddrMode == PER_BURST)
            destAddr = chooseDestAddr();
    }

    if (chooseDestAddrMode == PER_SEND)
        destAddr = chooseDestAddr();

    cPacket *payload = createPacket();
    payload->setTimestamp();
    emit(sentPkSignal, payload);

    // Check address type
    if (outputInterfaceMulticastBroadcast != -1 && (destAddr.isMulticast() || (!destAddr.isIPv6() && destAddr.get4() == IPv4Address::ALLONES_ADDRESS)))
        socket.sendTo(payload, destAddr, destPort,outputInterfaceMulticastBroadcast);
    else
        socket.sendTo(payload, destAddr, destPort,outputInterface);

    numSent++;

    // Next timer
    if (activeBurst && nextPkt >= nextSleep)
        nextPkt = nextBurst;

    scheduleAt(nextPkt, timerNext);
}

void UDPBasicBurst::finish()
{
    recordScalar("Total sent", numSent);
    recordScalar("Total received", numReceived);
    recordScalar("Total deleted", numDeleted);
}


