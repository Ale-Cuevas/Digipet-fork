#include "familiar_battle_service.h"

#include "game/vpet/familiar_battle_rules.h"

#include <NimBLEAdvertisedDevice.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <WiFi.h>
#include <algorithm>
#include <cstring>

// The wire protocol's move/capability values are trusted to line up with
// familiar_battle_rules.h's mirrors bit-for-bit (see that header's
// comments) -- both headers are visible here, so check it rather than
// trust the comments alone.
static_assert(static_cast<uint8_t>(BattleRulesMove::Attack) ==
                  static_cast<uint8_t>(FamiliarBattleMove::Attack) &&
              static_cast<uint8_t>(BattleRulesMove::Defend) ==
                  static_cast<uint8_t>(FamiliarBattleMove::Defend) &&
              static_cast<uint8_t>(BattleRulesMove::Special) ==
                  static_cast<uint8_t>(FamiliarBattleMove::Special) &&
              static_cast<uint8_t>(BattleRulesMove::Flee) ==
                  static_cast<uint8_t>(FamiliarBattleMove::Flee),
              "BattleRulesMove must mirror FamiliarBattleMove");
static_assert(BattleRulesCapability::BodyType == FamiliarCapBodyType &&
              BattleRulesCapability::Element == FamiliarCapElement &&
              BattleRulesCapability::Speed == FamiliarCapSpeed &&
              BattleRulesCapability::Special == FamiliarCapSpecial &&
              BattleRulesCapability::MoveMatchups == FamiliarCapMoveMatchups,
              "BattleRulesCapability must mirror FamiliarBattleCapability");

namespace {
// Same Nordic UART Service UUID trio chameleon_ultra_client.cpp already
// reuses here ("a de facto industry-wide UUID scheme... safe to reuse
// directly") -- one write characteristic (challenger -> host) and one
// notify characteristic (host -> challenger).
const NimBLEUUID kServiceUuid("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
const NimBLEUUID kWriteCharUuid("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
const NimBLEUUID kNotifyCharUuid("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");

// Manufacturer data company ID 0xFFFF (reserved for testing, per the
// Bluetooth SIG) -- same category ble_spam_service.cpp already draws
// ad-hoc IDs from for its own synthetic advertisements.
constexpr uint8_t kCompanyIdLow = 0xFF;
constexpr uint8_t kCompanyIdHigh = 0xFF;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint32_t kScanDurationMs = 4000;
constexpr uint32_t kConnectTimeoutMs = 6000;

constexpr uint8_t kMsgHello = 0x01;
constexpr uint8_t kMsgMove = 0x02;
constexpr uint8_t kMsgCapabilities = 0x03;
constexpr uint8_t kMsgGenomeChunk = 0x04;
constexpr uint8_t kCapabilitiesSchema = 1;

BattleRulesCapabilities toRulesCapabilities(const FamiliarBattleCapabilities& capabilities) {
    BattleRulesCapabilities rules;
    rules.bodyType = capabilities.bodyType;
    rules.element = capabilities.element;
    rules.speed = capabilities.speed;
    rules.special = capabilities.special;
    return rules;
}

FamiliarBattleService* g_owner = nullptr;

class BattleServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        if (g_owner != nullptr) g_owner->onPeerConnected();
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        if (g_owner != nullptr) g_owner->onPeerDisconnected();
    }
};
BattleServerCallbacks battleServerCallbacks;

class BattleWriteCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        if (g_owner == nullptr) return;
        const NimBLEAttValue value = characteristic->getValue();
        g_owner->onRawInbound(value.data(), value.size());
    }
};
BattleWriteCallbacks battleWriteCallbacks;
}  // namespace

uint16_t FamiliarBattleService::deriveMaxHp(uint8_t level) {
    return battleRulesDeriveMaxHp(level);
}

uint8_t FamiliarBattleService::deriveAttack(uint8_t level, uint8_t stageIndex) {
    return battleRulesDeriveAttack(level, stageIndex);
}

uint8_t FamiliarBattleService::deriveDefense(uint8_t level, uint8_t stageIndex) {
    return battleRulesDeriveDefense(level, stageIndex);
}

