//
// Copyright (C) 2008 Alfonso Ariza
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


package inet.linklayer.ieee80211mesh.nodes;
import inet.nodes.inet.WirelessHost;

import inet.util.PcapRecorder;
import inet.networklayer.ipv4.RoutingTable;
import inet.networklayer.common.InterfaceTable;
import inet.mobility.IMobility;
import inet.linklayer.ieee80211mesh.ethernet.EthernetInterfaceMesh;
import inet.linklayer.IWirelessNic;
import inet.linklayer.IWiredNic;
import inet.linklayer.IExternalNic;
import inet.base.NotificationBoard;
import inet.nodes.inet.NetworkLayer;
import inet.linklayer.ieee80211mesh.Ieee80211NewNicMesh;


//
// BaseHost contains the common lower layers (linklayer and networklayer)
// of Router, StandardHost, WirelessHost etc. 
// 
// NOTE: - IPForward is turned on by default.
//       - use this model only as a base type. In simulations use Router, StandardHost,
//         WirelessHost and AndhocHost
//
module gateWayMesh
{
    parameters:
        @display("bgb=611,448");
        @node;
        @labels(node,ethernet-node,wireless-node);
        int numExtInterfaces = default(0);
        int numRadios = default(0);               // the number of radios in the router. by default no wireless
        int numPcapRecorders = default(0); // no of PcapRecorders.
        string mobilityType = default("StaticMobility");
        string routingFile = default("");
        bool IPForward = default(true);
        wlan[*].mgmtType = default("Ieee80211MgmtSTA");
    gates:
        input radioIn[numRadios+1] @directIn;
        inout pppg[] @labels(PPPFrame-conn);
        inout ethg[] @labels(EtherFrame-conn);
    submodules:
        notificationBoard: NotificationBoard {
            parameters:
                @display("p=53,194");
        }

        // optional mobility module. Required only if wireless cards are present
        mobility: <mobilityType> like IMobility if numRadios>0 {
            parameters:
                @display("p=53,121");
        }

        // network layer
        networkLayer: NetworkLayer {
            parameters:
                @display("p=356,265;q=queue");
        }

        routingTable: RoutingTable {
            parameters:
                @display("p=53,287");
                IPForward = IPForward;
                routingFile = routingFile;
        }

        // linklayer
        interfaceTable: InterfaceTable {
            parameters:
                @display("p=53,386");
        }

        pcapRecorder[numPcapRecorders]: PcapRecorder {
            @display("p=183,233");
        }

        wlanMesh: Ieee80211NewNicMesh {
            parameters:
                @display("p=159,386;q=queue");
        }

        wlan[numRadios]: <default("Ieee80211Nic")> like IWirelessNic {
            parameters:
                @display("p=317,392;q=queue");
        }
        ethMesh: EthernetInterfaceMesh {
            parameters:
                @display("p=222,386,row,90;q=txQueue");
        }
        eth[sizeof(ethg)-1]: <default("EthernetInterface")> like IWiredNic {
            parameters:
                @display("p=384,392,row,90;q=txQueue");
        }
        ppp[sizeof(pppg)]: <default("PPPInterface")> like IWiredNic {
            parameters:
                @display("p=456,392,row,90;q=txQueue");
        }
        ext[numExtInterfaces]: <default("ExtInterface")> like IExternalNic {
            parameters:
                @display("p=547,386,row,90;q=txQueue;i=block/ifcard");
        }

    connections allowunconnected:
        // connections to network outside

        radioIn[1] --> wlanMesh.radioIn;
        wlanMesh.upperLayerOut --> networkLayer.ifIn++;
        wlanMesh.upperLayerIn <-- networkLayer.ifOut++;

        for i=0..sizeof(radioIn)-2 {
            radioIn[i+1] --> wlan[i].radioIn;
            wlan[i].upperLayerOut --> networkLayer.ifIn++;
            wlan[i].upperLayerIn <-- networkLayer.ifOut++;
        }


        ethg[0] <--> ethMesh.phys;
        ethMesh.wifiMeshIn <-- wlanMesh.toEthernet;
        ethMesh.wifiMeshOut --> wlanMesh.fromEthernet;
        ethMesh.netwOut --> networkLayer.ifIn++;
        ethMesh.netwIn <-- networkLayer.ifOut++;

        for i=0..sizeof(ethg)-2 {
            ethg[i+1] <--> eth[i].phys;
            eth[i].netwOut --> networkLayer.ifIn++;
            eth[i].netwIn <-- networkLayer.ifOut++;
        }

        for i=0..sizeof(pppg)-1 {
            pppg[i] <--> ppp[i].phys;
            ppp[i].netwOut --> networkLayer.ifIn++;
            ppp[i].netwIn <-- networkLayer.ifOut++;
        }

        for i=0..numExtInterfaces-1 {
            ext[i].netwOut --> networkLayer.ifIn++;
            ext[i].netwIn <-- networkLayer.ifOut++;
        }
}

