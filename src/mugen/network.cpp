#ifdef HAVE_NETWORKING

/* Server and client communicate to each other over a single TCP connection. Both send their inputs as soon
 * as possible to the other side. Occasionally the server will send a full world state. Once a second or so
 * the server will send a ping packet.
 *
 * Both client and server will run the game independant of the other. When any side receives an input
 * then that side will replay the game state from the time the input happened, or sometime soon before it.
 * In this way both the server and client should end up with the same game state.
 *
 * Due to subtle problems in floating point calculations it becomes necessary to update the client state
 * with the full world state.
 */

#include "network.h"
#include "behavior.h"
#include "util/system.h"
#include "world.h"
#include "character.h"
#include "game.h"
#include "config.h"
#include "command.h"

#include "util/network/network.h"
#include "util/thread.h"
#include "util/token.h"
#include "util/lz4/lz4.h"

#include <string>
#include <vector>

using std::string;
using std::vector;

/* FIXME: move this to token.h */
static Token * filterTokens(Token * start){
    if (start->isData()){
        return start->copy();
    }

    Token * out = new Token(start->getName());
    for (vector<Token*>::const_iterator it = start->getTokens()->begin(); it != start->getTokens()->end(); it++){
        Token * use = filterTokens(*it);
        if (use->isData() || use->numTokens() > 0){
            *out << use;
        }
    }
    return out;
}

namespace PaintownUtil = ::Util;

namespace Mugen{

NetworkObserver::NetworkObserver():
StageObserver(){
}
    
NetworkObserver::~NetworkObserver(){
}

/* FIXME: split this into InputNetworkBuffer and OutputNetworkBuffer */
class NetworkBuffer{
public:
    NetworkBuffer(int size = 128){
        length = 0;
        actualLength = size;
        buffer = new char[actualLength];
        contains = 0;
    }

    ~NetworkBuffer(){
        delete[] buffer;
    }

    NetworkBuffer & operator<<(int16_t data){
        checkBuffer(sizeof(data));
        Network::dump16(buffer + length, data);
        length += sizeof(data);
        return *this;
    }

    NetworkBuffer & operator<<(uint32_t data){
        checkBuffer(sizeof(data));
        Network::dump32(buffer + length, data);
        length += sizeof(data);
        return *this;
    }

    NetworkBuffer & operator>>(int16_t & data){
        if ((int)(length + sizeof(data)) < contains){
            char * where = buffer + length;
            char * out = Network::parse16(where, (uint16_t*) &data);
            length += out - where;
        } else {
            data = -1;
        }
        return *this;
    }

    NetworkBuffer & operator>>(uint32_t & data){
        if ((int)(length + sizeof(data)) < contains){
            char * where = buffer + length;
            char * out = Network::parse32(where, &data);
            length += out - where;
        } else {
            data = -1;
        }
        return *this;
    }

    NetworkBuffer & operator<<(const string & str){
        *this << (int16_t) str.size();
        add(str.c_str(), str.size());
        return *this;
    }

    NetworkBuffer & operator>>(string & str){
        int16_t size = 0;
        *this >> size;
        /* FIXME: if we read less bytes than we need make sure to read the rest
         * before continuing.
         */
        // Global::debug(0) << "Read string of length " << size << std::endl;
        if (size > 0 && length + size <= contains){
            char out[size+1];
            memcpy(out, buffer + length, size);
            length += size;
            out[size] = 0;
            str = string(out);
        }
        return *this;
    }

    virtual void readAll(const Network::Socket & socket){
        contains = Network::readUptoBytes(socket, (uint8_t*) buffer, actualLength);
    }

    virtual void add(const char * data, int size){
        checkBuffer(size);
        memcpy(buffer + length, data, size);
        length += size;
    }

    char * getBuffer(){
        return buffer;
    }

    /* make sure N bytes can be stored */
    void checkBuffer(int bytes){
        if (length + bytes >= actualLength){
            increaseBuffer(bytes);
        }
    }

