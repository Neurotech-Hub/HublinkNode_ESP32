#include "Hublink.h"

Hublink::Hublink(uint8_t chipSelect, uint32_t clockFrequency) : cs(chipSelect), clkFreq(clockFrequency),
                                                                piReadyForFilenames(false), deviceConnected(false),
                                                                currentFileName(""), allFilesSent(false),
                                                                watchdogTimer(0), sendFilenames(false),
                                                                defaultServerCallbacks(nullptr),
                                                                defaultFilenameCallbacks(nullptr),
                                                                defaultGatewayCallbacks(nullptr)
{
}

bool Hublink::init(String defaultAdvName, bool allowOverride)
{
    if (defaultServerCallbacks == nullptr)
    {
        defaultServerCallbacks = new DefaultServerCallbacks(this);
        if (defaultServerCallbacks == nullptr)
        {
            Serial.println("Failed to allocate server callbacks");
            return false;
        }
    }

    if (defaultFilenameCallbacks == nullptr)
    {
        defaultFilenameCallbacks = new DefaultFilenameCallback(this);
        if (defaultFilenameCallbacks == nullptr)
        {
            Serial.println("Failed to allocate filename callbacks");
            return false;
        }
    }

    if (defaultGatewayCallbacks == nullptr)
    {
        defaultGatewayCallbacks = new DefaultGatewayCallback(this);
        if (defaultGatewayCallbacks == nullptr)
        {
            Serial.println("Failed to allocate gateway callbacks");
            return false;
        }
    }

    if (!setBLECallbacks())
    {
        Serial.println("Failed to set BLE callbacks");
        return false;
    }

    if (!initSD())
    {
        Serial.println("✗ SD Card.");
        return false;
    }
    Serial.println("✓ SD Card.");

    // Initialize BLE stack and services
    metaJson = readMetaJson();
    // Set the advertising name based on configuration
    if (allowOverride && configuredAdvName.length() > 0)
    {
        advName = configuredAdvName;
    }
    else
    {
        advName = defaultAdvName;
    }

    BLEDevice::init(advName.c_str());
    pServer = BLEDevice::createServer();
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // Create characteristics
    pFilenameCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_FILENAME,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_INDICATE);
    pFilenameCharacteristic->addDescriptor(new BLE2902());

    pFileTransferCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_FILETRANSFER,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_INDICATE);
    pFileTransferCharacteristic->addDescriptor(new BLE2902());

    pConfigCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_GATEWAY,
        BLECharacteristic::PROPERTY_WRITE);

    pNodeCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_NODE,
        BLECharacteristic::PROPERTY_READ);

    pService->start();
    pNodeCharacteristic->setValue(metaJson.c_str());

    Serial.printf("Advertising name: %s, Connect every: %d, Connect for: %d\n",
                  advName.c_str(), bleConnectEvery, bleConnectFor);

    lastHublinkMillis = millis();
    return true;
}

String Hublink::readMetaJson()
{
    if (!initSD())
    {
        Serial.println("Failed to initialize SD card when reading meta.json");
        return "";
    }

    File configFile = SD.open(META_JSON_PATH, FILE_READ);
    if (!configFile)
    {
        Serial.println("No meta.json file found");
        return "";
    }

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error)
    {
        Serial.print("Failed to parse meta.json: ");
        Serial.println(error.c_str());
        return "";
    }

    JsonObject hublink = doc["hublink"];
    String content = "";

    // Parse configuration
    if (hublink.containsKey("advertise"))
    {
        configuredAdvName = hublink["advertise"].as<String>();
    }

    if (hublink.containsKey("advertise_every") && hublink.containsKey("advertise_for"))
    {
        uint32_t newEvery = hublink["advertise_every"];
        uint32_t newFor = hublink["advertise_for"];

        if (newEvery > 0 && newFor > 0)
        {
            bleConnectEvery = newEvery;
            bleConnectFor = newFor;
            Serial.printf("Updated intervals: every=%d, for=%d\n", bleConnectEvery, bleConnectFor);
        }
    }

    if (hublink.containsKey("disable"))
    {
        disable = hublink["disable"].as<bool>();
        Serial.printf("BLE disable flag set to: %s\n", disable ? "true" : "false");
    }

    // Convert the hublink object to a string for BLE characteristic
    serializeJson(doc, content);
    return content;
}