void FamiliarBattleService::beginRadio() {
    // Both radios share the ESP32-S3 radio resources -- same handoff every
    // other BLE feature in this codebase uses before touching BLE.
    WiFi.scanDelete();
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    delay(150);
    if (!initialized_) {
        NimBLEDevice::init("Ghostwire VPet");
        initialized_ = true;
    }
    g_owner = this;
}

void FamiliarBattleService::teardownRadio() {
    if (initialized_) {
        NimBLEDevice::deinit(true);
        initialized_ = false;
    }
    if (g_owner == this) g_owner = nullptr;
}

void FamiliarBattleService::resetForNewBattle() {
    opponent_ = FamiliarBattleOpponent{};
    opponentAttack_ = 0;
    opponentDefense_ = 0;
    myHp_ = myMaxHp_ = 0;
    opponentHp_ = opponentMaxHp_ = 0;
    turnNumber_ = 0;
    myMoveSubmitted_ = opponentMoveSubmitted_ = false;
    helloSent_ = helloReceived_ = false;
    prngState_ = 1;
    outcome_ = FamiliarBattleOutcome::None;
    log_.clear();
    server_ = nullptr;
    writeChar_ = nullptr;
    notifyChar_ = nullptr;
    client_ = nullptr;
    remoteWriteChar_ = nullptr;
    inboundHead_ = 0;
    inboundTail_ = 0;
    connectPending_ = false;
    disconnectPending_ = false;
    myGenomeCode_[0] = '\0';
    memset(opponentGenomeCode_, 0, sizeof(opponentGenomeCode_));
    opponentGenomeChunks_ = 0;
}

bool FamiliarBattleService::beginHost(
    uint32_t playerId, uint8_t stageIndex, uint8_t level,
    const FamiliarBattleCapabilities& capabilities, const char* genomeCode,
    FamiliarBattleMode mode) {
    end();
    resetForNewBattle();
    isHost_ = true;
    myMode_ = mode;
    myPlayerId_ = playerId;
    myStageIndex_ = stageIndex;
    myLevel_ = level;
    myCapabilities_ = capabilities;
    if (genomeCode && strlen(genomeCode) == 60)
        strncpy(myGenomeCode_, genomeCode, sizeof(myGenomeCode_) - 1);
    myAttack_ = deriveAttack(level, stageIndex);
    myDefense_ = deriveDefense(level, stageIndex);
    myMaxHp_ = deriveMaxHp(level);
    myHp_ = myMaxHp_;

    beginRadio();
    server_ = NimBLEDevice::createServer();
    if (server_ == nullptr) {
        status_ = "BLE server unavailable";
        teardownRadio();
        state_ = FamiliarBattleState::Idle;
        return false;
    }
    // NimBLEServer::setCallbacks() defaults to owning (and later deleting)
    // the callbacks pointer -- battleServerCallbacks is a static object,
    // not heap-allocated, so that would crash on teardown. Pass false.
    server_->setCallbacks(&battleServerCallbacks, false);
    NimBLEService* service = server_->createService(kServiceUuid);
    writeChar_ = service->createCharacteristic(kWriteCharUuid,
                                               NIMBLE_PROPERTY::WRITE);
    notifyChar_ = service->createCharacteristic(kNotifyCharUuid,
                                                NIMBLE_PROPERTY::NOTIFY);
    if (writeChar_ == nullptr || notifyChar_ == nullptr) {
        status_ = "BLE characteristic setup failed";
        teardownRadio();
        state_ = FamiliarBattleState::Idle;
        return false;
    }
    writeChar_->setCallbacks(&battleWriteCallbacks);
    // NimBLEService::start() (not called here) is a deprecated no-op in
    // this NimBLE-Arduino version -- services start automatically with the
    // *server's* start() below, per its own deprecation note.
    if (!server_->start()) {
        status_ = "BLE server failed to start";
        teardownRadio();
        state_ = FamiliarBattleState::Idle;
        return false;
    }

    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    if (advertising == nullptr) {
        status_ = "Advertising unavailable";
        teardownRadio();
        state_ = FamiliarBattleState::Idle;
        return false;
    }
    NimBLEAdvertisementData advData;
    uint8_t payload[12];
    payload[0] = kCompanyIdLow;
    payload[1] = kCompanyIdHigh;
    payload[2] = 'V';
    payload[3] = 'P';
    payload[4] = kProtocolVersion;
    payload[5] = static_cast<uint8_t>(playerId >> 24);
    payload[6] = static_cast<uint8_t>(playerId >> 16);
    payload[7] = static_cast<uint8_t>(playerId >> 8);
    payload[8] = static_cast<uint8_t>(playerId);
    payload[9] = stageIndex;
    payload[10] = level;
    payload[11] = 1;  // battle enabled
    advData.setManufacturerData(payload, sizeof(payload));
    advertising->setAdvertisementData(advData);
    advertising->addServiceUUID(kServiceUuid);
    if (mode == FamiliarBattleMode::FriendExchange) {
        // Friend discovery does not need battle matchmaking's fast response.
        // BLE interval units are 0.625 ms, so this advertises roughly once
        // every 0.8-1.0 seconds instead of using NimBLE's much faster default.
        // It remains easy to find during a four-second Nearby scan while
        // substantially reducing radio wakeups during the visible mode.
        advertising->setMinInterval(1280);
        advertising->setMaxInterval(1600);
    }
    if (!advertising->start()) {
        status_ = "Advertising failed";
        teardownRadio();
        state_ = FamiliarBattleState::Idle;
        return false;
    }

    state_ = FamiliarBattleState::Hosting;
    status_ = "Waiting for a challenger...";
    return true;
}