    void send(const Network::Socket & socket){
        Network::sendBytes(socket, (uint8_t*) getBuffer(), getLength());
    }

    /* will do a single nlWrite instead of trying to send all the bytes.
     * The buffer length had better be below the maximum packet size
     * which is something around 64k
     */
    /* mostly useful for udp */
    void sendAllxx(const Network::Socket & socket){
        Network::sendAllBytes(socket, (uint8_t*) getBuffer(), getLength());
    }

    int getLength() const {
        return length;
    }

    void increaseBuffer(int minimum){
        int newLength = PaintownUtil::max(actualLength * 2, actualLength + minimum);
        char * next = new char[newLength];
        memcpy(next, buffer, length);
        delete[] buffer;
        buffer = next;
        actualLength = newLength;
    }

protected:
    int length;
    char * buffer;
    int actualLength;
    int contains;
};

static const int16_t NetworkMagic = 0xd97f; 

class Packet{
public:
    enum Type{
        InputType,
        PingType,
        WorldType
    };

    Packet(Type type):
    type(type){
    }

    Type getType() const {
        return type;
    }

    Type type;
};

class InputPacket: public Packet {
public:
    InputPacket(const Input & inputs, uint32_t tick):
    Packet(InputType),
    inputs(inputs),
    tick(tick){
    }

    InputPacket():
    Packet(InputType),
    tick(0){
    }

    Input inputs;
    uint32_t tick;
};

class PingPacket: public Packet {
public:
    PingPacket(int ping):
    Packet(PingType),
    ping(ping){
    }

    int16_t ping;

    int16_t getPing() const {
        return ping;
    }
};

class WorldPacket: public Packet {
public:
    WorldPacket(const PaintownUtil::ReferenceCount<World> & world):
    Packet(WorldType),
    world(world){
    }

    PaintownUtil::ReferenceCount<World> world;

