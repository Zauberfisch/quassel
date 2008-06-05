/***************************************************************************
 *   Copyright (C) 2005-08 by the Quassel Project                          *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "networkconnection.h"

#include <QMetaObject>
#include <QMetaMethod>
#include <QDateTime>

#include "util.h"
#include "core.h"
#include "coresession.h"

#include "ircchannel.h"
#include "ircuser.h"
#include "network.h"
#include "identity.h"

#include "ircserverhandler.h"
#include "userinputhandler.h"
#include "ctcphandler.h"

NetworkConnection::NetworkConnection(Network *network, CoreSession *session)
  : QObject(network),
    _connectionState(Network::Disconnected),
    _network(network),
    _coreSession(session),
    _ircServerHandler(new IrcServerHandler(this)),
    _userInputHandler(new UserInputHandler(this)),
    _ctcpHandler(new CtcpHandler(this)),
    _autoReconnectCount(0),
    _quitRequested(false),

    _previousConnectionAttemptFailed(false),
    _lastUsedServerlistIndex(0),

    // TODO make autowho configurable (possibly per-network)
    _autoWhoEnabled(true),
    _autoWhoInterval(90),
    _autoWhoNickLimit(0), // unlimited
    _autoWhoDelay(3),

    // TokenBucket to avaid sending too much at once
    _messagesPerSecond(1),
    _burstSize(5),
    _tokenBucket(5) // init with a full bucket
{
  _autoReconnectTimer.setSingleShot(true);
  _socketCloseTimer.setSingleShot(true);
  connect(&_socketCloseTimer, SIGNAL(timeout()), this, SLOT(socketCloseTimeout()));
  
  _autoWhoTimer.setInterval(_autoWhoDelay * 1000);
  _autoWhoCycleTimer.setInterval(_autoWhoInterval * 1000);
  
  _tokenBucketTimer.start(_messagesPerSecond * 1000);

  QHash<QString, QString> channels = coreSession()->persistentChannels(networkId());
  foreach(QString chan, channels.keys()) {
    _channelKeys[chan.toLower()] = channels[chan];
  }

  connect(&_autoReconnectTimer, SIGNAL(timeout()), this, SLOT(doAutoReconnect()));
  connect(&_autoWhoTimer, SIGNAL(timeout()), this, SLOT(sendAutoWho()));
  connect(&_autoWhoCycleTimer, SIGNAL(timeout()), this, SLOT(startAutoWhoCycle()));
  connect(&_tokenBucketTimer, SIGNAL(timeout()), this, SLOT(fillBucketAndProcessQueue()));

  connect(network, SIGNAL(currentServerSet(const QString &)), this, SLOT(networkInitialized(const QString &)));
  connect(network, SIGNAL(useAutoReconnectSet(bool)), this, SLOT(autoReconnectSettingsChanged()));
  connect(network, SIGNAL(autoReconnectIntervalSet(quint32)), this, SLOT(autoReconnectSettingsChanged()));
  connect(network, SIGNAL(autoReconnectRetriesSet(quint16)), this, SLOT(autoReconnectSettingsChanged()));

#ifndef QT_NO_OPENSSL
  connect(&socket, SIGNAL(encrypted()), this, SLOT(socketEncrypted()));
  connect(&socket, SIGNAL(sslErrors(const QList<QSslError> &)), this, SLOT(sslErrors(const QList<QSslError> &)));
#endif
  connect(&socket, SIGNAL(connected()), this, SLOT(socketConnected()));

  connect(&socket, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));
  connect(&socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(socketError(QAbstractSocket::SocketError)));
  connect(&socket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SLOT(socketStateChanged(QAbstractSocket::SocketState)));
  connect(&socket, SIGNAL(readyRead()), this, SLOT(socketHasData()));

  connect(_ircServerHandler, SIGNAL(nickChanged(const QString &, const QString &)),
	  this, SLOT(nickChanged(const QString &, const QString &)));

  network->proxy()->attachSignal(this, SIGNAL(sslErrors(const QVariant &)));
}

NetworkConnection::~NetworkConnection() {
  if(connectionState() != Network::Disconnected && connectionState() != Network::Reconnecting)
    disconnectFromIrc(false); // clean up, but this does not count as requested disconnect!
  disconnect(&socket, 0, this, 0); // this keeps the socket from triggering events during clean up
  delete _ircServerHandler;
  delete _userInputHandler;
  delete _ctcpHandler;
}

void NetworkConnection::setConnectionState(Network::ConnectionState state) {
  _connectionState = state;
  network()->setConnectionState(state);
  emit connectionStateChanged(state);
}

QString NetworkConnection::serverDecode(const QByteArray &string) const {
  return network()->decodeServerString(string);
}

QString NetworkConnection::channelDecode(const QString &bufferName, const QByteArray &string) const {
  if(!bufferName.isEmpty()) {
    IrcChannel *channel = network()->ircChannel(bufferName);
    if(channel) return channel->decodeString(string);
  }
  return network()->decodeString(string);
}

QString NetworkConnection::userDecode(const QString &userNick, const QByteArray &string) const {
  IrcUser *user = network()->ircUser(userNick);
  if(user) return user->decodeString(string);
  return network()->decodeString(string);
}

QByteArray NetworkConnection::serverEncode(const QString &string) const {
  return network()->encodeServerString(string);
}

QByteArray NetworkConnection::channelEncode(const QString &bufferName, const QString &string) const {
  if(!bufferName.isEmpty()) {
    IrcChannel *channel = network()->ircChannel(bufferName);
    if(channel) return channel->encodeString(string);
  }
  return network()->encodeString(string);
}

QByteArray NetworkConnection::userEncode(const QString &userNick, const QString &string) const {
  IrcUser *user = network()->ircUser(userNick);
  if(user) return user->encodeString(string);
  return network()->encodeString(string);
}

void NetworkConnection::autoReconnectSettingsChanged() {
  if(!network()->useAutoReconnect()) {
    _autoReconnectTimer.stop();
    _autoReconnectCount = 0;
  } else {
    _autoReconnectTimer.setInterval(network()->autoReconnectInterval() * 1000);
    if(_autoReconnectCount != 0) {
      if(network()->unlimitedReconnectRetries()) _autoReconnectCount = -1;
      else _autoReconnectCount = network()->autoReconnectRetries();
    }
  }
}

void NetworkConnection::connectToIrc(bool reconnecting) {
  if(!reconnecting && network()->useAutoReconnect() && _autoReconnectCount == 0) {
    _autoReconnectTimer.setInterval(network()->autoReconnectInterval() * 1000);
    if(network()->unlimitedReconnectRetries()) _autoReconnectCount = -1;
    else _autoReconnectCount = network()->autoReconnectRetries();
  }
  QVariantList serverList = network()->serverList();
  Identity *identity = coreSession()->identity(network()->identity());
  if(!serverList.count()) {
    qWarning() << "Server list empty, ignoring connect request!";
    return;
  }
  if(!identity) {
    qWarning() << "Invalid identity configures, ignoring connect request!";
    return;
  }
  // use a random server?
  if(network()->useRandomServer()) {
    _lastUsedServerlistIndex = qrand() % serverList.size();
  } else if(_previousConnectionAttemptFailed) {
    // cycle to next server if previous connection attempt failed
    displayMsg(Message::Server, BufferInfo::StatusBuffer, "", tr("Connection failed. Cycling to next Server"));
    if(++_lastUsedServerlistIndex == serverList.size()) {
      _lastUsedServerlistIndex = 0;
    }
  }
  _previousConnectionAttemptFailed = false;

  QString host = serverList[_lastUsedServerlistIndex].toMap()["Host"].toString();
  quint16 port = serverList[_lastUsedServerlistIndex].toMap()["Port"].toUInt();
  displayStatusMsg(tr("Connecting to %1:%2...").arg(host).arg(port));
  displayMsg(Message::Server, BufferInfo::StatusBuffer, "", tr("Connecting to %1:%2...").arg(host).arg(port));
  socket.connectToHost(host, port);
}

void NetworkConnection::networkInitialized(const QString &currentServer) {
  if(currentServer.isEmpty()) return;

  if(network()->useAutoReconnect() && !network()->unlimitedReconnectRetries()) {
    _autoReconnectCount = network()->autoReconnectRetries(); // reset counter
  }

  sendPerform();

  // now we are initialized
  setConnectionState(Network::Initialized);
  network()->setConnected(true);
  emit connected(networkId());

  if(_autoWhoEnabled) {
    _autoWhoCycleTimer.start();
    _autoWhoTimer.start();
    startAutoWhoCycle();  // FIXME wait for autojoin to be completed
  }
}

void NetworkConnection::sendPerform() {
  BufferInfo statusBuf = Core::bufferInfo(coreSession()->user(), network()->networkId(), BufferInfo::StatusBuffer);
  // do auto identify
  if(network()->useAutoIdentify() && !network()->autoIdentifyService().isEmpty() && !network()->autoIdentifyPassword().isEmpty()) {
    userInputHandler()->handleMsg(statusBuf, QString("%1 IDENTIFY %2").arg(network()->autoIdentifyService(), network()->autoIdentifyPassword()));
  }
  // send perform list
  foreach(QString line, network()->perform()) {
    if(!line.isEmpty()) userInput(statusBuf, line);
  }

  // rejoin channels we've been in
  QStringList channels, keys;
  foreach(QString chan, persistentChannels()) {
    QString key = channelKey(chan);
    if(!key.isEmpty()) {
      channels.prepend(chan); keys.prepend(key);
    } else {
      channels.append(chan);
    }
  }
  QString joinString = QString("%1 %2").arg(channels.join(",")).arg(keys.join(",")).trimmed();
  if(!joinString.isEmpty()) userInputHandler()->handleJoin(statusBuf, joinString);
}

void NetworkConnection::disconnectFromIrc(bool requested) {
  _autoReconnectTimer.stop();
  _autoReconnectCount = 0;
  displayMsg(Message::Server, BufferInfo::StatusBuffer, "", tr("Disconnecting."));
  if(socket.state() < QAbstractSocket::ConnectedState) {
    setConnectionState(Network::Disconnected);
    socketDisconnected();
  } else {
    _socketCloseTimer.start(10000); // the irc server has 10 seconds to close the socket
  }

  // this flag triggers quitRequested() once the socket is closed
  // it is needed to determine whether or not the connection needs to be
  // in the automatic session restore.
  _quitRequested = requested;
}

void NetworkConnection::socketHasData() {
  while(socket.canReadLine()) {
    QByteArray s = socket.readLine().trimmed();
    ircServerHandler()->handleServerMsg(s);
  }
}

void NetworkConnection::socketError(QAbstractSocket::SocketError) {
  _previousConnectionAttemptFailed = true;
  qDebug() << qPrintable(tr("Could not connect to %1 (%2)").arg(network()->networkName(), socket.errorString()));
  emit connectionError(socket.errorString());
  emit displayMsg(Message::Error, BufferInfo::StatusBuffer, "", tr("Connection failure: %1").arg(socket.errorString()));
  network()->emitConnectionError(socket.errorString());
  if(socket.state() < QAbstractSocket::ConnectedState) {
    setConnectionState(Network::Disconnected);
    socketDisconnected();
  }
  // mark last connection attempt as failed
  
  //qDebug() << "exiting...";
  //exit(1);
}

#ifndef QT_NO_OPENSSL

void NetworkConnection::sslErrors(const QList<QSslError> &sslErrors) {
  Q_UNUSED(sslErrors)
  socket.ignoreSslErrors();
  /* TODO errorhandling
  QVariantMap errmsg;
  QVariantList errnums;
  foreach(QSslError err, errors) errnums << err.error();
  errmsg["SslErrors"] = errnums;
  errmsg["SslCert"] = socket.peerCertificate().toPem();
  errmsg["PeerAddress"] = socket.peerAddress().toString();
  errmsg["PeerPort"] = socket.peerPort();
  errmsg["PeerName"] = socket.peerName();
  emit sslErrors(errmsg);
  disconnectFromIrc();
  */
}

