/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_websocket_socket.h"

#include "base/invoke_queued.h"
#include "base/random.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QtEndian>

namespace MTP::details {
namespace {

constexpr auto kOpcodeContinuation = uchar(0x0);
constexpr auto kOpcodeText = uchar(0x1);
constexpr auto kOpcodeBinary = uchar(0x2);
constexpr auto kOpcodeClose = uchar(0x8);
constexpr auto kOpcodePing = uchar(0x9);
constexpr auto kOpcodePong = uchar(0xA);

constexpr auto kMaxFramePayload = quint64(16 * 1024 * 1024);
constexpr auto kMaxUpgradeResponse = 16 * 1024;
constexpr auto kAcceptGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr auto kPath = "/apiws";
constexpr auto kTestModeShift = 10000;

struct ParsedDcId {
	int dcId = 0;
	bool media = false;
	bool test = false;
};

[[nodiscard]] ParsedDcId ParseProtocolDcId(int16 protocolDcId) {
	auto result = ParsedDcId();
	auto value = int(protocolDcId);
	if (value < 0) {
		result.media = true;
		value = -value;
	}
	if (value >= kTestModeShift) {
		result.test = true;
		value -= kTestModeShift;
	}
	result.dcId = value;
	return result;
}

[[nodiscard]] QStringList Domains(int16 protocolDcId) {
	const auto parsed = ParseProtocolDcId(protocolDcId);
	const auto dcId = (parsed.dcId == 203) ? 2 : parsed.dcId;
	const auto main = u"kws%1.web.telegram.org"_q.arg(dcId);
	if (!parsed.media) {
		return { main };
	}
	return { u"kws%1-1.web.telegram.org"_q.arg(dcId), main };
}

} // namespace

QString WebSocketSocket::EndpointAddress(int16 protocolDcId) {
	const auto parsed = ParseProtocolDcId(protocolDcId);
	if (parsed.test) {
		return QString();
	}
	// The web endpoints known to accept the WebSocket transport,
	// the same defaults as in tg-ws-proxy.
	switch (parsed.dcId) {
	case 2:
	case 4: return u"149.154.167.220"_q;
	}
	return QString();
}

WebSocketSocket::WebSocketSocket(
	not_null<QThread*> thread,
	int16 protocolDcId,
	bool protocolForFiles)
: AbstractSocket(thread)
, _domains(Domains(protocolDcId))
, _path(QString::fromLatin1(kPath))
, _protocolForFiles(protocolForFiles) {
}

WebSocketSocket::~WebSocketSocket() {
	if (_socket) {
		QObject::disconnect(_socket.get(), nullptr, this, nullptr);
		_socket->abort();
	}
}

void WebSocketSocket::connectToHost(const QString &address, int port) {
	_address = address;
	_port = port;
	_domainIndex = 0;
	startAttempt();
}

void WebSocketSocket::startAttempt() {
	const auto attempt = ++_attempt;
	if (_socket) {
		QObject::disconnect(_socket.get(), nullptr, this, nullptr);
		_socket->abort();
		_socket.release()->deleteLater();
	}
	_state = State::Connecting;
	_incoming.clear();
	_payload.clear();

	_socket = std::make_unique<QSslSocket>();
	_socket->moveToThread(QObject::thread());
	_socket->setProxy(QNetworkProxy::NoProxy);
	_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
	if (_protocolForFiles) {
		_socket->setSocketOption(
			QAbstractSocket::SendBufferSizeSocketOption,
			kFilesSendBufferSize);
		_socket->setSocketOption(
			QAbstractSocket::ReceiveBufferSizeSocketOption,
			kFilesReceiveBufferSize);
	}
	const auto socket = _socket.get();
	QObject::connect(socket, &QSslSocket::encrypted, this, [=] {
		InvokeQueued(this, [=] { handleEncrypted(attempt); });
	});
	QObject::connect(socket, &QSslSocket::readyRead, this, [=] {
		InvokeQueued(this, [=] { handleReadyRead(attempt); });
	});
	QObject::connect(socket, &QSslSocket::disconnected, this, [=] {
		InvokeQueued(this, [=] { handleDisconnected(attempt); });
	});
	QObject::connect(
		socket,
		&QAbstractSocket::errorOccurred,
		this,
		[=](QAbstractSocket::SocketError error) {
			InvokeQueued(this, [=] { handleError(attempt, int(error)); });
		});

	// Connect by the address, not by the name, the name goes to SNI.
	const auto domain = _domains.value(_domainIndex);
	socket->connectToHostEncrypted(_address, quint16(_port), domain);
}

void WebSocketSocket::failAttempt() {
	if (_state == State::Closed) {
		return;
	}
	if (++_domainIndex < _domains.size()) {
		startAttempt();
		return;
	}
	_state = State::Closed;
	_error.fire({});
}

void WebSocketSocket::handleEncrypted(int attempt) {
	if (attempt != _attempt || _state != State::Connecting) {
		return;
	}
	_state = State::WaitingUpgrade;
	sendUpgradeRequest();
}

void WebSocketSocket::sendUpgradeRequest() {
	auto keyBytes = std::array<uchar, 16>();
	base::RandomFill(keyBytes.data(), keyBytes.size());
	_key = QByteArray(
		reinterpret_cast<const char*>(keyBytes.data()),
		int(keyBytes.size())).toBase64();

	const auto request = u"GET "_q + _path + u" HTTP/1.1\r\n"_q
		+ u"Host: "_q + _domains.value(_domainIndex) + u"\r\n"_q
		+ u"Upgrade: websocket\r\n"_q
		+ u"Connection: Upgrade\r\n"_q
		+ u"Sec-WebSocket-Key: "_q + QString::fromLatin1(_key) + u"\r\n"_q
		+ u"Sec-WebSocket-Version: 13\r\n"_q
		+ u"Sec-WebSocket-Protocol: binary\r\n"_q
		+ u"\r\n"_q;
	_socket->write(request.toLatin1());
}

bool WebSocketSocket::readUpgradeResponse() {
	const auto end = _incoming.indexOf("\r\n\r\n");
	if (end < 0) {
		return (_incoming.size() < kMaxUpgradeResponse);
	}
	const auto head = _incoming.left(end);
	const auto lines = head.split('\n');
	const auto status = lines.isEmpty()
		? QByteArray()
		: lines.front().trimmed();
	if (!status.contains(" 101")) {
		DEBUG_LOG(("Socket %1 WebSocket upgrade refused: %2"
			).arg(_debugId
			).arg(QString::fromLatin1(status)));
		return false;
	}
	const auto expected = QCryptographicHash::hash(
		_key + kAcceptGuid,
		QCryptographicHash::Sha1).toBase64();
	auto accepted = false;
	for (const auto &line : lines) {
		const auto colon = line.indexOf(':');
		if (colon < 0
			|| (line.left(colon).trimmed().toLower()
				!= "sec-websocket-accept")) {
			continue;
		}
		accepted = (line.mid(colon + 1).trimmed() == expected);
		break;
	}
	if (!accepted) {
		DEBUG_LOG(("Socket %1 WebSocket bad accept key."
			).arg(_debugId));
		return false;
	}
	_incoming.remove(0, end + 4);
	_state = State::Connected;
	_connected.fire({});
	return true;
}

void WebSocketSocket::handleReadyRead(int attempt) {
	if (attempt != _attempt || !_socket) {
		return;
	}
	if (_state == State::WaitingUpgrade) {
		_incoming.append(_socket->readAll());
		if (!readUpgradeResponse()) {
			failAttempt();
			return;
		} else if (_state != State::Connected) {
			return;
		}
	} else if (_state == State::Connected) {
		_incoming.append(_socket->readAll());
	} else {
		return;
	}
	const auto had = _payload.size();
	if (!parseFrames()) {
		if (_state == State::Connected) {
			_state = State::Closed;
			_disconnected.fire({});
		}
		return;
	}
	if (_payload.size() > had) {
		_readyRead.fire({});
	}
}

bool WebSocketSocket::parseFrames() {
	auto offset = 0;
	const auto size = int(_incoming.size());
	const auto data = reinterpret_cast<const uchar*>(_incoming.constData());
	while (size - offset >= 2) {
		const auto first = data[offset];
		const auto second = data[offset + 1];
		const auto opcode = uchar(first & 0x0F);
		const auto masked = ((second & 0x80) != 0);
		auto length = quint64(second & 0x7F);
		auto header = 2;
		if (length == 126) {
			if (size - offset < 4) {
				break;
			}
			length = (quint64(data[offset + 2]) << 8)
				| quint64(data[offset + 3]);
			header = 4;
		} else if (length == 127) {
			if (size - offset < 10) {
				break;
			}
			length = 0;
			for (auto i = 0; i != 8; ++i) {
				length = (length << 8) | quint64(data[offset + 2 + i]);
			}
			header = 10;
		}
		if (length > kMaxFramePayload) {
			return false;
		}
		const auto maskOffset = offset + header;
		if (masked) {
			header += 4;
		}
		if (quint64(size - offset) < quint64(header) + length) {
			break;
		}
		const auto from = offset + header;
		const auto count = int(length);
		offset += header + count;

		switch (opcode) {
		case kOpcodeContinuation:
		case kOpcodeBinary:
		case kOpcodeText: {
			const auto start = int(_payload.size());
			_payload.append(
				reinterpret_cast<const char*>(data + from),
				count);
			if (masked) {
				const auto mask = data + maskOffset;
				auto out = _payload.data() + start;
				for (auto i = 0; i != count; ++i) {
					out[i] = char(uchar(out[i]) ^ mask[i % 4]);
				}
			}
		} break;
		case kOpcodePing: {
			auto pong = QByteArray(
				reinterpret_cast<const char*>(data + from),
				count);
			if (masked) {
				const auto mask = data + maskOffset;
				for (auto i = 0; i != count; ++i) {
					pong[i] = char(uchar(pong[i]) ^ mask[i % 4]);
				}
			}
			writeFrame(bytes::make_span(pong), kOpcodePong);
		} break;
		case kOpcodePong:
			break;
		case kOpcodeClose:
			DEBUG_LOG(("Socket %1 WebSocket closed by the server."
				).arg(_debugId));
			_incoming.clear();
			return false;
		default:
			return false;
		}
	}
	if (offset > 0) {
		_incoming.remove(0, offset);
	}
	return true;
}

void WebSocketSocket::writeFrame(bytes::const_span payload, uchar opcode) {
	if (!_socket) {
		return;
	}
	const auto length = quint64(payload.size());
	auto frame = QByteArray();
	frame.reserve(int(length) + 14);
	frame.append(char(0x80 | opcode));
	if (length < 126) {
		frame.append(char(0x80 | int(length)));
	} else if (length <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		frame.append(char((length >> 8) & 0xFF));
		frame.append(char(length & 0xFF));
	} else {
		frame.append(char(0x80 | 127));
		for (auto i = 7; i >= 0; --i) {
			frame.append(char((length >> (i * 8)) & 0xFF));
		}
	}
	auto mask = std::array<uchar, 4>();
	base::RandomFill(mask.data(), mask.size());
	frame.append(reinterpret_cast<const char*>(mask.data()), 4);

	const auto start = int(frame.size());
	frame.resize(start + int(length));
	const auto from = reinterpret_cast<const uchar*>(payload.data());
	auto out = frame.data() + start;
	for (auto i = 0; i != int(length); ++i) {
		out[i] = char(from[i] ^ mask[i % 4]);
	}
	_socket->write(frame);
}

void WebSocketSocket::handleDisconnected(int attempt) {
	if (attempt != _attempt) {
		return;
	}
	if (_state == State::Connected) {
		_state = State::Closed;
		_disconnected.fire({});
	} else {
		failAttempt();
	}
}

void WebSocketSocket::handleError(int attempt, int errorCode) {
	if (attempt != _attempt || !_socket) {
		return;
	}
	logError(errorCode, _socket->errorString());
	if (_state == State::Connected) {
		_state = State::Closed;
		_error.fire({});
	} else {
		failAttempt();
	}
}

bool WebSocketSocket::isGoodStartNonce(bytes::const_span nonce) {
	Expects(nonce.size() >= 2 * sizeof(uint32));

	// Same restrictions as for the plain TCP obfuscated transport.
	const auto bytes = nonce.data();
	const auto zero = gsl::to_integer<uchar>(nonce.front());
	const auto first = qFromUnaligned<uint32>(bytes);
	const auto second = qFromUnaligned<uint32>(bytes + sizeof(uint32));
	const auto reserved01 = 0x000000EFU;
	const auto reserved11 = 0x44414548U;
	const auto reserved12 = 0x54534F50U;
	const auto reserved13 = 0x20544547U;
	const auto reserved14 = 0xEEEEEEEEU;
	const auto reserved15 = 0xDDDDDDDDU;
	const auto reserved16 = 0x02010316U;
	const auto reserved21 = 0x00000000U;
	return (zero != reserved01)
		&& (first != reserved11)
		&& (first != reserved12)
		&& (first != reserved13)
		&& (first != reserved14)
		&& (first != reserved15)
		&& (first != reserved16)
		&& (second != reserved21);
}

void WebSocketSocket::timedOut() {
}

bool WebSocketSocket::isConnected() {
	return (_state == State::Connected)
		&& _socket
		&& (_socket->state() == QAbstractSocket::ConnectedState);
}

bool WebSocketSocket::hasBytesAvailable() {
	return !_payload.isEmpty();
}

int64 WebSocketSocket::read(bytes::span buffer) {
	const auto count = std::min(int64(buffer.size()), int64(_payload.size()));
	if (count <= 0) {
		return 0;
	}
	bytes::copy(
		buffer,
		bytes::make_span(_payload).subspan(0, count));
	_payload.remove(0, int(count));
	return count;
}

void WebSocketSocket::write(
		bytes::const_span prefix,
		bytes::const_span buffer) {
	Expects(!buffer.empty());

	if (_state != State::Connected) {
		return;
	}
	// The connection start and every packet go in separate frames.
	if (!prefix.empty()) {
		writeFrame(prefix, kOpcodeBinary);
	}
	writeFrame(buffer, kOpcodeBinary);
}

int32 WebSocketSocket::debugState() {
	return _socket ? int32(_socket->state()) : -1;
}

QString WebSocketSocket::debugPostfix() const {
	return u"_ws"_q;
}

} // namespace MTP::details