    const PaintownUtil::ReferenceCount<World> & getWorld() const {
        return world;
    }
    
};

static PaintownUtil::ReferenceCount<Packet> readPacket(const Network::Socket & socket){
    /* TODO */
    return PaintownUtil::ReferenceCount<Packet>(NULL);
}

static PaintownUtil::ReferenceCount<Packet> readPacket(NetworkBuffer & buffer){
    int16_t magic;
    buffer >> magic;
    if (magic != NetworkMagic){
        return PaintownUtil::ReferenceCount<Packet>(NULL);
    }
    int16_t type;
    buffer >> type;
    switch (type){
        case Packet::InputType: {
            /* FIXME! */
            /*
            uint32_t ticks = 0;
            int16_t inputCount = 0;
            buffer >> ticks;
            buffer >> inputCount;
            // Global::debug(0) << "Tick " << ticks << " inputs " << inputCount << std::endl;
            vector<string> inputs;
            for (int i = 0; i < inputCount; i++){
                string input;
                buffer >> input;
                inputs.push_back(input);
            }
            return PaintownUtil::ReferenceCount<Packet>(new InputPacket(inputs, ticks));
            */
            break;
        }
        case Packet::PingType: {
            int16_t clientPing = 0;
            buffer >> clientPing;
            return PaintownUtil::ReferenceCount<Packet>(new PingPacket((uint16_t) clientPing));
            break;
        }
        case Packet::WorldType: {
            /* FIXME! */
            break;
        }
        default: {
            std::ostringstream out;
            out << "Unknown packet type: " << type;
            throw MugenException(out.str(), __FILE__, __LINE__);
        }
    }
    return PaintownUtil::ReferenceCount<Packet>(NULL);
}

static void sendPacket(const Network::Socket & socket, const PaintownUtil::ReferenceCount<Packet> & packet){
    switch (packet->type){
        case Packet::InputType: {
            NetworkBuffer buffer;
            PaintownUtil::ReferenceCount<InputPacket> input = packet.convert<InputPacket>();
            buffer << NetworkMagic;
            buffer << (int16_t) Packet::InputType;
            buffer << input->tick;

            Token * data = new Token("input");

            *data->newToken() << "a" << input->inputs.a;
            *data->newToken() << "b" << input->inputs.b;
            *data->newToken() << "c" << input->inputs.c;
            *data->newToken() << "x" << input->inputs.x;
            *data->newToken() << "y" << input->inputs.y;
            *data->newToken() << "z" << input->inputs.z;
            *data->newToken() << "back" << input->inputs.back;
            *data->newToken() << "forward" << input->inputs.forward;
            *data->newToken() << "up" << input->inputs.up;
            *data->newToken() << "down" << input->inputs.down;

            buffer << data->toStringCompact();
            delete data;

            // Global::debug(0) << "Send packet of " << buffer.getLength() << " bytes " << std::endl;
            buffer.send(socket);
            break;
        }
        case Packet::WorldType: {
            PaintownUtil::ReferenceCount<WorldPacket> world = packet.convert<WorldPacket>();
            Token * test = world->getWorld()->serialize();
            Token * filtered = filterTokens(test);
            // Global::debug(0) << "Snapshot: " << filtered->toString() << std::endl;
            string compact = filtered->toStringCompact();
            // Global::debug(0) << "Size: " << compact.size() << std::endl;
            char * out = new char[LZ4_compressBound(compact.size())];
            int compressed = LZ4_compress(compact.c_str(), out, compact.size());

            NetworkBuffer buffer;
            buffer << (int16_t) NetworkMagic;
            buffer << (int16_t) Packet::WorldType;
            buffer << (int16_t) compressed;
            buffer << (int16_t) compact.size();
            buffer.add(out, compressed);
            buffer.send(socket);

            // Global::debug(0) << "Compressed size: " << compressed << std::endl;
            delete[] out;

            delete test;
            delete filtered;

            break;
        }
        case Packet::PingType: {
            PaintownUtil::ReferenceCount<PingPacket> ping = packet.convert<PingPacket>();
            NetworkBuffer buffer;
            buffer << (int16_t) NetworkMagic;
            buffer << (int16_t) Packet::PingType;
            buffer << (int16_t) ping->getPing();
            buffer.send(socket);
            break;
        }
        default: {
            throw MugenException("Unknown packet type", __FILE__, __LINE__);
        }
    }
}

/* We don't have the latest commands so just assume if holdfwd/back/down was pressed
 * before that its still being pressed.
 */
class NetworkBehavior: public Behavior {
public:
    NetworkBehavior():
    lastTick(0){
    }

    uint32_t lastTick;
    Input lastInput;
    std::map<uint32_t, Input> history;

    virtual void setInput(uint32_t tick, const Input & input){
        if (tick > lastTick){
            lastTick = tick;
            lastInput = input;
        }
    }

    virtual std::vector<std::string> currentCommands(const Stage & stage, Character * owner, const std::vector<Command*> & commands, bool reversed){
        vector<string> out;
        return out;
    }

    /* called when the player changes direction. useful for updating
     * the input mapping.
     */
    virtual void flip(){
    }

    virtual ~NetworkBehavior(){
    }
};

class NetworkServerObserver: public NetworkObserver {
public:
    NetworkServerObserver(Network::Socket reliable, const PaintownUtil::ReferenceCount<Character> & player1, const PaintownUtil::ReferenceCount<Character> & player2, HumanBehavior & player1Behavior, NetworkBehavior & player2Behavior):
    NetworkObserver(),
    reliable(reliable),
    player1(player1),
    player2(player2),
    player1Behavior(player1Behavior),
    player2Behavior(player2Behavior),
    sendThread(this, send),
    receiveThread(this, receive),
    alive_(true),
    count(0),
    lastPing(System::currentMilliseconds()),
    ping(0){
    }