void NetworkConnection::socketEncrypted() {
  //qDebug() << "encrypted!";
  socketInitialized();
}

#endif  // QT_NO_OPENSSL

void NetworkConnection::socketConnected() {
#ifdef QT_NO_OPENSSL
  socketInitialized();
  return;
#else
  if(!network()->serverList()[_lastUsedServerlistIndex].toMap()["UseSSL"].toBool()) {
    socketInitialized();
    return;
  }
  //qDebug() << "starting handshake";
  socket.startClientEncryption();
#endif
}

void NetworkConnection::socketInitialized() {
  //emit connected(networkId());  initialize first!
  Identity *identity = coreSession()->identity(network()->identity());
  if(!identity) {
    qWarning() << "Identity invalid!";
    disconnectFromIrc();
    return;
  }
  QString passwd = network()->serverList()[_lastUsedServerlistIndex].toMap()["Password"].toString();
  if(!passwd.isEmpty()) {
    putRawLine(serverEncode(QString("PASS %1").arg(passwd)));
  }
  putRawLine(serverEncode(QString("NICK :%1").arg(identity->nicks()[0])));  // FIXME: try more nicks if error occurs
  putRawLine(serverEncode(QString("USER %1 8 * :%2").arg(identity->ident(), identity->realName())));
}

