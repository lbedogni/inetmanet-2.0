/*
 * Copyright (C) 2003 Andras Varga; CTIE, Monash University, Australia
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


#ifndef MACADDRESS_H_
#define MACADDRESS_H_

#include <string>
#include <omnetpp.h>
#include "INETDefs.h"

#define MAC_ADDRESS_SIZE 6
#define MAC_ADDRESS_MASK 0xffffffffffffL
#define MAC_ADDRESS_MASK64 0xffffffffffffffffL

#define MAC_ADDRESS_BYTES 6
#define MAC_ADDRESS_BYTES64 8

class InterfaceToken;

/**
 * Stores an IEEE 802 MAC address (6 octets = 48 bits).
 */
class INET_API MACAddress
{
  private:
    uint64 address;   // 6*8=48 bit address, lowest 6 bytes are used, highest 2 bytes are always zero
    static unsigned int autoAddressCtr; // global counter for generateAutoAddress()
    bool macAddress64;

  public:
    /** The unspecified MAC address, 00:00:00:00:00:00 */
    static const MACAddress UNSPECIFIED_ADDRESS;

    /** The broadcast MAC address, ff:ff:ff:ff:ff:ff */
    static const MACAddress BROADCAST_ADDRESS;

    /** The special multicast PAUSE MAC address, 01:80:C2:00:00:01 */
    static const MACAddress MULTICAST_PAUSE_ADDRESS;

  static const MACAddress BROADCAST_ADDRESS64;

    /**
     * Default constructor initializes address bytes to zero.
     */
    MACAddress()
    {
        address = 0;
        macAddress64 = false;
    }

    /**
     * Initializes the address from a 48-bit integer
     */
    MACAddress(uint64 bits)
    {
        if (bits & !MAC_ADDRESS_MASK)
            macAddress64 = true;
        else
            macAddress64 = false;
        address = bits;
    }


    /**
     * Constructor which accepts a hex string (12 hex digits, may also
     * contain spaces, hyphens and colons)
     */
    MACAddress(const char *hexstr) { setAddress(hexstr); }

    /**
     * Copy constructor.
     */
    MACAddress(const MACAddress& other) { address = other.address; macAddress64 = other.macAddress64;}

    /**
     * Assignment.
     */
    MACAddress& operator=(const MACAddress& other) { address = other.address; macAddress64 = other.macAddress64; return *this; }

    /**
     * Returns the address size in bytes, that is, 6.
     */
    unsigned int getAddressSize() const { return (macAddress64?MAC_ADDRESS_BYTES64:MAC_ADDRESS_SIZE); }

    /**
     * Returns the kth byte of the address.
     */
    unsigned char getAddressByte(unsigned int k) const;

    /**
     * Sets the kth byte of the address.
     */
    void setAddressByte(unsigned int k, unsigned char addrbyte);

    /**
     * Sets the address and returns true if the syntax of the string
     * is correct. (See setAddress() for the syntax.)
     */
    bool tryParse(const char *hexstr);

    /**
     * Converts address value from hex string (12 hex digits, may also
     * contain spaces, hyphens and colons)
     */
    void setAddress(const char *hexstr);

    /**
     * Copies the address to the given pointer (array of 6 unsigned chars).
     */
    void getAddressBytes(unsigned char *addrbytes) const;
    void getAddressBytes(char *addrbytes) const { getAddressBytes((unsigned char *)addrbytes); }

    /**
     * Sets address bytes. The argument should point to an array of 6 unsigned chars.
     */
    void setAddressBytes(unsigned char *addrbytes);
    void setAddressBytes(char *addrbytes) { setAddressBytes((unsigned char *)addrbytes); }

    /**
     * Sets the address to the broadcast address (hex ff:ff:ff:ff:ff:ff).
     */
    void setBroadcast() {address = (macAddress64 ? MAC_ADDRESS_MASK64 : MAC_ADDRESS_MASK); }

    /**
     * Returns true if this is the broadcast address (hex ff:ff:ff:ff:ff:ff).
     */
    bool isBroadcast() const {return (macAddress64 ? address == MAC_ADDRESS_MASK64 : address == MAC_ADDRESS_MASK); }

    /**
     * Returns true if this is a multicast logical address (first byte's lsb is 1).
     */
    bool isMulticast() const  { return getAddressByte(0) & 0x01; };


    /**
     * Returns true if all address bytes are zero.
     */
    bool isUnspecified() const { return address == 0; }

    /**
     * Converts address to a hex string.
     */
    std::string str() const;

    /**
     * Converts address to 48 bits integer.
     */
    uint64 getInt() const { return address; }

    /**
     * Returns true if the two addresses are equal.
     */
    bool equals(const MACAddress& other) const { return address == other.address; }

    /**
     * Returns true if the two addresses are equal.
     */
    bool operator==(const MACAddress& other) const { return address == other.address; }

    /**
     * Returns true if the two addresses are not equal.
     */
    bool operator!=(const MACAddress& other) const { return address != other.address; }

    /**
     * Returns -1, 0 or 1 as result of comparison of 2 addresses.
     */
    int compareTo(const MACAddress& other) const;

    /**
     * Create interface identifier (IEEE EUI-64) which can be used by IPv6
     * stateless address autoconfiguration.
     */
    InterfaceToken formInterfaceIdentifier() const;

    /**
     * Generates a unique address which begins with 0a:aa and ends in a unique
     * suffix.
     */
    static MACAddress generateAutoAddress();

    bool operator<(const MACAddress& other) const { return address < other.address; }

    bool operator>(const MACAddress& other) const { return address > other.address; }

    // works with MACaddress (64 bits)
    // convert EUI-48 to EUI64
    void convert64();

    // get the address like EUI64, with independence if it is EUI64 or EUI48
    MACAddress getEui64();

    // try to convert EUI-64 to EUI-48 if us possible
    void convert48();

    // try to get the address like EUI-48, if is possible
    MACAddress getEui48();

    MACAddress getMaskEUI48() { return MACAddress(MAC_ADDRESS_MASK); }

    MACAddress getMaskEUI64() { return MACAddress(MAC_ADDRESS_MASK64); }

    bool getFlagEui64() const {return macAddress64; }
    void setFlagEui64(bool v)  {macAddress64 = v; }

    bool operator&(const MACAddress& other) const { return address & other.address; }
    bool operator|(const MACAddress& other) const { return address | other.address; }

};

inline std::ostream& operator<<(std::ostream& os, const MACAddress& mac)
{
    return os << mac.str();
}

#endif


