#ifndef ALCHEMY_QLAB_COREMIDI_H
#define ALCHEMY_QLAB_COREMIDI_H
/* CoreMIDI's packet-list types as the Swift ClangImporter sees them, for
   Alchemy's project-authored CoreMIDI overlay (graphics/swift/CoreMIDI).
   Independently authored from the published CoreMIDI interface; no SDK
   content. Packets and packet lists are packed to 4-byte alignment, and on
   arm64 each following packet starts at the next 4-byte boundary after the
   previous packet's data (MIDIPacketNext). */
#include <stdint.h>

typedef uint64_t MIDITimeStamp;

#pragma pack(push, 4)
/* A collection of simultaneous MIDI events (length bytes of data). */
typedef struct MIDIPacket {
    MIDITimeStamp timeStamp;
    uint16_t length;
    uint8_t data[256];
} MIDIPacket;

/* A variable-length list of MIDI packets (numPackets entries). */
typedef struct MIDIPacketList {
    uint32_t numPackets;
    MIDIPacket packet[1];
} MIDIPacketList;
#pragma pack(pop)

#endif
