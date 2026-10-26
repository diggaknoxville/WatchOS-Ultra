#include "UsbMscManager.h"
#include "NavEngine.h"
#include "RadioLoRa.h"
#include "DisplayEngine.h"
#include "ui/Icons.h"
#include <USB.h>
#include <USBMSC.h>
#include <SD.h>

static USBMSC sMsc;
static bool sMscInited = false;
static bool sMscEnabled = true;
static volatile bool sMediaMounted = false;
static volatile bool sDirtyWrite = false;

static int32_t onMscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;
    if (!NavEngine::status().sdReady) return -1;
    if (!NavEngine::lockSd(200)) return -1;
    uint32_t secSize = SD.sectorSize();
    if (!secSize) {
        NavEngine::unlockSd();
        return -1;
    }
    uint32_t numBlocks = bufsize / secSize;
    for (uint32_t i = 0; i < numBlocks; i++) {
        if (!SD.readRAW((uint8_t*)buffer + (i * secSize), lba + i)) {
            NavEngine::unlockSd();
            return -1;
        }
    }
    NavEngine::unlockSd();
    return bufsize;
}

static int32_t onMscWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)offset;
    if (!NavEngine::status().sdWritable) return -1;
    if (!NavEngine::lockSd(300)) return -1;
    uint32_t secSize = SD.sectorSize();
    if (!secSize) {
        NavEngine::unlockSd();
        return -1;
    }
    uint32_t numBlocks = bufsize / secSize;
    for (uint32_t i = 0; i < numBlocks; i++) {
        if (!SD.writeRAW((uint8_t*)buffer + (i * secSize), lba + i)) {
            NavEngine::unlockSd();
            return -1;
        }
    }
    NavEngine::unlockSd();
    sDirtyWrite = true;
    return bufsize;
}

static bool onMscStartStop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    Serial.printf("[USB] MSC StartStop: start=%d eject=%d\n", (int)start, (int)load_eject);
    sMediaMounted = start && !load_eject;
    return true;
}

bool UsbMscManager::init() {
    if (sMscInited) return true;
    if (!NavEngine::status().sdReady) return false;

    uint32_t totalSectors = SD.numSectors();
    uint16_t sectorSize = SD.sectorSize();
    if (totalSectors == 0 || sectorSize == 0) return false;

    sMsc.vendorID("WATCHOS");
    sMsc.productID("MICROSD DRIVE");
    sMsc.productRevision("1.0");
    sMsc.mediaPresent(true);
    sMsc.isWritable(NavEngine::status().sdWritable);
    sMsc.onRead(onMscRead);
    sMsc.onWrite(onMscWrite);
    sMsc.onStartStop(onMscStartStop);

    if (sMsc.begin(totalSectors, sectorSize)) {
        sMscInited = true;
        Serial.printf("[USB] MSC initialized: %lu sectors x %u bytes (%lu MB)\n",
                      (unsigned long)totalSectors, (unsigned)sectorSize,
                      (unsigned long)(totalSectors / (1024UL * 1024UL / sectorSize)));
        return true;
    }
    return false;
}

void UsbMscManager::loop(unsigned long now) {
    (void)now;
    if (!sMscInited && NavEngine::status().sdReady) {
        init();
    }
    if (sDirtyWrite && !sMediaMounted) {
        sDirtyWrite = false;
        Serial.println("[USB] MSC write completed, reloading maps & channels...");
        NavEngine::reloadMap();
        RadioLoRa::reloadChannelsFromSd();
        DisplayEngine::showNotification("USB-Laufwerk", "Daten auf SD aktualisiert", 3500, ICO_UPDATE);
    }
}

bool UsbMscManager::isAvailable() { return sMscInited; }
bool UsbMscManager::isMounted() { return sMediaMounted; }
bool UsbMscManager::isConnected() { return sMscInited; }
void UsbMscManager::setEnabled(bool en) {
    sMscEnabled = en;
    sMsc.mediaPresent(en && NavEngine::status().sdReady);
}
bool UsbMscManager::isEnabled() { return sMscEnabled; }
uint32_t UsbMscManager::getSectorCount() {
    return NavEngine::status().sdReady ? SD.numSectors() : 0;
}