void Hublink::setCPUFrequency(uint32_t freq_mhz)
{
    setCpuFrequencyMhz(freq_mhz);
}

void Hublink::startAdvertising()
{
    resetBLEState();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->setMinInterval(0x20);
    pAdvertising->setMaxInterval(0x40);

    pAdvertising->start();
    delay(10); // let the BLE stack start
}

void Hublink::stopAdvertising()
{
    BLEDevice::getAdvertising()->stop();
    delay(10); // let the BLE stack stop
}

// Use SD.begin(cs, SPI, clkFreq) whenever SD functions are needed in this way:
bool Hublink::initSD()
{
    return SD.begin(cs, SPI, clkFreq);
}

bool Hublink::setBLECallbacks(BLEServerCallbacks *newServerCallbacks,
                              BLECharacteristicCallbacks *newFilenameCallbacks,
                              BLECharacteristicCallbacks *newConfigCallbacks)
{
    if (!pServer || !pFilenameCharacteristic || !pConfigCharacteristic)
    {
        return false;
    }

    if (newServerCallbacks != nullptr)
    {
        pServer->setCallbacks(newServerCallbacks);
    }

    if (newFilenameCallbacks != nullptr)
    {
        pFilenameCharacteristic->setCallbacks(newFilenameCallbacks);
    }

    if (newConfigCallbacks != nullptr)
    {
        pConfigCharacteristic->setCallbacks(newConfigCallbacks);
    }

    return true;
}

void Hublink::updateConnectionStatus()
{
    // Watchdog timer
    if (deviceConnected && (millis() - watchdogTimer > WATCHDOG_TIMEOUT_MS))
    {
        Serial.println("Hublink node timeout detected, disconnecting...");
        pServer->disconnect(pServer->getConnId());
        delay(500); // Give BLE stack time to clean up
    }

    // Handle file transfers when connected
    if (deviceConnected && !currentFileName.isEmpty())
    {
        Serial.print("Requested file: ");
        Serial.println(currentFileName);
        handleFileTransfer(currentFileName);
        currentFileName = "";
    }

    // once sendFilenames is true
    if (deviceConnected && currentFileName.isEmpty() && !allFilesSent && sendFilenames)
    {
        updateMtuSize();
        Serial.print("MTU Size (negotiated): ");
        Serial.println(mtuSize);
        Serial.println("Sending filenames...");
        sendAvailableFilenames();
    }
}

void Hublink::sendAvailableFilenames()
{
    if (!initSD())
    {
        Serial.println("Failed to initialize SD card");
        return;
    }

    File root = SD.open("/");
    String accumulatedFileInfo = ""; // Accumulate file info here
    while (deviceConnected)
    {
        watchdogTimer = millis(); // Reset watchdog timer
        File entry = root.openNextFile();
        if (!entry)
        {
            Serial.println("All filenames sent.");
            int index = 0;
            while (index < accumulatedFileInfo.length() && deviceConnected)
            {
                watchdogTimer = millis();
                String chunk = accumulatedFileInfo.substring(index, index + mtuSize);
                pFilenameCharacteristic->setValue(chunk.c_str());
                pFilenameCharacteristic->indicate();
                index += mtuSize;
            }
            // Send "EOF" as a separate indication to signal the end
            pFilenameCharacteristic->setValue("EOF");
            pFilenameCharacteristic->indicate();
            allFilesSent = true;
            break;
        }

        String fileName = entry.name();
        if (isValidFile(fileName))
        {
            Serial.println(fileName);
            String fileInfo = fileName + "|" + String(entry.size());
            if (!accumulatedFileInfo.isEmpty())
            {
                accumulatedFileInfo += ";"; // Separate entries with a semicolon
            }
            accumulatedFileInfo += fileInfo;
        }
    }
    root.close();
}