    PaintownUtil::Thread::LockObject lock;
    Network::Socket reliable;
    PaintownUtil::ReferenceCount<Character> player1;
    PaintownUtil::ReferenceCount<Character> player2;
    HumanBehavior & player1Behavior;
    NetworkBehavior & player2Behavior;
    PaintownUtil::Thread::ThreadObject sendThread;
    PaintownUtil::Thread::ThreadObject receiveThread;

    vector<PaintownUtil::ReferenceCount<Packet> > outBox;
    vector<PaintownUtil::ReferenceCount<Packet> > inBox;

    Input lastInput;
    
    /* mapping from logical ping to the time in milliseconds when the ping was sent */
    std::map<int, uint64_t> pings;

    bool alive_;
    uint32_t count;
    uint64_t lastPing;
    uint16_t ping;

    bool alive(){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        return alive_;
    }

    void kill(){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        alive_ = false;
    }

    uint16_t nextPing(){
        uint16_t out = ping;
        ping += 1;
        return out;
    }

    void sendPacket(const PaintownUtil::ReferenceCount<Packet> & packet){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        outBox.push_back(packet);
    }

    PaintownUtil::ReferenceCount<Packet> getSendPacket(){
        PaintownUtil::ReferenceCount<Packet> out;

        {
            PaintownUtil::Thread::ScopedLock scoped(lock);
            if (outBox.size() > 0){
                out = outBox.front();
                outBox.erase(outBox.begin());
            }
        }

        return out;
    }

    void doSend(){
        while (alive()){
            PaintownUtil::ReferenceCount<Packet> nextPacket = getSendPacket();
            if (nextPacket != NULL){
                Mugen::sendPacket(reliable, nextPacket);
            } else {
                PaintownUtil::rest(1);
            }
        }
    }

    static void * send(void * self_){
        NetworkServerObserver * self = (NetworkServerObserver*) self_;
        self->doSend();
        return NULL;
    }

    void handlePacket(const PaintownUtil::ReferenceCount<Packet> & packet){
        switch (packet->getType()){
            case Packet::PingType: {
                PaintownUtil::ReferenceCount<PingPacket> ping = packet.convert<PingPacket>();
                int16_t client = ping->getPing();
                if (pings.find(client) != pings.end()){
                    Global::debug(0) << "Client ping: " << (System::currentMilliseconds() - pings[client]) << std::endl;
                    pings.erase(client);
                }
                break;
            }
            case Packet::InputType: {
                PaintownUtil::ReferenceCount<InputPacket> input = packet.convert<InputPacket>();
                addInput(*input);
                break;
            }
            case Packet::WorldType: {
                Global::debug(0) << "Should not have gotten a world packet from the client" << std::endl;
                break;
            }
        }
    }

    void doReceive(){
        while (alive()){
            PaintownUtil::ReferenceCount<Packet> nextPacket = readPacket(reliable);
            if (nextPacket != NULL){
                handlePacket(nextPacket);
            }
        }
    }

    static void * receive(void * self_){
        NetworkServerObserver * self = (NetworkServerObserver*) self_;
        self->doReceive();
        return NULL;
    }
    
    virtual void start(){
        sendThread.start();
        receiveThread.start();
    }

    PaintownUtil::ReferenceCount<World> lastState;
    std::map<uint32_t, InputPacket> inputs;

    void addInput(const InputPacket & input){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        inputs[input.tick] = input;
    }

    std::map<uint32_t, InputPacket> getInputs(){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        std::map<uint32_t, InputPacket> out = inputs;
        inputs.clear();
        return out;
    }
    
