//
// Copyright (C) 2006 Andras Babos and Andras Varga
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

#include "OSPFNeighborStateAttempt.h"
#include "OSPFNeighborStateDown.h"
#include "OSPFNeighborStateInit.h"
#include "MessageHandler.h"
#include "OSPFInterface.h"
#include "OSPFArea.h"
#include "OSPFRouter.h"

void OSPF::NeighborStateAttempt::processEvent(OSPF::Neighbor* neighbor, OSPF::Neighbor::NeighborEventType event)
{
    if ((event == OSPF::Neighbor::KILL_NEIGHBOR) || (event == OSPF::Neighbor::LINK_DOWN)) {
        MessageHandler* messageHandler = neighbor->getInterface()->getArea()->getRouter()->getMessageHandler();
        neighbor->reset();
        messageHandler->clearTimer(neighbor->getInactivityTimer());
        changeState(neighbor, new OSPF::NeighborStateDown, this);
    }
    if (event == OSPF::Neighbor::INACTIVITY_TIMER) {
        neighbor->reset();
        if (neighbor->getInterface()->getType() == OSPF::Interface::NBMA) {
            MessageHandler* messageHandler = neighbor->getInterface()->getArea()->getRouter()->getMessageHandler();
            messageHandler->startTimer(neighbor->getPollTimer(), neighbor->getInterface()->getPollInterval());
        }
        changeState(neighbor, new OSPF::NeighborStateDown, this);
    }
    if (event == OSPF::Neighbor::HELLO_RECEIVED) {
        MessageHandler* messageHandler = neighbor->getInterface()->getArea()->getRouter()->getMessageHandler();
        messageHandler->clearTimer(neighbor->getInactivityTimer());
        messageHandler->startTimer(neighbor->getInactivityTimer(), neighbor->getRouterDeadInterval());
        changeState(neighbor, new OSPF::NeighborStateInit, this);
    }
}