void FamiliarBattleService::parseAdvertisement(
    const NimBLEAdvertisedDevice* advertised) {
    if (advertised == nullptr) return;

    FamiliarBattleOpponent found;
    bool haveIdentity = false;
    if (advertised->haveManufacturerData()) {
        const std::string data = advertised->getManufacturerData();
        if (data.size() >= 12) {
            const auto byte = [&data](size_t index) {
                return static_cast<uint8_t>(data[index]);
            };
            if (byte(0) == kCompanyIdLow && byte(1) == kCompanyIdHigh &&
                byte(2) == 'V' && byte(3) == 'P' &&
                byte(4) == kProtocolVersion && byte(11) != 0) {
                found.playerId = (static_cast<uint32_t>(byte(5)) << 24) |
                                 (static_cast<uint32_t>(byte(6)) << 16) |
                                 (static_cast<uint32_t>(byte(7)) << 8) |
                                 byte(8);
                found.stageIndex = byte(9);
                found.level = byte(10);
                haveIdentity = true;
            }
        }
    }
    // Fall back to a bare service-UUID match (no player/level info yet --
    // filled in once the real HELLO handshake happens after connecting).
    // Cross-platform BLE peripheral libraries (the desktop simulator uses
    // `bless`) don't all expose custom manufacturer data as reliably as
    // NimBLE does, but registering a GATT service always advertises its
    // UUID, so this keeps discovery working against those peers too.
    if (!haveIdentity && !advertised->isAdvertisingService(kServiceUuid)) {
        return;
    }

    found.address = advertised->getAddress().toString().c_str();
    found.addressType = advertised->getAddressType();
    found.rssi = advertised->getRSSI();
    if (!haveIdentity) found.level = 1;  // placeholder for the picker list

    for (auto& existing : scanResults_) {
        if (existing.address == found.address) {
            existing = found;
            return;
        }
    }
    scanResults_.push_back(found);
}

bool FamiliarBattleService::beginFind(
    uint32_t playerId, uint8_t stageIndex, uint8_t level,
    const FamiliarBattleCapabilities& capabilities, const char* genomeCode,
    FamiliarBattleMode mode) {
    end();
    resetForNewBattle();
    isHost_ = false;
    myMode_ = mode;
    myPlayerId_ = playerId;
    myStageIndex_ = stageIndex;
    myLevel_ = level;
    myCapabilities_ = capabilities;
    if (genomeCode && strlen(genomeCode) == 60)
        strncpy(myGenomeCode_, genomeCode, sizeof(myGenomeCode_) - 1);
    myAttack_ = deriveAttack(level, stageIndex);
    myDefense_ = deriveDefense(level, stageIndex);
    myMaxHp_ = deriveMaxHp(level);
    myHp_ = myMaxHp_;

    state_ = FamiliarBattleState::Scanning;
    status_ = "Scanning...";
    scanResults_.clear();

    beginRadio();
    NimBLEScan* scanner = NimBLEDevice::getScan();
    if (scanner == nullptr) {
        status_ = "BLE scanner unavailable";
        teardownRadio();
        state_ = FamiliarBattleState::Idle;
        return false;
    }
    scanner->clearResults();
    scanner->setActiveScan(true);
    scanner->setInterval(100);
    scanner->setWindow(80);
    NimBLEScanResults results = scanner->getResults(kScanDurationMs, false);
    const int count = results.getCount();
    for (int i = 0; i < count; ++i) {
        parseAdvertisement(results.getDevice(i));
    }
    scanner->clearResults();
    teardownRadio();

    std::sort(scanResults_.begin(), scanResults_.end(),
              [](const FamiliarBattleOpponent& a, const FamiliarBattleOpponent& b) {
                  return a.rssi > b.rssi;
              });
    state_ = FamiliarBattleState::Idle;
    status_ = scanResults_.empty() ? "No VPet badges found"
                                   : String(scanResults_.size()) + " found";
    return true;
}

