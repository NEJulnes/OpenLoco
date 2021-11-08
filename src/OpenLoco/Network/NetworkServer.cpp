#include "NetworkServer.h"
#include "../Console.h"
#include "../Core/Span.hpp"
#include "../OpenLoco.h"
#include "../Platform/Platform.h"
#include "../S5/S5.h"
#include "../Utility/String.hpp"
#include "NetworkConnection.h"
#include <sstream>

using namespace OpenLoco::Network;

void NetworkServer::listen(port_t port)
{
    _socket->Listen(defaultPort);
    beginReceivePacketLoop();

    setScreenFlag(ScreenFlags::networked);
    setScreenFlag(ScreenFlags::networkHost);

    Console::log("Server opened");
    Console::log("Listening for incoming connections...");
}

void NetworkServer::onClose()
{
    clearScreenFlag(ScreenFlags::networked);
    clearScreenFlag(ScreenFlags::networkHost);
}

Client* NetworkServer::findClient(const INetworkEndpoint& endpoint)
{
    for (auto& client : _clients)
    {
        if (client->connection->getEndpoint().equals(endpoint))
        {
            return client.get();
        }
    }
    return nullptr;
}

void NetworkServer::createNewClient(std::unique_ptr<NetworkConnection> conn, const ConnectPacket& packet)
{
    auto newClient = std::make_unique<Client>();
    newClient->id = _nextClientId++;
    newClient->connection = std::move(conn);
    newClient->name = Utility::nullTerminatedView(packet.name);
    _clients.push_back(std::move(newClient));

    auto& newClientPtr = *_clients.back();

    ConnectResponsePacket response;
    response.result = ConnectionResult::success;
    newClientPtr.connection->sendPacket(response);

    Console::log("Accepted new client: %s", newClientPtr.name.c_str());
}

void NetworkServer::onReceivePacket(std::unique_ptr<INetworkEndpoint> endpoint, const Packet& packet)
{
    auto client = findClient(*endpoint);
    if (client == nullptr)
    {
        auto connectPacket = packet.As<PacketKind::connect, ConnectPacket>();
        if (connectPacket != nullptr)
        {
            auto conn = std::make_unique<NetworkConnection>(_socket.get(), std::move(endpoint));
            conn->receivePacket(packet);

            std::unique_lock<std::mutex> lk(_incomingConnectionsSync);
            _incomingConnections.push_back(std::move(conn));
        }
    }
    else
    {
        client->connection->receivePacket(packet);
    }
}

void NetworkServer::onReceivePacketFromClient(Client& client, const Packet& packet)
{
    switch (packet.header.kind)
    {
        case PacketKind::requestState:
            onReceiveStateRequestPacket(client, *packet.Cast<RequestStatePacket>());
            break;
    }
}

void NetworkServer::onReceiveStateRequestPacket(Client& client, const RequestStatePacket& request)
{
    constexpr uint16_t chunkSize = 4000;

    std::stringstream ss;
    S5::save(ss, S5::SaveFlags::noWindowClose);
    auto final = ss.str();
    auto saveData = stdx::span(reinterpret_cast<const uint8_t*>(final.data()), final.size());

    RequestStateResponse response;
    response.cookie = request.cookie;
    response.totalSize = saveData.size();
    response.numChunks = static_cast<uint16_t>((saveData.size() + (chunkSize - 1)) / chunkSize);
    client.connection->sendPacket(response);

    uint32_t offset = 0;
    uint32_t remaining = response.totalSize;
    uint16_t index = 0;
    while (index < response.numChunks)
    {
        RequestStateResponseChunk chunk;
        chunk.cookie = request.cookie;
        chunk.index = index;
        chunk.offset = offset;
        chunk.size = std::min<uint32_t>(chunkSize, remaining - offset);

        client.connection->sendPacket(chunk);

        offset += chunk.size;
        index++;
    }
}

void NetworkServer::removedTimedOutClients()
{
    for (auto it = _clients.begin(); it != _clients.end();)
    {
        auto& client = *it;
        if (client->connection->hasTimedOut())
        {
            Console::log("Client timed out: %s", client->name.c_str());
            it = _clients.erase(it);
        }
        else
        {
            it++;
        }
    }

    _clients.erase(
        std::remove_if(_clients.begin(), _clients.end(), [](const std::unique_ptr<Client>& client) { return client->connection->hasTimedOut(); }), _clients.end());
}

void NetworkServer::sendPings()
{
    auto now = Platform::getTime();
    if (now - _lastPing > 2500)
    {
        _lastPing = now;

        PingPacket packet;
        for (auto& client : _clients)
        {
            client->connection->sendPacket(packet);
        }
    }
}

void NetworkServer::processIncomingConnections()
{
    std::unique_lock<std::mutex> lk(_incomingConnectionsSync);
    for (auto& conn : _incomingConnections)
    {
        // The connect packet should be the first one
        while (auto packet = conn->takeNextPacket())
        {
            if (auto connectPacket = packet->As<PacketKind::connect, ConnectPacket>())
            {
                createNewClient(std::move(conn), *connectPacket);
                break;
            }
        }
    }

    _incomingConnections.clear();
}

void NetworkServer::processPackets()
{
    for (auto& client : _clients)
    {
        while (auto packet = client->connection->takeNextPacket())
        {
            onReceivePacketFromClient(*client, *packet);
        }
    }
}

void NetworkServer::onUpdate()
{
    processIncomingConnections();
    processPackets();
    updateClients();
    sendPings();
    removedTimedOutClients();
}

void NetworkServer::updateClients()
{
    for (auto& client : _clients)
    {
        client->connection->update();
    }
}