void NetworkConnection::socketStateChanged(QAbstractSocket::SocketState socketState) {
  Network::ConnectionState state;
  switch(socketState) {
    case QAbstractSocket::UnconnectedState:
      state = Network::Disconnected;
      break;
    case QAbstractSocket::HostLookupState:
    case QAbstractSocket::ConnectingState:
      state = Network::Connecting;
      break;
    case QAbstractSocket::ConnectedState:
      state = Network::Initializing;
      break;
    case QAbstractSocket::ClosingState:
      state = Network::Disconnecting;
      break;
    default:
      state = Network::Disconnected;
  }
  setConnectionState(state);
}

void NetworkConnection::socketCloseTimeout() {
  socket.disconnectFromHost();
}

void NetworkConnection::socketDisconnected() {
  _autoWhoCycleTimer.stop();
  _autoWhoTimer.stop();
  _autoWhoQueue.clear();
  _autoWhoInProgress.clear();

  _socketCloseTimer.stop();
  
  network()->setConnected(false);
  emit disconnected(networkId());
  if(_autoReconnectCount != 0) {
    setConnectionState(Network::Reconnecting);
    if(_autoReconnectCount == network()->autoReconnectRetries()) doAutoReconnect(); // first try is immediate
    else _autoReconnectTimer.start();
  } else if(_quitRequested) {
    emit quitRequested(networkId());
  }
}