std::vector<uint32_t> FamiliarBattleService::scanNearbyBle(uint32_t durationMs) {
    std::vector<uint32_t> hashes;
    if (state_ != FamiliarBattleState::Idle) return hashes;

    beginRadio();
    NimBLEScan* scanner = NimBLEDevice::getScan();
    if (scanner != nullptr) {
        scanner->clearResults();
        scanner->setActiveScan(false);  // passive -- never transmits a scan request
        scanner->setInterval(100);
        scanner->setWindow(80);
        NimBLEScanResults results = scanner->getResults(durationMs, false);
        const int count = results.getCount();
        hashes.reserve(count);
        for (int i = 0; i < count; ++i) {
            const NimBLEAdvertisedDevice* device = results.getDevice(i);
            if (device == nullptr) continue;
            const std::string address = device->getAddress().toString();
            uint32_t hash = 2166136261u;  // FNV-1a, same constants as main.cpp's own
            for (char c : address) {      // WiFi-BSSID hash -- see that comment for why
                hash ^= static_cast<uint8_t>(c);  // this isn't shared code.
                hash *= 16777619u;
            }
            hashes.push_back(hash);
        }
        scanner->clearResults();
    }
    teardownRadio();
    return hashes;
}

bool FamiliarBattleService::connectTo(size_t resultIndex) {
    if (resultIndex >= scanResults_.size()) return false;
    const FamiliarBattleOpponent target = scanResults_[resultIndex];

    beginRadio();
    client_ = NimBLEDevice::createClient();
    if (client_ == nullptr) {
        status_ = "BLE client unavailable";
        teardownRadio();
        return false;
    }
    const NimBLEAddress address(std::string(target.address.c_str()),
                                target.addressType);
    if (!client_->connect(address)) {
        status_ = "Connect failed";
        client_ = nullptr;
        teardownRadio();
        return false;
    }

    NimBLERemoteService* service = client_->getService(kServiceUuid);
    NimBLERemoteCharacteristic* notifyChar =
        service != nullptr ? service->getCharacteristic(kNotifyCharUuid)
                            : nullptr;
    remoteWriteChar_ = service != nullptr
                           ? service->getCharacteristic(kWriteCharUuid)
                           : nullptr;
    if (service == nullptr || notifyChar == nullptr ||
        remoteWriteChar_ == nullptr) {
        status_ = "VPet battle service not found";
        client_->disconnect();
        remoteWriteChar_ = nullptr;
        client_ = nullptr;
        teardownRadio();
        return false;
    }
    if (!notifyChar->subscribe(
            true, [this](NimBLERemoteCharacteristic*, uint8_t* data,
                         size_t length, bool) { onRawInbound(data, length); })) {
        status_ = "Notify subscribe failed";
        client_->disconnect();
        remoteWriteChar_ = nullptr;
        client_ = nullptr;
        teardownRadio();
        return false;
    }
    // Same 150ms settle as chameleon_ultra_client.cpp's connect(): the
    // CCCD subscription needs a moment on the peer's side before it's
    // actually ready to deliver notifications, even though the link
    // itself is already healthy at this point -- without it, HELLO's
    // response can beat that setup.
    delay(150);

    opponent_ = target;
    opponentAttack_ = deriveAttack(target.level, target.stageIndex);
    opponentDefense_ = deriveDefense(target.level, target.stageIndex);
    opponentMaxHp_ = deriveMaxHp(target.level);
    opponentHp_ = opponentMaxHp_;

    state_ = FamiliarBattleState::Connecting;
    status_ = "Connected - starting battle...";
    connectingDeadlineMs_ = millis() + kConnectTimeoutMs;
    sendHello();
    return true;
}