    virtual void beforeLogic(Stage & stage){
        if (count % 30 == 0){
            sendPacket(PaintownUtil::ReferenceCount<Packet>(new WorldPacket(stage.snapshotState())));
        }
        count += 1;

        uint32_t currentTicks = stage.getTicks();

        std::map<uint32_t, InputPacket> useInputs = getInputs();
        if (inputs.size() > 0){
            for (std::map<uint32_t, InputPacket>::iterator it = useInputs.begin(); it != useInputs.end(); it++){
                uint32_t tick = it->first;
                const InputPacket & input = it->second;
                // player2->setInputs(tick, input.inputs);
                // player2Behavior.setInput(tick, input.inputs);
            }

            if (lastState != NULL && currentTicks > lastState->getStageData().ticker){
                stage.updateState(*lastState);
                Mugen::Sound::disableSounds();
                for (uint32_t i = 0; i < currentTicks - lastState->getStageData().ticker; i++){
                    stage.logic();
                }
                Mugen::Sound::enableSounds();
            }
        }

        if (System::currentMilliseconds() - lastPing > 1000){
            lastPing = System::currentMilliseconds();
            uint16_t ping = nextPing();
            pings[ping] = lastPing;
            sendPacket(PaintownUtil::ReferenceCount<Packet>(new PingPacket(ping)));
        }
    }

    virtual void afterLogic(Stage & stage){
        Input latest = player1Behavior.getInput();
        if (latest != lastInput){
            lastInput = latest;
            /*
            Global::debug(0) << "Tick " << stage.getTicks() << std::endl;
            for (vector<string>::iterator it = inputs.begin(); it != inputs.end(); it++){
                Global::debug(0) << "Input: " << *it << std::endl;
            }
            */
            sendPacket(PaintownUtil::ReferenceCount<Packet>(new InputPacket(latest, stage.getTicks())));
        }
    }
};

/* FIXME: fix the whole client */
class NetworkClientObserver: public NetworkObserver {
public:
    NetworkClientObserver(Network::Socket socket, Network::Socket unreliable, const PaintownUtil::ReferenceCount<Character> & player1, const PaintownUtil::ReferenceCount<Character> & player2, NetworkBehavior & player2Behavior):
    NetworkObserver(),
    socket(socket),
    unreliable(unreliable),
    player1(player1),
    player2(player2),
    player2Behavior(player2Behavior),
    thread(this, receive),
    input(this, input_),
    alive_(true){
    }

    Network::Socket socket;
    Network::Socket unreliable;
    PaintownUtil::ReferenceCount<Character> player1;
    PaintownUtil::ReferenceCount<Character> player2;
    NetworkBehavior & player2Behavior;
    PaintownUtil::Thread::ThreadObject thread;
    PaintownUtil::Thread::ThreadObject input;
    PaintownUtil::ReferenceCount<World> world;
    PaintownUtil::Thread::LockObject lock;
    bool alive_;
    std::map<uint32_t, InputPacket> inputs;

    void setWorld(const PaintownUtil::ReferenceCount<World> & world){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        this->world = world;
    }

    PaintownUtil::ReferenceCount<World> getWorld(){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        PaintownUtil::ReferenceCount<World> out = world;
        world = NULL;
        return out;
    }

    bool alive(){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        return alive_;
    }

    virtual void kill(){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        alive_ = false;
        Network::close(unreliable);
    }

    void sendPing(int16_t ping, Network::Socket socket){
        NetworkBuffer buffer;
        buffer << (int16_t) NetworkMagic;
        buffer << (int16_t) Packet::PingType;
        buffer << (int16_t) ping;
        buffer.send(socket);
    }

    void doReceive(){
        while (alive()){

            int16_t magic = Network::read16(socket);
            if (magic != NetworkMagic){
                Global::debug(0) << "Garbage message packet: " << magic << std::endl;
                continue;
            }

            int16_t type = Network::read16(socket);

            switch (type){
                case Packet::WorldType: {
                    int16_t compressed = Network::read16(socket);
                    int16_t uncompressed = Network::read16(socket);
                    uint8_t * data = new uint8_t[compressed];
                    Network::readBytes(socket, data, compressed);
                    uint8_t * what = new uint8_t[uncompressed + 1];
                    what[uncompressed] = '\0';
                    LZ4_uncompress((const char *) data, (char *) what, uncompressed);
                    TokenReader reader;
                    std::string use((const char *) what);
                    Token * head = reader.readTokenFromString(use);
                    // Global::debug(0) << "Client received token " << head->toString() << std::endl;
                    if (head != NULL){
                        PaintownUtil::ReferenceCount<World> world(World::deserialize(head));
                        setWorld(world);
                    }
                    break;
                }
                case Packet::PingType: {
                    int16_t ping = Network::read16(socket);
                    sendPing(ping, socket);
                    break;
                }
            }
        }
    }

