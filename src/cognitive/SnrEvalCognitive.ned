//***************************************************************************
// * file:        SnrEval80211.ned
// *
// * author:      Marc Loebbers
// *
// * copyright:   (C) 2004 Telecommunication Networks Group (TKN) at
// *              Technische Universitaet Berlin, Germany.
// *
// *              This program is free software; you can redistribute it
// *              and/or modify it under the terms of the GNU General Public
// *              License as published by the Free Software Foundation; either
// *              version 2 of the License, or (at your option) any later
// *              version.
// *              For further information see file COPYING
// *              in the top level directory
// ***************************************************************************
// * part of:     framework implementation developed by tkn
// **************************************************************************/


package inet.cognitive;

//
// This component has been taken over from Mobility Framework 1.0a5.
//
simple SnrEvalCognitive
{
    parameters:
        int channelNumber; // channel identifier
        bool debug = default(false); // debug switch
        double transmitterPower @unit("mW");
        double bitrate @unit("bps");
        int headerLength @unit(b);
        double carrierFrequency @unit("Hz");
        double thermalNoise @unit("dBm");
        double pathLossAlpha;
        double sensitivity @unit(dBm);
        @display("i=block/wrxtx");
    gates:
        input upperLayerIn @labels(Mac80211Pkt);
        output upperLayerOut;
        input radioIn @labels(AirFrame); // to receive AirFrames
}