void FamiliarBattleService::end() {
    if (state_ == FamiliarBattleState::Idle && !initialized_) return;
    if (client_ != nullptr) {
        client_->disconnect();
        client_ = nullptr;
    }
    if (server_ != nullptr) {
        const std::vector<uint16_t> peers = server_->getPeerDevices();
        for (const uint16_t handle : peers) server_->disconnect(handle);
        const unsigned long deadline = millis() + 300;
        while (server_->getConnectedCount() > 0 &&
               static_cast<long>(deadline - millis()) > 0) {
            delay(10);
        }
    }
    teardownRadio();
    state_ = FamiliarBattleState::Idle;
    status_ = "Idle";
    isHost_ = false;
}

void FamiliarBattleService::onPeerConnected() { connectPending_ = true; }

void FamiliarBattleService::onPeerDisconnected() { disconnectPending_ = true; }

void FamiliarBattleService::onRawInbound(const uint8_t* data, size_t length) {
    const uint8_t head = inboundHead_;
    const uint8_t next = static_cast<uint8_t>((head + 1) % kInboundQueueDepth);
    if (next == inboundTail_) return;  // queue full -- drop rather than block
    InboundMessage& slot = inboundQueue_[head];
    slot.len = static_cast<uint8_t>(
        std::min(length, sizeof(InboundMessage::data)));
    memcpy(slot.data, data, slot.len);
    inboundHead_ = next;
}

void FamiliarBattleService::update() {
    if (connectPending_) {
        connectPending_ = false;
        if (isHost_ && state_ == FamiliarBattleState::Hosting) {
            status_ = "Challenger connected - awaiting handshake...";
        }
    }
    if (disconnectPending_) {
        disconnectPending_ = false;
        if (state_ == FamiliarBattleState::Battling ||
            state_ == FamiliarBattleState::Connecting) {
            addLog("Connection lost.");
            concludeBattle(FamiliarBattleOutcome::Disconnected);
        } else if (isHost_ && state_ == FamiliarBattleState::Hosting) {
            status_ = "Waiting for a challenger...";
        }
    }
    // Drain every queued message, not just one -- a peer can send its
    // HELLO reply immediately followed by its first MOVE with no gap
    // between them, and both can land before update() next runs.
    while (inboundTail_ != inboundHead_) {
        const InboundMessage& slot = inboundQueue_[inboundTail_];
        uint8_t buf[sizeof(InboundMessage::data)];
        const size_t len = slot.len;
        memcpy(buf, slot.data, len);
        inboundTail_ = static_cast<uint8_t>((inboundTail_ + 1) % kInboundQueueDepth);
        handleIncomingMessage(buf, len);
    }
    if (state_ == FamiliarBattleState::Connecting &&
        static_cast<long>(connectingDeadlineMs_ - millis()) < 0) {
        status_ = "Handshake timed out";
        concludeBattle(FamiliarBattleOutcome::Disconnected);
    }
}

void FamiliarBattleService::sendHello() {
    if (!isHost_) {
        // Only the challenger originates the shared seed -- the host
        // adopts whatever it receives in the challenger's HELLO.
        prngState_ = static_cast<uint32_t>(random(1, 2147483647)) ^
                    (static_cast<uint32_t>(micros()) << 1);
        if (prngState_ == 0) prngState_ = 1;
    }
    uint8_t buf[13];
    buf[0] = kMsgHello;
    buf[1] = static_cast<uint8_t>(myPlayerId_ >> 24);
    buf[2] = static_cast<uint8_t>(myPlayerId_ >> 16);
    buf[3] = static_cast<uint8_t>(myPlayerId_ >> 8);
    buf[4] = static_cast<uint8_t>(myPlayerId_);
    buf[5] = myStageIndex_;
    buf[6] = myLevel_;
    buf[7] = static_cast<uint8_t>(myHp_ >> 8);
    buf[8] = static_cast<uint8_t>(myHp_);
    buf[9] = static_cast<uint8_t>(prngState_ >> 24);
    buf[10] = static_cast<uint8_t>(prngState_ >> 16);
    buf[11] = static_cast<uint8_t>(prngState_ >> 8);
    buf[12] = static_cast<uint8_t>(prngState_);
    if (sendRaw(buf, sizeof(buf))) {
        helloSent_ = true;
        sendCapabilities();
        sendGenomeCode();
    }
}