    static void * receive(void * self){
        ((NetworkClientObserver*)self)->doReceive();
        return NULL;
    }
    
    virtual void start(){
        thread.start();
        input.start();
    }

    static void * input_(void * self){
        ((NetworkClientObserver*)self)->doClientInput();
        return NULL;
    }

    void addInput(const InputPacket & input){
        PaintownUtil::Thread::ScopedLock scoped(lock);
        inputs[input.tick] = input;
    }

    void doClientInput(){
        try{
            while (alive()){
                NetworkBuffer buffer(1024);
                buffer.readAll(unreliable);
                int16_t magic = 0;
                buffer >> magic;
                if (magic != NetworkMagic){
                    Global::debug(0) << "Garbage udp packet: " << magic << std::endl;
                    continue;
                }

                PaintownUtil::ReferenceCount<Packet> packet = readPacket(buffer);
                if (packet != NULL){
                    switch (packet->type){
                        case Packet::InputType: {
                            PaintownUtil::ReferenceCount<InputPacket> input = packet.convert<InputPacket>();
                            addInput(*input);
                            /*
                            Global::debug(0) << "Received inputs " << input->inputs.size() << std::endl;
                            for (vector<string>::iterator it = input->inputs.begin(); it != input->inputs.end(); it++){
                                Global::debug(0) << " '" << *it << "'" << std::endl;
                            }
                            */
                            break;
                        }
                    }
                }
            }
        } catch (const Exception::Base & ex){
            Global::debug(0) << "Error in client read input thread. " << ex.getTrace() << std::endl;
        }
    }

    PaintownUtil::ReferenceCount<World> lastState;
    
    virtual void beforeLogic(Stage & stage){
        uint32_t currentTicks = stage.getTicks();
        PaintownUtil::ReferenceCount<World> next = getWorld();
        if (next != NULL){
            stage.updateState(*next);
            lastState = next;
        }

        PaintownUtil::Thread::ScopedLock scoped(lock);
        if (inputs.size() > 0){
            for (std::map<uint32_t, InputPacket>::iterator it = inputs.begin(); it != inputs.end(); it++){
                uint32_t tick = it->first;
                const InputPacket & input = it->second;
                /*
                player2->setInputs(tick, input.inputs);
                player2Behavior.setCommands(tick, input.inputs);
                */
            }
            inputs.clear();

            if (lastState != NULL && currentTicks > lastState->getStageData().ticker){
                stage.updateState(*lastState);
                Mugen::Sound::disableSounds();
                for (uint32_t i = 0; i < currentTicks - lastState->getStageData().ticker; i++){
                    stage.logic();
                }
                Mugen::Sound::enableSounds();
            }
        }
    }