void Hublink::handleFileTransfer(String fileName)
{
    // Begin SD communication with updated parameters
    if (!initSD())
    {
        Serial.println("Failed to initialize SD card");
        return;
    }

    File file = SD.open("/" + fileName);
    if (!file)
    {
        Serial.printf("Failed to open file: %s\n", fileName.c_str());
        return;
    }

    uint8_t buffer[mtuSize];
    while (file.available() && deviceConnected)
    {
        watchdogTimer = millis();
        int bytesRead = file.read(buffer, mtuSize);
        if (bytesRead > 0)
        {
            pFileTransferCharacteristic->setValue(buffer, bytesRead);
            pFileTransferCharacteristic->indicate();
        }
        else
        {
            Serial.println("Error reading from file.");
            break;
        }
    }
    pFileTransferCharacteristic->setValue("EOF");
    pFileTransferCharacteristic->indicate();
    file.close();
    Serial.println("File transfer complete.");
}

bool Hublink::isValidFile(String fileName)
{
    // Exclude files that start with a dot
    if (fileName.startsWith("."))
    {
        return false;
    }

    // Convert filename to lowercase
    String lowerFileName = fileName;
    lowerFileName.toLowerCase();

    // Check for valid extensions
    for (const auto &ext : validExtensions) // Use range-based for loop
    {
        if (lowerFileName.endsWith(ext))
        {
            return true;
        }
    }
    return false;
}

void Hublink::onConnect()
{
    Serial.println("Hublink node connected.");
    deviceConnected = true;
    watchdogTimer = millis();
    BLEDevice::setMTU(NEGOTIATE_MTU_SIZE);
}

void Hublink::onDisconnect()
{
    Serial.println("Hublink node disconnected.");
    deviceConnected = false;
}

void Hublink::resetBLEState()
{
    deviceConnected = false;
    piReadyForFilenames = false;
    currentFileName = "";
    allFilesSent = false;
    sendFilenames = false;
}

void Hublink::updateMtuSize()
{
    mtuSize = BLEDevice::getMTU() - MTU_HEADER_SIZE;
}

String Hublink::parseGateway(BLECharacteristic *pCharacteristic, const String &key)
{
    std::string value = pCharacteristic->getValue().c_str();
    if (value.empty())
    {
        Serial.println("Config: None");
        return "";
    }

    String configStr = String(value.c_str());
    Serial.println("Config: Parsing JSON: " + configStr);

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, configStr);

    if (error)
    {
        Serial.print("Config: JSON parsing failed: ");
        Serial.println(error.c_str());
        return "";
    }

    if (!doc.containsKey(key))
    {
        return "";
    }

    return doc[key].as<String>();
}

void Hublink::sleep(uint64_t milliseconds)
{
    // Print memory stats before sleep
    // Serial.printf("Before sleep - Free heap: %d bytes, Min free heap: %d bytes\n",
    //               ESP.getFreeHeap(),
    //               ESP.getMinFreeHeap());

    esp_sleep_enable_timer_wakeup(milliseconds * 1000); // Convert to microseconds
    esp_light_sleep_start();

    // Print memory stats after sleep
    // Serial.printf("After sleep  - Free heap: %d bytes, Min free heap: %d bytes\n",
    //               ESP.getFreeHeap(),
    //               ESP.getMinFreeHeap());
}

void Hublink::doBLE()
{
    startAdvertising();
    unsigned long subLoopStartTime = millis();
    bool didConnect = false;

    while ((millis() - subLoopStartTime < bleConnectFor * 1000 && !didConnect) || deviceConnected)
    {
        updateConnectionStatus();
        didConnect |= deviceConnected;
        delay(100); // This existing delay is good for BLE operations
    }
    stopAdvertising();
}

void Hublink::sync()
{
    unsigned long currentTime = millis();
    if (!disable && currentTime - lastHublinkMillis >= bleConnectEvery * 1000)
    {
        Serial.println("Hublink started advertising... ");
        doBLE();
        Serial.println("Done advertising.");
        lastHublinkMillis = currentTime;
    }
}

// Add destructor to clean up callback instances
Hublink::~Hublink()
{
    if (defaultServerCallbacks != nullptr)
    {
        delete defaultServerCallbacks;
        defaultServerCallbacks = nullptr;
    }

    if (defaultFilenameCallbacks != nullptr)
    {
        delete defaultFilenameCallbacks;
        defaultFilenameCallbacks = nullptr;
    }

    if (defaultGatewayCallbacks != nullptr)
    {
        delete defaultGatewayCallbacks;
        defaultGatewayCallbacks = nullptr;
    }
}