void FamiliarBattleService::sendCapabilities() {
    if (myCapabilities_.flags == 0) return;
    const uint8_t buf[] = {
        kMsgCapabilities,
        kCapabilitiesSchema,
        static_cast<uint8_t>(myCapabilities_.flags >> 8),
        static_cast<uint8_t>(myCapabilities_.flags),
        myCapabilities_.bodyType,
        myCapabilities_.element,
        myCapabilities_.speed,
        myCapabilities_.special,
    };
    sendRaw(buf, sizeof(buf));
}

void FamiliarBattleService::sendGenomeCode() {
    if (strlen(myGenomeCode_) != 60) return;
    for (uint8_t chunk = 0; chunk < 5; ++chunk) {
        uint8_t buf[14] = {kMsgGenomeChunk, chunk};
        memcpy(buf + 2, myGenomeCode_ + chunk * 12, 12);
        sendRaw(buf, sizeof(buf));
    }
}

void FamiliarBattleService::sendMove(FamiliarBattleMove move) {
    uint8_t buf[2] = {kMsgMove, static_cast<uint8_t>(move)};
    sendRaw(buf, sizeof(buf));
}

bool FamiliarBattleService::sendRaw(const uint8_t* data, size_t length) {
    if (isHost_) {
        return notifyChar_ != nullptr && notifyChar_->notify(data, length);
    }
    return remoteWriteChar_ != nullptr &&
           remoteWriteChar_->writeValue(data, length, true);
}

void FamiliarBattleService::handleIncomingMessage(const uint8_t* data,
                                                  size_t length) {
    if (length == 0) return;
    switch (data[0]) {
        case kMsgHello: {
            if (length < 13) return;
            const uint32_t peerId = (static_cast<uint32_t>(data[1]) << 24) |
                                    (static_cast<uint32_t>(data[2]) << 16) |
                                    (static_cast<uint32_t>(data[3]) << 8) |
                                    data[4];
            const uint8_t peerStage = data[5];
            const uint8_t peerLevel = data[6];
            const uint16_t peerHp =
                (static_cast<uint16_t>(data[7]) << 8) | data[8];
            const uint32_t peerSeed = (static_cast<uint32_t>(data[9]) << 24) |
                                      (static_cast<uint32_t>(data[10]) << 16) |
                                      (static_cast<uint32_t>(data[11]) << 8) |
                                      data[12];
            opponent_.playerId = peerId;
            opponent_.stageIndex = peerStage;
            opponent_.level = peerLevel;
            opponentAttack_ = deriveAttack(peerLevel, peerStage);
            opponentDefense_ = deriveDefense(peerLevel, peerStage);
            opponentMaxHp_ = deriveMaxHp(peerLevel);
            opponentHp_ = peerHp > 0 ? peerHp : opponentMaxHp_;
            helloReceived_ = true;
            if (isHost_) {
                prngState_ = peerSeed == 0 ? 1 : peerSeed;
                sendHello();
            }
            if (helloSent_ && helloReceived_ &&
                state_ != FamiliarBattleState::Battling &&
                state_ != FamiliarBattleState::Exchanged) {
                // Same handshake either way -- myMode_ (set by whichever of
                // beginHost()/beginFind() started this connection) decides
                // what happens the instant both sides have it, not
                // anything peer-negotiated. See FamiliarBattleMode's own
                // comment for why this is a branch here rather than a
                // second connection stack.
                if (myMode_ == FamiliarBattleMode::FriendExchange) {
                    state_ = FamiliarBattleState::Exchanged;
                    status_ = "Friend added!";
                } else {
                    state_ = FamiliarBattleState::Battling;
                    status_ = "Battle!";
                    turnNumber_ = 1;
                    addLog("Lv " + String(peerLevel) + " Familiar appeared!");
                }
            }
            break;
        }
        case kMsgMove: {
            if (length < 2 || state_ != FamiliarBattleState::Battling) return;
            opponentMove_ = data[1] <= 3
                                ? static_cast<FamiliarBattleMove>(data[1])
                                : FamiliarBattleMove::Attack;
            if (opponentMove_ == FamiliarBattleMove::Flee) {
                addLog("Opponent fled!");
                concludeBattle(FamiliarBattleOutcome::OpponentFled);
                return;
            }
            opponentMoveSubmitted_ = true;
            resolveTurnIfReady();
            break;
        }
        case kMsgCapabilities: {
            if (length < 8 || data[1] != kCapabilitiesSchema) return;
            FamiliarBattleCapabilities received;
            received.flags = (static_cast<uint16_t>(data[2]) << 8) | data[3];
            received.bodyType = data[4];
            received.element = data[5];
            received.speed = data[6];
            received.special = data[7];
            if (received.bodyType >= 5) received.flags &= ~FamiliarCapBodyType;
            if (received.element >= 6) received.flags &= ~FamiliarCapElement;
            opponent_.capabilities = received;
            const uint16_t active = negotiatedCapabilities();
            if (active) addLog("Enhanced link active.");
            break;
        }
        case kMsgGenomeChunk: {
            if (length != 14 || data[1] >= 5) return;
            const uint8_t chunk = data[1];
            memcpy(opponentGenomeCode_ + chunk * 12, data + 2, 12);
            opponentGenomeChunks_ |= 1u << chunk;
            if (opponentGenomeCodeAvailable()) {
                opponentGenomeCode_[60] = '\0';
                addLog("Genome received.");
            }
            break;
        }
        default:
            break;
    }
}

