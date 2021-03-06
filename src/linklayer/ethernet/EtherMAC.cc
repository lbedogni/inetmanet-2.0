/*
 * Copyright (C) 2003 Andras Varga; CTIE, Monash University, Australia
 * Copyright (C) 2011 Zoltan Bojthe
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <string.h>

#include "EtherMAC.h"

#include "EtherFrame_m.h"
#include "Ethernet.h"
#include "Ieee802Ctrl_m.h"
#include "IPassiveQueue.h"

// TODO: there is some code that is pretty much the same as the one found in EtherMACFullDuplex.cc (e.g. EtherMAC::beginSendFrames)
// TODO: refactor using a statemachine that is present in a single function
// TODO: this helps understanding what interactions are there and how they affect the state


static std::ostream& operator<<(std::ostream& out, cMessage *msg)
{
    out << "(" << msg->getClassName() << ")" << msg->getFullName();
    return out;
}


Define_Module(EtherMAC);

simsignal_t EtherMAC::collisionSignal = SIMSIGNAL_NULL;
simsignal_t EtherMAC::backoffSignal = SIMSIGNAL_NULL;

EtherMAC::EtherMAC()
{
    frameBeingReceived = NULL;
    endJammingMsg = endRxMsg = endBackoffMsg = NULL;
}

EtherMAC::~EtherMAC()
{
    delete frameBeingReceived;
    cancelAndDelete(endRxMsg);
    cancelAndDelete(endBackoffMsg);
    cancelAndDelete(endJammingMsg);
}

void EtherMAC::initialize()
{
    EtherMACBase::initialize();

    endRxMsg = new cMessage("EndReception", ENDRECEPTION);
    endBackoffMsg = new cMessage("EndBackoff", ENDBACKOFF);
    endJammingMsg = new cMessage("EndJamming", ENDJAMMING);

    // initialize state info
    backoffs = 0;
    numConcurrentTransmissions = 0;
    currentSendPkTreeID = 0;

    WATCH(backoffs);
    WATCH(numConcurrentTransmissions);
}

void EtherMAC::initializeStatistics()
{
    EtherMACBase::initializeStatistics();

    framesSentInBurst = 0;
    bytesSentInBurst = 0;

    WATCH(framesSentInBurst);
    WATCH(bytesSentInBurst);

    // initialize statistics
    totalCollisionTime = 0.0;
    totalSuccessfulRxTxTime = 0.0;
    numCollisions = numBackoffs = 0;

    WATCH(numCollisions);
    WATCH(numBackoffs);

    collisionSignal = registerSignal("collision");
    backoffSignal = registerSignal("backoff");
}

void EtherMAC::initializeFlags()
{
    EtherMACBase::initializeFlags();

    duplexMode = par("duplexMode").boolValue();
    frameBursting = !duplexMode && par("frameBursting").boolValue();
    physInGate->setDeliverOnReceptionStart(true);
}

void EtherMAC::handleDisconnect()
{
    delete frameBeingReceived;
    frameBeingReceived = NULL;
    cancelEvent(endRxMsg);
    cancelEvent(endBackoffMsg);
    cancelEvent(endJammingMsg);
    bytesSentInBurst = 0;
    framesSentInBurst = 0;

    EtherMACBase::handleDisconnect();
}

void EtherMAC::calculateParameters(bool errorWhenAsymmetric)
{
    EtherMACBase::calculateParameters(errorWhenAsymmetric);

    if (connected && !duplexMode)
    {
        if (curEtherDescr->halfDuplexFrameMinBytes < 0.0)
            error("The %g bps ethernet supports only the full duplex connections", curEtherDescr->txrate);
    }
}

void EtherMAC::handleSelfMessage(cMessage *msg)
{
    // Process different self-messages (timer signals)
    EV << "Self-message " << msg << " received\n";

    switch (msg->getKind())
    {
        case ENDIFG:
            handleEndIFGPeriod();
            break;

        case ENDTRANSMISSION:
            handleEndTxPeriod();
            break;

        case ENDRECEPTION:
            handleEndRxPeriod();
            break;

        case ENDBACKOFF:
            handleEndBackoffPeriod();
            break;

        case ENDJAMMING:
            handleEndJammingPeriod();
            break;

        case ENDPAUSE:
            handleEndPausePeriod();
            break;

        default:
            throw cRuntimeError("Self-message with unexpected message kind %d", msg->getKind());
    }
}

void EtherMAC::handleMessage(cMessage *msg)
{
    if (dataratesDiffer)
        calculateParameters(true);

    printState();

    // some consistency check
    if (!duplexMode && transmitState == TRANSMITTING_STATE && receiveState != RX_IDLE_STATE)
        error("Inconsistent state -- transmitting and receiving at the same time");

    if (msg->isSelfMessage())
        handleSelfMessage(msg);
    else if (!connected)
        processMessageWhenNotConnected(msg);
    else if (disabled)
        processMessageWhenDisabled(msg);
    else if (msg->getArrivalGate() == upperLayerInGate)
        processFrameFromUpperLayer(check_and_cast<EtherFrame *>(msg));
    else if (msg->getArrivalGate() == physInGate)
        processMsgFromNetwork(check_and_cast<EtherTraffic *>(msg));
    else
        throw cRuntimeError("Message received from unknown gate");

    if (ev.isGUI())
        updateDisplayString();

    printState();
}

void EtherMAC::processFrameFromUpperLayer(EtherFrame *frame)
{
    if (frame->getByteLength() < MIN_ETHERNET_FRAME_BYTES)
        throw cRuntimeError("Ethernet frame too short, must be at least 64 bytes (padding should be done at encapsulation)");

    frame->setFrameByteLength(frame->getByteLength());

    EV << "Received frame from upper layer: " << frame << endl;

    emit(packetReceivedFromUpperSignal, frame);

    if (frame->getDest().equals(address))
    {
        error("logic error: frame %s from higher layer has local MAC address as dest (%s)",
                frame->getFullName(), frame->getDest().str().c_str());
    }

    if (frame->getByteLength() > MAX_ETHERNET_FRAME_BYTES)
    {
        error("packet from higher layer (%d bytes) exceeds maximum Ethernet frame size (%d)",
                (int)(frame->getByteLength()), MAX_ETHERNET_FRAME_BYTES);
    }

    // fill in src address if not set
    if (frame->getSrc().isUnspecified())
        frame->setSrc(address);

    bool isPauseFrame = (dynamic_cast<EtherPauseFrame*>(frame) != NULL);

    if (!isPauseFrame)
    {
        numFramesFromHL++;
        emit(rxPkFromHLSignal, frame);
    }

    if (txQueue.extQueue)
    {
        ASSERT(curTxFrame == NULL);
        curTxFrame = frame;
    }
    else
    {
        if (txQueue.innerQueue->isFull())
            error("txQueue length exceeds %d -- this is probably due to "
                  "a bogus app model generating excessive traffic "
                  "(or if this is normal, increase txQueueLimit!)",
                  txQueue.innerQueue->getQueueLimit());

        // store frame and possibly begin transmitting
        EV << "Frame " << frame << " arrived from higher layers, enqueueing\n";
        txQueue.innerQueue->insertFrame(frame);

        if (!curTxFrame && !txQueue.innerQueue->empty())
            curTxFrame = (EtherFrame*)txQueue.innerQueue->pop();
    }

    if ((duplexMode || receiveState == RX_IDLE_STATE) && transmitState == TX_IDLE_STATE)
    {
        EV << "No incoming carrier signals detected, frame clear to send, wait IFG first\n";
        scheduleEndIFGPeriod();
    }
}

void EtherMAC::removeExpiredEndRxTimes()
{
    // remove expired entries from endRxTimeList
    simtime_t now = simTime();
    while (!endRxTimeList.empty() && endRxTimeList.front().endTime <= now)
        endRxTimeList.pop_front();
}

simtime_t EtherMAC::insertEndReception(long packetTreeId, simtime_t endRxTime)
{
    ASSERT(packetTreeId != 0);

    // remove expired entries from endRxTimeList
    removeExpiredEndRxTimes();

    EndRxTimeList::iterator i;

    // remove old entry with same packet tree ID
    for (i = endRxTimeList.begin(); i != endRxTimeList.end(); i++)
    {
        if (i->packetTreeId == packetTreeId)
        {
            endRxTimeList.erase(i);
            break;
        }
    }

    // find insertion position
    for (i = endRxTimeList.begin(); i != endRxTimeList.end() && i->endTime <= endRxTime; i++)
        ;

    // insert
    PkIdRxTime item(packetTreeId, endRxTime);
    i = endRxTimeList.insert(i, item);

    numConcurrentTransmissions = endRxTimeList.size();

    // return the highest endTime (stored in last element)
    simtime_t maxRxTime = endRxTimeList.back().endTime;
    return maxRxTime;
}

void EtherMAC::processReceivedJam(EtherJam *jam)
{
    simtime_t newRxTime = insertEndReception(jam->getAbortedPkTreeID(), simTime() + jam->getDuration());
    cancelEvent(endRxMsg);
    scheduleAt(newRxTime, endRxMsg);
    delete jam;
    processDetectedCollision();
}

void EtherMAC::processMsgFromNetwork(EtherTraffic *msg)
{
    EV << "Received frame from network: " << msg << endl;

    // detect cable length violation in half-duplex mode
    if (!duplexMode)
    {
        if (simTime() - msg->getSendingTime() >= curEtherDescr->maxPropagationDelay)
        {
            error("very long frame propagation time detected, "
                  "maybe cable exceeds maximum allowed length? "
                  "(%lgs corresponds to an approx. %lgm cable)",
                  SIMTIME_STR(simTime() - msg->getSendingTime()),
                  SIMTIME_STR((simTime() - msg->getSendingTime()) * SPEED_OF_LIGHT_IN_CABLE));
        }
    }

    simtime_t endRxTime = simTime() + msg->getDuration();
    bool isJam = dynamic_cast<EtherJam*>(msg) != NULL;

    if (!duplexMode && (transmitState == TRANSMITTING_STATE || transmitState == SEND_IFG_STATE))
    {
        // since we're halfduplex, receiveState must be RX_IDLE_STATE (asserted at top of handleMessage)
        if (isJam)
            error("Stray jam signal arrived while transmitting (usual cause is cable length exceeding allowed maximum)");

        // set receive state and schedule end of reception
        receiveState = RX_COLLISION_STATE;

        simtime_t newTime = insertEndReception(msg->getTreeId(), endRxTime);
        cancelEvent(endRxMsg);
        scheduleAt(newTime, endRxMsg);
        delete msg;

        EV << "Transmission interrupted by incoming frame, handling collision\n";
        cancelEvent((transmitState==TRANSMITTING_STATE) ? endTxMsg : endIFGMsg);

        EV << "Transmitting jam signal\n";
        sendJamSignal(); // backoff will be executed when jamming finished

        numCollisions++;
        emit(collisionSignal, 1L);
    }
    else if (receiveState == RX_IDLE_STATE)
    {
        channelBusySince = simTime();
        if (isJam)
        {
            EV << "Stray jam signal arrived (usual cause is cable length exceeding allowed maximum)";
            processReceivedJam((EtherJam *)msg);
        }
        else
        {
            EV << "Start reception of frame\n";
            scheduleEndRxPeriod(msg);
        }
    }
    else if (receiveState == RECEIVING_STATE
            && !isJam
            && endRxMsg->getArrivalTime() - simTime() < curEtherDescr->halfBitTime)
    {
        // With the above condition we filter out "false" collisions that may occur with
        // back-to-back frames. That is: when "beginning of frame" message (this one) occurs
        // BEFORE "end of previous frame" event (endRxMsg) -- same simulation time,
        // only wrong order.

        EV << "Back-to-back frames: completing reception of current frame, starting reception of next one\n";

        // complete reception of previous frame
        cancelEvent(endRxMsg);
        frameReceptionComplete();

        // calculate usability
        totalSuccessfulRxTxTime += simTime()-channelBusySince;
        channelBusySince = simTime();

        // start receiving next frame
        scheduleEndRxPeriod(msg);
    }
    else // (receiveState==RECEIVING_STATE || receiveState==RX_COLLISION_STATE)
    {
        // handle overlapping receptions
        if (isJam)
        {
            processReceivedJam((EtherJam *)msg);
        }
        else // EtherFrame or EtherPauseFrame
        {
            EV << "Overlapping receptions -- setting collision state\n";
            endRxTime = insertEndReception(msg->getTreeId(), endRxTime);
            cancelEvent(endRxMsg);
            scheduleAt(endRxTime, endRxMsg);
            // delete collided frames: arrived frame as well as the one we're currently receiving
            delete msg;
            processDetectedCollision();
        }
    }
}

void EtherMAC::processDetectedCollision()
{
    if (receiveState != RX_COLLISION_STATE)
    {
        delete frameBeingReceived;
        frameBeingReceived = NULL;

        numCollisions++;
        emit(collisionSignal, 1L);
        // go to collision state
        receiveState = RX_COLLISION_STATE;
    }
}

void EtherMAC::handleEndIFGPeriod()
{
    if (transmitState != WAIT_IFG_STATE && transmitState != SEND_IFG_STATE)
        error("Not in WAIT_IFG_STATE at the end of IFG period");

    currentSendPkTreeID = 0;

    if (NULL == curTxFrame)
        error("End of IFG and no frame to transmit");

    EV << "IFG elapsed, now begin transmission of frame " << curTxFrame << endl;

    // End of IFG period, okay to transmit, if Rx idle OR duplexMode ( checked in startFrameTransmission(); )

    // send frame to network
    startFrameTransmission();
}

void EtherMAC::startFrameTransmission()
{
    EV << "Transmitting a copy of frame " << curTxFrame << endl;

    EtherFrame *frame = curTxFrame->dup();

    if (frame->getSrc().isUnspecified())
        frame->setSrc(address);

    bool inBurst = frameBursting && framesSentInBurst;
    int64 minFrameLength = duplexMode ? curEtherDescr->frameMinBytes : (inBurst ? curEtherDescr->frameInBurstMinBytes : curEtherDescr->halfDuplexFrameMinBytes);

    if (frame->getByteLength() < minFrameLength)
        frame->setByteLength(minFrameLength);

    // add preamble and SFD (Starting Frame Delimiter), then send out
    frame->addByteLength(PREAMBLE_BYTES+SFD_BYTES);

    if (ev.isGUI())
        updateConnectionColor(TRANSMITTING_STATE);

    currentSendPkTreeID = frame->getTreeId();
    send(frame, physOutGate);

    // check for collisions (there might be an ongoing reception which we don't know about, see below)
    if (!duplexMode && receiveState != RX_IDLE_STATE)
    {
        // During the IFG period the hardware cannot listen to the channel,
        // so it might happen that receptions have begun during the IFG,
        // and even collisions might be in progress.
        //
        // But we don't know of any ongoing transmission so we blindly
        // start transmitting, immediately collide and send a jam signal.
        //
        EV << "startFrameTransmission() send JAM signal.\n";
        printState();

        sendJamSignal();
        // numConcurrentRxTransmissions stays the same: +1 transmission, -1 jam

        if (receiveState == RECEIVING_STATE)
        {
            delete frameBeingReceived;
            frameBeingReceived = NULL;

            numCollisions++;
            emit(collisionSignal, 1L);
        }
        // go to collision state
        receiveState = RX_COLLISION_STATE;
    }
    else
    {
        // no collision
        scheduleEndTxPeriod(frame);

        // only count transmissions in totalSuccessfulRxTxTime if channel is half-duplex
        if (!duplexMode)
            channelBusySince = simTime();
    }
}

void EtherMAC::handleEndTxPeriod()
{
    // we only get here if transmission has finished successfully, without collision
    if (transmitState != TRANSMITTING_STATE || (!duplexMode && receiveState != RX_IDLE_STATE))
        error("End of transmission, and incorrect state detected");

    currentSendPkTreeID = 0;

    if (NULL == curTxFrame)
        error("Frame under transmission cannot be found");

    emit(packetSentToLowerSignal, curTxFrame);  //consider: emit with start time of frame

    if (dynamic_cast<EtherPauseFrame*>(curTxFrame) != NULL)
    {
        numPauseFramesSent++;
        emit(txPausePkUnitsSignal, ((EtherPauseFrame*)curTxFrame)->getPauseTime());
    }
    else
    {
        unsigned long curBytes = curTxFrame->getFrameByteLength();
        numFramesSent++;
        numBytesSent += curBytes;
        emit(txPkSignal, curTxFrame);
    }

    EV << "Transmission of " << curTxFrame << " successfully completed\n";
    delete curTxFrame;
    curTxFrame = NULL;
    lastTxFinishTime = simTime();
    getNextFrameFromQueue();

    // only count transmissions in totalSuccessfulRxTxTime if channel is half-duplex
    if (!duplexMode)
    {
        simtime_t dt = simTime() - channelBusySince;
        totalSuccessfulRxTxTime += dt;
    }

    backoffs = 0;

    // check for and obey received PAUSE frames after each transmission
    if (checkAndScheduleEndPausePeriod())
        return;

    beginSendFrames();
}

void EtherMAC::scheduleEndRxPeriod(EtherTraffic *frame)
{
    ASSERT(frameBeingReceived == NULL);

    frameBeingReceived = frame;
    receiveState = RECEIVING_STATE;
    simtime_t endRxTime = insertEndReception(frame->getTreeId(), simTime() + frame->getDuration());
    scheduleAt(endRxTime, endRxMsg);
}

void EtherMAC::handleEndRxPeriod()
{
    EV << "Frame reception complete\n";
    removeExpiredEndRxTimes();

    simtime_t dt = simTime() - channelBusySince;

    if (receiveState == RECEIVING_STATE) // i.e. not RX_COLLISION_STATE
    {
        frameReceptionComplete();
        totalSuccessfulRxTxTime += dt;
    }
    else
    {
        totalCollisionTime += dt;
    }

    receiveState = RX_IDLE_STATE;
    numConcurrentTransmissions = 0;

    if (transmitState == TX_IDLE_STATE)
        beginSendFrames();
}

void EtherMAC::handleEndBackoffPeriod()
{
    if (transmitState != BACKOFF_STATE)
        error("At end of BACKOFF not in BACKOFF_STATE!");

    if (NULL == curTxFrame)
        error("At end of BACKOFF and not have current tx frame!");

    if (receiveState == RX_IDLE_STATE)
    {
        EV << "Backoff period ended, wait IFG\n";
        scheduleEndIFGPeriod();
    }
    else
    {
        EV << "Backoff period ended but channel not free, idling\n";
        transmitState = TX_IDLE_STATE;
    }
}

void EtherMAC::sendJamSignal()
{
    if (currentSendPkTreeID == 0)
        throw cRuntimeError("model error: send JAM without transmit packet");

    EtherJam *jam = new EtherJam("JAM_SIGNAL");
    jam->setByteLength(JAM_SIGNAL_BYTES);
    jam->setAbortedPkTreeID(currentSendPkTreeID);

    transmissionChannel->forceTransmissionFinishTime(SIMTIME_ZERO);
    emit(packetSentToLowerSignal, jam);
    send(jam, physOutGate);

    scheduleAt(transmissionChannel->getTransmissionFinishTime(), endJammingMsg);
    transmitState = JAMMING_STATE;

    if (ev.isGUI())
        updateConnectionColor(JAMMING_STATE);
}

void EtherMAC::handleEndJammingPeriod()
{
    if (transmitState != JAMMING_STATE)
        error("At end of JAMMING not in JAMMING_STATE!");

    EV << "Jamming finished, executing backoff\n";
    handleRetransmission();
}

void EtherMAC::handleRetransmission()
{
    if (++backoffs > MAX_ATTEMPTS)
    {
        EV << "Number of retransmit attempts of frame exceeds maximum, cancelling transmission of frame\n";
        delete curTxFrame;
        curTxFrame = NULL;
        transmitState = TX_IDLE_STATE;
        backoffs = 0;
        getNextFrameFromQueue();
        beginSendFrames();
        return;
    }

    EV << "Executing backoff procedure\n";
    int backoffrange = (backoffs >= BACKOFF_RANGE_LIMIT) ? 1024 : (1 << backoffs);
    int slotNumber = intuniform(0, backoffrange-1);

    scheduleAt(simTime() + slotNumber *curEtherDescr->slotTime, endBackoffMsg);
    transmitState = BACKOFF_STATE;

    numBackoffs++;
    emit(backoffSignal, 1L);
}

void EtherMAC::printState()
{
#define CASE(x) case x: EV << #x; break

    EV << "transmitState: ";
    switch (transmitState)
    {
        CASE(TX_IDLE_STATE);
        CASE(WAIT_IFG_STATE);
        CASE(SEND_IFG_STATE);
        CASE(TRANSMITTING_STATE);
        CASE(JAMMING_STATE);
        CASE(BACKOFF_STATE);
        CASE(PAUSE_STATE);
    }

    EV << ",  receiveState: ";
    switch (receiveState)
    {
        CASE(RX_IDLE_STATE);
        CASE(RECEIVING_STATE);
        CASE(RX_COLLISION_STATE);
    }

    EV << ",  backoffs: " << backoffs;
    EV << ",  numConcurrentRxTransmissions: " << numConcurrentTransmissions;

    if (txQueue.innerQueue)
        EV << ",  queueLength: " << txQueue.innerQueue->length();

    EV << endl;

#undef CASE
}

void EtherMAC::finish()
{
    EtherMACBase::finish();

    simtime_t t = simTime();
    simtime_t totalChannelIdleTime = t - totalSuccessfulRxTxTime - totalCollisionTime;
    recordScalar("rx channel idle (%)", 100*(totalChannelIdleTime/t));
    recordScalar("rx channel utilization (%)", 100*(totalSuccessfulRxTxTime/t));
    recordScalar("rx channel collision (%)", 100*(totalCollisionTime/t));
    recordScalar("collisions", numCollisions);
    recordScalar("backoffs", numBackoffs);
}

void EtherMAC::processMessageWhenNotConnected(cMessage *msg)
{
    EV << "Interface is not connected -- dropping packet " << msg << endl;
    emit(dropPkIfaceDownSignal, msg);
    numDroppedIfaceDown++;
    delete msg;

    requestNextFrameFromExtQueue();
}

void EtherMAC::processMessageWhenDisabled(cMessage *msg)
{
    EV << "MAC is disabled -- dropping message " << msg << endl;
    emit(dropPkIfaceDownSignal, msg);
    delete msg;

    requestNextFrameFromExtQueue();
}

void EtherMAC::handleEndPausePeriod()
{
    if (transmitState != PAUSE_STATE)
        error("At end of PAUSE not in PAUSE_STATE!");

    EV << "Pause finished, resuming transmissions\n";
    beginSendFrames();
}

void EtherMAC::frameReceptionComplete()
{
    EtherTraffic *msg = frameBeingReceived;
    frameBeingReceived = NULL;

    if ((dynamic_cast<EtherIFG*>(msg)) != NULL)
    {
        delete msg;
        return;
    }

    EtherFrame *frame = check_and_cast<EtherFrame *>(msg);

    emit(packetReceivedFromLowerSignal, frame);

    // bit errors
    if (frame->hasBitError())
    {
        numDroppedBitError++;
        emit(dropPkBitErrorSignal, frame);
        delete msg;
        return;
    }

    if (dropFrameNotForUs(frame))
        return;

    if (dynamic_cast<EtherPauseFrame*>(frame) != NULL)
    {
        int pauseUnits = ((EtherPauseFrame*)frame)->getPauseTime();
        delete frame;
        numPauseFramesRcvd++;
        emit(rxPausePkUnitsSignal, pauseUnits);
        processPauseCommand(pauseUnits);
    }
    else
    {
        processReceivedDataFrame(check_and_cast<EtherFrame *>(frame));
    }
}

void EtherMAC::processReceivedDataFrame(EtherFrame *frame)
{
    // strip physical layer overhead (preamble, SFD, carrier extension) from frame
    frame->setByteLength(frame->getFrameByteLength());

    // statistics
    unsigned long curBytes = frame->getByteLength();
    numFramesReceivedOK++;
    numBytesReceivedOK += curBytes;
    emit(rxPkOkSignal, frame);

    numFramesPassedToHL++;
    emit(packetSentToUpperSignal, frame);
    // pass up to upper layer
    send(frame, "upperLayerOut");
}

void EtherMAC::processPauseCommand(int pauseUnits)
{
    if (transmitState == TX_IDLE_STATE)
    {
        EV << "PAUSE frame received, pausing for " << pauseUnitsRequested << " time units\n";
        if (pauseUnits > 0)
            scheduleEndPausePeriod(pauseUnits);
    }
    else if (transmitState == PAUSE_STATE)
    {
        EV << "PAUSE frame received, pausing for " << pauseUnitsRequested
           << " more time units from now\n";
        cancelEvent(endPauseMsg);

        if (pauseUnits > 0)
            scheduleEndPausePeriod(pauseUnits);
    }
    else
    {
        // transmitter busy -- wait until it finishes with current frame (endTx)
        // and then it'll go to PAUSE state
        EV << "PAUSE frame received, storing pause request\n";
        pauseUnitsRequested = pauseUnits;
    }
}

void EtherMAC::scheduleEndIFGPeriod()
{
    ASSERT(curTxFrame);

    if (frameBursting
            && (simTime() == lastTxFinishTime)
            && (framesSentInBurst > 0)
            && (framesSentInBurst < curEtherDescr->maxFramesInBurst)
            && (bytesSentInBurst + (INTERFRAME_GAP_BITS / 8) + curTxFrame->getByteLength()
                    <= curEtherDescr->maxBytesInBurst)
       )
    {
        EtherIFG *gap = new EtherIFG("IFG");
        bytesSentInBurst += gap->getByteLength();
        currentSendPkTreeID = gap->getTreeId();
        send(gap, physOutGate);
        transmitState = SEND_IFG_STATE;
        scheduleAt(transmissionChannel->getTransmissionFinishTime(), endIFGMsg);
        // FIXME Check collision?
    }
    else
    {
        EtherIFG gap;
        bytesSentInBurst = 0;
        framesSentInBurst = 0;
        transmitState = WAIT_IFG_STATE;
        scheduleAt(simTime() + transmissionChannel->calculateDuration(&gap), endIFGMsg);
    }
}

void EtherMAC::scheduleEndTxPeriod(EtherFrame *frame)
{
    // update burst variables
    if (frameBursting)
    {
        bytesSentInBurst += frame->getByteLength();
        framesSentInBurst++;
    }

    scheduleAt(transmissionChannel->getTransmissionFinishTime(), endTxMsg);
    transmitState = TRANSMITTING_STATE;
}

void EtherMAC::scheduleEndPausePeriod(int pauseUnits)
{
    // length is interpreted as 512-bit-time units
    cPacket pause;
    pause.setBitLength(pauseUnits * PAUSE_UNIT_BITS);
    simtime_t pausePeriod = transmissionChannel->calculateDuration(&pause);
    scheduleAt(simTime() + pausePeriod, endPauseMsg);
    transmitState = PAUSE_STATE;
}

bool EtherMAC::checkAndScheduleEndPausePeriod()
{
    if (pauseUnitsRequested > 0)
    {
        // if we received a PAUSE frame recently, go into PAUSE state
        EV << "Going to PAUSE mode for " << pauseUnitsRequested << " time units\n";

        scheduleEndPausePeriod(pauseUnitsRequested);
        pauseUnitsRequested = 0;
        return true;
    }

    return false;
}

void EtherMAC::beginSendFrames()
{
    if (curTxFrame)
    {
        // Other frames are queued, therefore wait IFG period and transmit next frame
        EV << "Transmit next frame in output queue, after IFG period\n";
        scheduleEndIFGPeriod();
    }
    else
    {
        transmitState = TX_IDLE_STATE;
        // No more frames set transmitter to idle
        EV << "No more frames to send, transmitter set to idle\n";
    }
}