    virtual void afterLogic(Stage & stage){
        /* FIXME! */
        vector<string> inputs = player1->currentInputs();
        if (inputs.size() >= 0){
            /*
            Global::debug(0) << "Tick " << stage.getTicks() << std::endl;
            for (vector<string>::iterator it = inputs.begin(); it != inputs.end(); it++){
                Global::debug(0) << "Input: " << *it << std::endl;
            }
            */

            /*
            PaintownUtil::ReferenceCount<InputPacket> packet(new InputPacket(inputs, stage.getTicks()));
            sendPacket(unreliable, packet.convert<Packet>());
            */
        }
    }
};

void Game::startNetworkVersus1(const PaintownUtil::ReferenceCount<Character> & player1,
                               const PaintownUtil::ReferenceCount<Character> & player2,
                               Stage & stage,
                               bool server, const std::string & host, int port){

    try{
        Network::reuseSockets(true);

        Network::Socket socket = 0;
        if (server){
            Network::Socket remote = Network::openReliable(port);
            Network::listen(remote);
            Global::debug(0) << "Waiting for a connection on port " << port << std::endl;
            socket = Network::accept(remote);
            Network::close(remote);
            Global::debug(0) << "Got a connection" << std::endl;
        } else {
            int maxTries = 5;
            int tries = 0;
            for (tries = 0; tries < maxTries; tries++){
                Global::debug(0) << "Connecting to " << host << " on port " << port << ". Attempt " << (tries + 1) << "/" << maxTries << std::endl;
                try{
                    socket = Network::connectReliable(host, port); 
                    Global::debug(0) << "Connected" << std::endl;
                    break;
                } catch (const Network::NetworkException & fail){
                    Global::debug(0) << "Failed to connect: " << fail.getMessage() << std::endl;
                    PaintownUtil::rest(1000);
                }
            }
            if (tries == maxTries){
                throw MugenException("Could not connect", __FILE__, __LINE__);
            }
        }

        HumanBehavior player1Behavior(getPlayer1Keys(), getPlayer1InputLeft());
        NetworkBehavior player2Behavior;
        // DummyBehavior player2Behavior;

        // NetworkLocalBehavior player1Behavior(&local1Behavior, socket);
        // NetworkRemoteBehavior player2Behavior(socket);

        // Set regenerative health
        player1->setRegeneration(false);
        player2->setRegeneration(false);
        PaintownUtil::ReferenceCount<NetworkObserver> observer;
        if (server){
            player1->setBehavior(&player1Behavior);
            player2->setBehavior(&player2Behavior);
            observer = PaintownUtil::ReferenceCount<NetworkObserver>(new NetworkServerObserver(socket, player1, player2, player1Behavior, player2Behavior));
            stage.setObserver(observer.convert<StageObserver>());
        } else {
            player2->setBehavior(&player1Behavior);
            player1->setBehavior(&player2Behavior);
            /*
            observer = PaintownUtil::ReferenceCount<NetworkObserver>(new NetworkClientObserver(socket, udp, player2, player1, player2Behavior));
            stage.setObserver(observer.convert<StageObserver>());
            */
        }

        RunMatchOptions options;

        options.setBehavior(&player1Behavior, NULL);

        /* server is player1 */
        if (server){
            stage.addPlayer1(player1.raw());
            stage.addPlayer2(player2.raw());
        } else {
            stage.addPlayer1(player1.raw());
            stage.addPlayer2(player2.raw());
        }

        stage.reset();
        int time = Mugen::Data::getInstance().getTime();
        Mugen::Data::getInstance().setTime(-1);

        /* Synchronize client and server at this point */
        if (server){
            int sync = Network::read16(socket);
            Network::send16(socket, sync);
        } else {
            Network::send16(socket, 0);
            Network::read16(socket);
        }

        observer->start();

        /*
           if (!Network::blocking(socket, false)){
           Global::debug(0) << "Could not set socket to be non-blocking" << std::endl;
           }
           */

        /*
           player1Behavior.begin();
           player2Behavior.begin();
           */

        /*
           if (!Network::noDelay(socket, true)){
           Global::debug(0) << "Could not set no delay!" << std::endl;
           }
           */

        try{
            runMatch(&stage, "", options);
        } catch (const QuitGameException & ex){
        } catch (const MugenException & ex){
            Global::debug(0) << ex.getTrace() << std::endl;
        } catch (const Exception::Base & ex){
            Global::debug(0) << ex.getTrace() << std::endl;
        }
        Mugen::Data::getInstance().setTime(time);

        observer->kill();

        Network::close(socket);

    } catch (const Network::NetworkException & fail){
        Global::debug(0) << "Network exception: " << fail.getMessage() << std::endl;
    }
}

}

#endif