void FamiliarBattleService::submitMove(FamiliarBattleMove move) {
    if (state_ != FamiliarBattleState::Battling || myMoveSubmitted_) return;
    myMove_ = move;
    myMoveSubmitted_ = true;
    sendMove(move);
    if (move == FamiliarBattleMove::Flee) {
        addLog("You fled!");
        concludeBattle(FamiliarBattleOutcome::Fled);
        return;
    }
    resolveTurnIfReady();
}

void FamiliarBattleService::logAction(bool actorIsMe, const BattleRulesActionResult& action) {
    if (!action.acted) return;  // Defend/Flee, or pre-empted by a Speed win -- no roll, no log.
    if (action.missed) {
        addLog(String(actorIsMe ? "You" : "Opp") + ": SPECIAL MISSED");
        return;
    }
    addLog(String(actorIsMe ? "You" : "Opp") + ": " +
          (action.special ? "SPECIAL" : "ATTACK") + " -" + String(action.damage) + " dmg");
}

void FamiliarBattleService::resolveTurnIfReady() {
    if (!(myMoveSubmitted_ && opponentMoveSubmitted_)) return;

    BattleRulesCombatant me;
    me.playerId = myPlayerId_;
    me.attack = myAttack_;
    me.defense = myDefense_;
    me.capabilities = toRulesCapabilities(myCapabilities_);
    me.move = static_cast<BattleRulesMove>(myMove_);

    BattleRulesCombatant opp;
    opp.playerId = opponent_.playerId;
    opp.attack = opponentAttack_;
    opp.defense = opponentDefense_;
    opp.capabilities = toRulesCapabilities(opponent_.capabilities);
    opp.move = static_cast<BattleRulesMove>(opponentMove_);

    const BattleRulesTurnResult result = resolveBattleTurn(
        me, myHp_, opp, opponentHp_, negotiatedCapabilities(), prngState_);
    myHp_ = result.hpA;
    opponentHp_ = result.hpB;

    // Log in the order the actions actually happened, matching resolveBattleTurn's
    // own resolution order rather than always "me" first.
    if (result.aActedFirst) {
        logAction(true, result.a);
        logAction(false, result.b);
    } else {
        logAction(false, result.b);
        logAction(true, result.a);
    }

    ++turnNumber_;
    myMoveSubmitted_ = false;
    opponentMoveSubmitted_ = false;

    if (myHp_ == 0 || opponentHp_ == 0) {
        concludeBattle(opponentHp_ == 0 ? FamiliarBattleOutcome::Victory
                                        : FamiliarBattleOutcome::Defeat);
    }
}

void FamiliarBattleService::concludeBattle(FamiliarBattleOutcome outcome) {
    outcome_ = outcome;
    state_ = FamiliarBattleState::Result;
    status_ = "Battle over";
}

void FamiliarBattleService::addLog(const String& line) {
    log_.push_back(line);
    if (log_.size() > 40) log_.erase(log_.begin());
}
