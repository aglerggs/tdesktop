/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/details/mtproto_abstract_socket.h"

#include <QtNetwork/QSslSocket>

namespace MTP::details {

// MTProto over WebSocket to the Telegram web endpoints, the same way
// the web clients connect (kwsN.web.telegram.org/apiws). Inside the
// WebSocket goes the usual obfuscated MTProto stream, unchanged.
// The idea and the endpoints are from Flowseal/tg-ws-proxy (MIT).
class WebSocketSocket final : public AbstractSocket {
public:
	WebSocketSocket(
		not_null<QThread*> thread,
		int16 protocolDcId,
		bool protocolForFiles);
	~WebSocketSocket();

	// Address to connect to for the given protocol dc id,
	// empty if there is no WebSocket endpoint for that dc.
	[[nodiscard]] static QString EndpointAddress(int16 protocolDcId);
	static constexpr auto kPort = 443;

	void connectToHost(const QString &address, int port) override;
	bool isGoodStartNonce(bytes::const_span nonce) override;
	void timedOut() override;
	bool isConnected() override;
	bool hasBytesAvailable() override;
	int64 read(bytes::span buffer) override;
	void write(bytes::const_span prefix, bytes::const_span buffer) override;

	int32 debugState() override;
	QString debugPostfix() const override;

private:
	enum class State {
		Idle,
		Connecting,
		WaitingUpgrade,
		Connected,
		Closed,
	};

	void startAttempt();
	void handleEncrypted(int attempt);
	void handleReadyRead(int attempt);
	void handleDisconnected(int attempt);
	void handleError(int attempt, int errorCode);
	void failAttempt();
	void sendUpgradeRequest();
	[[nodiscard]] bool readUpgradeResponse();
	[[nodiscard]] bool parseFrames();
	void writeFrame(bytes::const_span payload, uchar opcode);

	std::unique_ptr<QSslSocket> _socket;
	QStringList _domains;
	QString _path;
	QString _address;
	int _port = 0;
	int _domainIndex = 0;
	int _attempt = 0;
	bool _protocolForFiles = false;

	State _state = State::Idle;
	QByteArray _key;
	QByteArray _incoming;
	QByteArray _payload;

};

} // namespace MTP::details