void NetworkConnection::doAutoReconnect() {
  if(connectionState() != Network::Disconnected && connectionState() != Network::Reconnecting) {
    qWarning() << "NetworkConnection::doAutoReconnect(): Cannot reconnect while not being disconnected!";
    return;
  }
  if(_autoReconnectCount > 0) _autoReconnectCount--;
  connectToIrc(true);
}

// FIXME switch to BufferId
void NetworkConnection::userInput(BufferInfo buf, QString msg) {
  userInputHandler()->handleUserInput(buf, msg);
}

void NetworkConnection::putRawLine(QByteArray s) {
  if(_tokenBucket > 0) {
    writeToSocket(s);
  } else {
    _msgQueue.append(s);
  }
}

void NetworkConnection::writeToSocket(QByteArray s) {
  s += "\r\n";
  socket.write(s);
  _tokenBucket--;
}

void NetworkConnection::fillBucketAndProcessQueue() {
  if(_tokenBucket < _burstSize) {
    _tokenBucket++;
  }

  while(_msgQueue.size() > 0 && _tokenBucket > 0) {
    writeToSocket(_msgQueue.takeFirst());
  }
}

void NetworkConnection::putCmd(const QString &cmd, const QVariantList &params, const QByteArray &prefix) {
  QByteArray msg;
  if(!prefix.isEmpty())
    msg += ":" + prefix + " ";
  msg += cmd.toUpper().toAscii();

  for(int i = 0; i < params.size() - 1; i++) {
    msg += " " + params[i].toByteArray();
  }
  if(!params.isEmpty())
    msg += " :" + params.last().toByteArray();

  putRawLine(msg);
}

void NetworkConnection::sendAutoWho() {
  while(!_autoWhoQueue.isEmpty()) {
    QString chan = _autoWhoQueue.takeFirst();
    IrcChannel *ircchan = network()->ircChannel(chan);
    if(!ircchan) continue;
    if(_autoWhoNickLimit > 0 && ircchan->ircUsers().count() > _autoWhoNickLimit) continue;
    _autoWhoInProgress[chan]++;
    putRawLine("WHO " + serverEncode(chan));
    if(_autoWhoQueue.isEmpty() && _autoWhoEnabled && !_autoWhoCycleTimer.isActive()) {
      // Timer was stopped, means a new cycle is due immediately
      _autoWhoCycleTimer.start();
      startAutoWhoCycle();
    }
    break;
  }
}

void NetworkConnection::startAutoWhoCycle() {
  if(!_autoWhoQueue.isEmpty()) {
    _autoWhoCycleTimer.stop();
    return;
  }
  _autoWhoQueue = network()->channels();
}

bool NetworkConnection::setAutoWhoDone(const QString &channel) {
  if(_autoWhoInProgress.value(channel.toLower(), 0) <= 0) return false;
  _autoWhoInProgress[channel.toLower()]--;
  return true;
}

void NetworkConnection::setChannelJoined(const QString &channel) {
  emit channelJoined(networkId(), channel, _channelKeys[channel.toLower()]);
  _autoWhoQueue.prepend(channel.toLower()); // prepend so this new chan is the first to be checked
}

void NetworkConnection::setChannelParted(const QString &channel) {
  removeChannelKey(channel);
  _autoWhoQueue.removeAll(channel.toLower());
  _autoWhoInProgress.remove(channel.toLower());
  emit channelParted(networkId(), channel);
}

void NetworkConnection::addChannelKey(const QString &channel, const QString &key) {
  if(key.isEmpty()) {
    removeChannelKey(channel);
  } else {
    _channelKeys[channel.toLower()] = key;
  }
}

void NetworkConnection::removeChannelKey(const QString &channel) {
  _channelKeys.remove(channel.toLower());
}

void NetworkConnection::nickChanged(const QString &newNick, const QString &oldNick) {
  emit nickChanged(networkId(), newNick, oldNick);
}

/* Exception classes for message handling */
NetworkConnection::ParseError::ParseError(QString cmd, QString prefix, QStringList params) {
  Q_UNUSED(prefix);
  _msg = QString("Command Parse Error: ") + cmd + params.join(" ");
}

NetworkConnection::UnknownCmdError::UnknownCmdError(QString cmd, QString prefix, QStringList params) {
  Q_UNUSED(prefix);
  _msg = QString("Unknown Command: ") + cmd + params.join(" ");
}

