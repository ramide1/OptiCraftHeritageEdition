#pragma once

// ProtocolTranslator189IO -- game-side wiring for the translation core.
//
// Translator189Connection is owned per NetworkManager connection when the
// target protocol is 1.8.9 (see NetworkManager::setTranslationTarget):
//   * readTranslated() consumes 1.8.9 frames (length prefix, optional
//     zlib block) from the socket stream and returns decoded NATIVE
//     1.2.5 packets via the regular Packet::readPacket factory, so every
//     downstream handler, queue lane and dispatch rule is untouched.
//   * writeTranslated() serializes a native packet with Packet::writePacket
//     and emits the equivalent 1.8.9 frame(s), or drops it when the 1.8.9
//     side has no equivalent (login swallowed, quit, unmapped).
// pollStatus189() answers the server-list row for 1.8.9 targets (1.8
// status handshake instead of the 0xFE ping).

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "ProtocolTranslator189.h"

class Packet;

class Translator189Connection
{
public:
	Translator189Connection(const std::string &host, int port);

	translator189::Session189 &session() { return session_; }

	// Reads 1.8.9 frames until at least one native packet is decoded
	// (dropped frames are skipped inside). Returns nullptr on clean EOF;
	// throws on protocol/IO errors (the caller's onNetworkError owns them).
	std::unique_ptr<Packet> readOneTranslated(std::istream &is);
	// Emits the 1.8.9 equivalent of a native packet. Returns true when
	// bytes were written, false when the packet was intentionally dropped.
	bool writeTranslated(Packet *packet, std::ostream &os);

	// 1.8 status ping for the server list. Returns false when unreachable.
	static bool pollStatus(const std::string &host, int port,
	                       std::string &motdOut, int &onlineOut, int &maxOut,
	                       long long &lagMsOut);

private:
	bool appendDecoded(translator189::ClientboundOut &bout);
	std::unique_ptr<Packet> takePending();

	std::deque<std::unique_ptr<Packet>> pending_;

	std::string host_;
	int port_;
	translator189::Session189 session_;
};
