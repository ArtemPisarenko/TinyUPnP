/*
 * TinyUPnP.h - Library for creating UPnP rules automatically in your router.
 * Created by Ofek Pearl, September 2017.
*/

#if defined(ESP8266)
    #include <ESP8266WiFi.h>
#else
    #include <WiFi.h>
#endif

#include "TinyUPnP.h"

#ifdef UPNP_DEBUG
#define debugPrint(...) Serial.print( __VA_ARGS__ )
#define debugPrintln(...) Serial.println( __VA_ARGS__ )
#else
#define debugPrint(...)
#define debugPrintln(...)
#endif

IPAddress ipMulti(239, 255, 255, 250);  // multicast address for SSDP
IPAddress connectivityTestIp(64, 233, 187, 99);  // Google
IPAddress ipNull(0, 0, 0, 0);  // indication to update rules when the IP of the device changes

char packetBuffer[UPNP_UDP_TX_PACKET_MAX_SIZE];  // buffer to hold incoming packet
char responseBuffer[UDP_TX_RESPONSE_MAX_SIZE];

char body_tmp[1200];
char integer_string[32];

SOAPAction SOAPActionGetSpecificPortMappingEntry = {.name = "GetSpecificPortMappingEntry"};
SOAPAction SOAPActionDeletePortMapping = {.name = "DeletePortMapping"};

// timeoutMs - timeout in milli seconds for the operations of this class, 0 for blocking operation
TinyUPnP::TinyUPnP(unsigned long timeoutMs = 20000) {
    _timeoutMs = timeoutMs;
    _lastUpdateTime = 0;
    _consequtiveFails = 0;
    _headRuleNode = NULL;
    clearGatewayInfo(&_gwInfo);

    debugPrint(F("UPNP_UDP_TX_PACKET_MAX_SIZE="));
    debugPrintln(String(UPNP_UDP_TX_PACKET_MAX_SIZE));
    debugPrint(F("UDP_TX_RESPONSE_MAX_SIZE="));
    debugPrintln(String(UDP_TX_RESPONSE_MAX_SIZE));
}

TinyUPnP::~TinyUPnP() {
}

void TinyUPnP::addPortMappingConfig(IPAddress ruleIP, int rulePort, String ruleProtocol, int ruleLeaseDuration, String ruleFriendlyName) {
	addPortMappingConfig(ruleIP, rulePort, rulePort, ruleProtocol, ruleLeaseDuration, ruleFriendlyName);
}

void TinyUPnP::addPortMappingConfig(IPAddress ruleIP, int ruleInternalPort, int ruleExternalPort, String ruleProtocol, int ruleLeaseDuration, String ruleFriendlyName) {
    static int index = 0;
    upnpRule *newUpnpRule = new upnpRule();
    newUpnpRule->index = index++;
    newUpnpRule->internalAddr = (ruleIP == WiFi.localIP()) ? ipNull : ruleIP;  // for automatic IP change handling
    newUpnpRule->internalPort = ruleInternalPort;
    newUpnpRule->externalPort = ruleExternalPort;
    newUpnpRule->leaseDuration = ruleLeaseDuration;
    newUpnpRule->protocol = ruleProtocol;
    newUpnpRule->devFriendlyName = ruleFriendlyName;

    // linked list insert
    upnpRuleNode *newUpnpRuleNode = new upnpRuleNode();
    newUpnpRuleNode->upnpRule = newUpnpRule;
    newUpnpRuleNode->next = NULL;
    
    if (_headRuleNode == NULL) {
        _headRuleNode = newUpnpRuleNode;
    } else {
        upnpRuleNode *currNode = _headRuleNode;
        while (currNode->next != NULL) {
            currNode = currNode->next;
        }
        currNode->next = newUpnpRuleNode;
    }
}

portMappingResult TinyUPnP::commitPortMappings() {
    if (!_headRuleNode) {
        debugPrintln(F("ERROR: No UPnP port mapping was set."));
        return EMPTY_PORT_MAPPING_CONFIG;
    }

    unsigned long startTime = millis();

    // verify WiFi is connected
    if (!testConnectivity(startTime)) {
        debugPrintln(F("ERROR: not connected to WiFi, cannot continue"));
        return NETWORK_ERROR;
    }

    // get all the needed IGD information using SSDP if we don't have it already
    if (!isGatewayInfoValid(&_gwInfo)) {
        getGatewayInfo(&_gwInfo, startTime);
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrintln(F("ERROR: Invalid router info, cannot continue"));
            _wifiClient.stop();
            return NETWORK_ERROR;
        }
        delay(1000);  // longer delay to allow more time for the router to update its rules
    }

    debugPrint(F("port ["));
    debugPrint(String(_gwInfo.port));
    debugPrint(F("] actionPort ["));
    debugPrint(String(_gwInfo.actionPort));
    debugPrintln(F("]"));

    // double verify gateway information is valid
    if (!isGatewayInfoValid(&_gwInfo)) {
        debugPrintln(F("ERROR: Invalid router info, cannot continue"));
        return NETWORK_ERROR;
    }

    if (_gwInfo.port != _gwInfo.actionPort) {
        // in this case we need to connect to a different port
        debugPrintln(F("Connection port changed, disconnecting from IGD"));
        _wifiClient.stop();
    }

    bool allPortMappingsAlreadyExist = true;  // for debug
    int addedPortMappings = 0;  // for debug
    upnpRuleNode *currNode = _headRuleNode;
    while (currNode != NULL) {
        debugPrint(F("Verify port mapping for rule ["));
        debugPrint(currNode->upnpRule->devFriendlyName);
        debugPrintln(F("]"));
        bool currPortMappingAlreadyExists = true;  // for debug
        // TODO: since verifyPortMapping connects to the IGD then addPortMappingEntry can skip it
        if (!verifyPortMapping(&_gwInfo, currNode->upnpRule)) {
            // need to add the port mapping
            currPortMappingAlreadyExists = false;
            allPortMappingsAlreadyExist = false;
            if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
                debugPrintln(F("Timeout expired while trying to add a port mapping"));
                _wifiClient.stop();
                return TIMEOUT;
            }

            addPortMappingEntry(&_gwInfo, currNode->upnpRule);

            int tries = 0;
            while (tries <= 3) {
                delay(2000);  // longer delay to allow more time for the router to update its rules
                if (verifyPortMapping(&_gwInfo, currNode->upnpRule)) {
                    break;
                }
                tries++;
            }

            if (tries > 3) {
                _wifiClient.stop();
                return VERIFICATION_FAILED;
            }
        }

        if (!currPortMappingAlreadyExists) {
            addedPortMappings++;
            debugPrint(F("Port mapping ["));
            debugPrint(currNode->upnpRule->devFriendlyName);
            debugPrintln(F("] was added"));
        }

        currNode = currNode->next;
    }

    _wifiClient.stop();
    
    if (allPortMappingsAlreadyExist) {
        debugPrintln(F("All port mappings were already found in the IGD, not doing anything"));
        return ALREADY_MAPPED;
    } else {
        // addedPortMappings is at least 1 here
        if (addedPortMappings > 1) {
            debugPrint(addedPortMappings);
            debugPrintln(F(" UPnP port mappings were added"));
        } else {
            debugPrintln(F("One UPnP port mapping was added"));
        }
    }

    return SUCCESS;
}

boolean TinyUPnP::getGatewayInfo(gatewayInfo *deviceInfo, long startTime) {
    while (!connectUDP()) {
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrint(F("Timeout expired while connecting UDP"));
            _udpClient.stop();
            return false;
        }
        delay(500);
        debugPrint(".");
    }
    debugPrintln("");  // \n
    
    broadcastMSearch();
    IPAddress gatewayIP = WiFi.gatewayIP();

    debugPrint(F("Gateway IP ["));
    debugPrint(gatewayIP.toString());
    debugPrintln(F("]"));

    ssdpDevice* ssdpDevice_ptr = NULL;
    while ((ssdpDevice_ptr = waitForUnicastResponseToMSearch(gatewayIP)) == NULL) {
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrintln(F("Timeout expired while waiting for the gateway router to respond to M-SEARCH message"));
            _udpClient.stop();
            return false;
        }
        delay(1);
    }

    deviceInfo->host = ssdpDevice_ptr->host;
    deviceInfo->port = ssdpDevice_ptr->port;
    deviceInfo->path = ssdpDevice_ptr->path;
    // the following is the default and may be overridden if URLBase tag is specified
    deviceInfo->actionPort = ssdpDevice_ptr->port;

    delete ssdpDevice_ptr;

    // close the UDP connection
    _udpClient.stop();

    // connect to IGD (TCP connection)
    while (!connectToIGD(deviceInfo->host, deviceInfo->port)) {
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrintln(F("Timeout expired while trying to connect to the IGD"));
            _wifiClient.stop();
            return false;
        }
        delay(500);
    }
    
    // get event urls from the gateway IGD
    while (!getIGDEventURLs(deviceInfo)) {
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrintln(F("Timeout expired while adding a new port mapping"));
            _wifiClient.stop();
            return false;
        }
        delay(500);
    }

    return true;
}

void TinyUPnP::clearGatewayInfo(gatewayInfo *deviceInfo) {
    deviceInfo->host = IPAddress(0, 0, 0, 0);
    deviceInfo->port = 0;
    deviceInfo->path = "";
    deviceInfo->actionPort = 0;
    deviceInfo->actionPath = "";
    deviceInfo->serviceTypeName = "";
}

boolean TinyUPnP::isGatewayInfoValid(gatewayInfo *deviceInfo) {
    debugPrint(F("isGatewayInfoValid ["));
    debugPrint(deviceInfo->host.toString());
    debugPrint(F("] port ["));
    debugPrint(String(deviceInfo->port));
    debugPrint(F("] path ["));
    debugPrint(deviceInfo->path);
    debugPrint(F("] actionPort ["));
    debugPrint(String(deviceInfo->actionPort));
    debugPrint(F("] actionPath ["));
    debugPrint(deviceInfo->actionPath);
    debugPrint(F("] serviceTypeName ["));
    debugPrint(deviceInfo->serviceTypeName);
    debugPrintln(F("]"));

    if (deviceInfo->host == IPAddress(0, 0, 0, 0)
        || deviceInfo->port == 0
        || deviceInfo->path.length() == 0
        || deviceInfo->actionPort == 0) {
        debugPrintln(F("Gateway info is not valid"));
        return false;
    }

    debugPrintln(F("Gateway info is valid"));
    return true;
}

portMappingResult TinyUPnP::updatePortMappings(unsigned long intervalMs, callback_function fallback) {
    if (millis() - _lastUpdateTime >= intervalMs) {
        debugPrintln(F("Updating port mapping"));

        // fallback
        if (_consequtiveFails >= MAX_NUM_OF_UPDATES_WITH_NO_EFFECT) {
            debugPrint(F("ERROR: Too many times with no effect on updatePortMappings. Current number of fallbacks times ["));
            debugPrint(String(_consequtiveFails));
            debugPrintln(F("]"));

            _consequtiveFails = 0;
            clearGatewayInfo(&_gwInfo);
            if (fallback != NULL) {
                debugPrintln(F("Executing fallback method"));
                fallback();
            }

            return TIMEOUT;
        }

        // } else if (_consequtiveFails > 300) {
        // 	ESP.restart();  // should test as last resort
        // 	return;
        // }

        portMappingResult result = commitPortMappings();

        if (result == SUCCESS || result == ALREADY_MAPPED) {
            _lastUpdateTime = millis();
            _wifiClient.stop();
            _consequtiveFails = 0;
            return result;
        } else {
            _lastUpdateTime += intervalMs / 2;  // delay next try
            debugPrint(F("ERROR: While updating UPnP port mapping. Failed with error code ["));
            debugPrint(String(result));
            debugPrintln(F("]"));
            _wifiClient.stop();
            _consequtiveFails++;
            return result;
        }
    }

    _wifiClient.stop();
    return NOP;  // no need to check yet
}

boolean TinyUPnP::testConnectivity(unsigned long startTime) {
    debugPrint(F("Testing WiFi connection for ["));
    debugPrint(WiFi.localIP().toString());
    debugPrint("]");
    while (WiFi.status() != WL_CONNECTED) {
        if (_timeoutMs > 0 && startTime > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrint(F(" ==> Timeout expired while verifying WiFi connection"));
            _wifiClient.stop();
            return false;
        }
        delay(200);
        debugPrint(".");
    }
    debugPrintln(F(" ==> GOOD"));  // \n

    debugPrint(F("Testing internet connection"));
    _wifiClient.connect(connectivityTestIp, 80);
    while (!_wifiClient.connected()) {
        if (startTime + TCP_CONNECTION_TIMEOUT_MS > millis()) {
            debugPrintln(F(" ==> BAD"));
            _wifiClient.stop();
            return false;
        }
    }

    debugPrintln(F(" ==> GOOD"));
    _wifiClient.stop();
    return true;
}

boolean TinyUPnP::verifyPortMapping(gatewayInfo *deviceInfo, upnpRule *rule_ptr) {
    if (!applyActionOnSpecificPortMapping(&SOAPActionGetSpecificPortMappingEntry ,deviceInfo, rule_ptr)) {
        return false;
    }

    debugPrintln(F("verifyPortMapping called"));
    
    // TODO: extract the current lease duration and return it instead of a boolean
    boolean isSuccess = false;
    boolean detectedChangedIP = false;
    while (_wifiClient.available()) {
        String line = _wifiClient.readStringUntil('\r');
        debugPrint(line);
        if (line.indexOf(F("errorCode")) >= 0) {
            isSuccess = false;
            // flush response and exit loop
            while (_wifiClient.available()) {
                line = _wifiClient.readStringUntil('\r');
                debugPrint(line);
            }
            continue;
        }

        if (line.indexOf(F("NewInternalClient")) >= 0) {
            String content = getTagContent(line, F("NewInternalClient"));
            if (content.length() > 0) {
                IPAddress ipAddressToVerify = (rule_ptr->internalAddr == ipNull) ? WiFi.localIP() : rule_ptr->internalAddr;
                if (content == ipAddressToVerify.toString()) {
                    isSuccess = true;
                } else {
                    detectedChangedIP = true;
                }
            }
        }
    }

    debugPrintln("");  // \n

    _wifiClient.stop();

    if (isSuccess) {
        debugPrintln(F("Port mapping found in IGD"));
    } else if (detectedChangedIP) {
        debugPrintln(F("Detected a change in IP"));
        removeAllPortMappingsFromIGD();
    } else {
        debugPrintln(F("Could not find port mapping in IGD"));
    }

    return isSuccess;
}

boolean TinyUPnP::deletePortMapping(gatewayInfo *deviceInfo, upnpRule *rule_ptr) {
    if (!applyActionOnSpecificPortMapping(&SOAPActionDeletePortMapping ,deviceInfo, rule_ptr)) {
        return false;
    }
    
    boolean isSuccess = false;
    while (_wifiClient.available()) {
        String line = _wifiClient.readStringUntil('\r');
        debugPrint(line);
        if (line.indexOf(F("errorCode")) >= 0) {
            isSuccess = false;
            // flush response and exit loop
            while (_wifiClient.available()) {
                line = _wifiClient.readStringUntil('\r');
                debugPrint(line);
            }
            continue;
        }
        if (line.indexOf(F("DeletePortMappingResponse")) >= 0) { 
            isSuccess = true;
        }
    }

    return isSuccess;
}

boolean TinyUPnP::applyActionOnSpecificPortMapping(SOAPAction *soapAction, gatewayInfo *deviceInfo, upnpRule *rule_ptr) {
    debugPrint(F("Apply action ["));
    debugPrint(soapAction->name);
    debugPrint(F("] on port mapping ["));
    debugPrint(rule_ptr->devFriendlyName);
    debugPrintln(F("]"));

    // connect to IGD (TCP connection) again, if needed, in case we got disconnected after the previous query
    unsigned long timeout = millis() + TCP_CONNECTION_TIMEOUT_MS;
    if (!_wifiClient.connected()) {
        while (!connectToIGD(deviceInfo->host, deviceInfo->actionPort)) {
            if (millis() > timeout) {
                debugPrintln(F("Timeout expired while trying to connect to the IGD"));
                _wifiClient.stop();
                return false;
            }
            delay(500);
        }
    }

    strcpy_P(body_tmp, PSTR("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n<s:Body>\r\n<u:"));
    strcat_P(body_tmp, soapAction->name);
    strcat_P(body_tmp, PSTR(" xmlns:u=\""));
    strcat_P(body_tmp, deviceInfo->serviceTypeName.c_str());
    strcat_P(body_tmp, PSTR("\">\r\n<NewRemoteHost></NewRemoteHost>\r\n<NewExternalPort>"));
    sprintf(integer_string, "%d", rule_ptr->externalPort);
    strcat_P(body_tmp, integer_string);
    strcat_P(body_tmp, PSTR("</NewExternalPort>\r\n<NewProtocol>"));
    strcat_P(body_tmp, rule_ptr->protocol.c_str());
    strcat_P(body_tmp, PSTR("</NewProtocol>\r\n</u:"));
    strcat_P(body_tmp, soapAction->name);
    strcat_P(body_tmp, PSTR(">\r\n</s:Body>\r\n</s:Envelope>\r\n"));

    sprintf(integer_string, "%d", strlen(body_tmp));

    _wifiClient.print(F("POST "));

    _wifiClient.print(deviceInfo->actionPath);
    _wifiClient.println(F(" HTTP/1.1"));
    _wifiClient.println(F("Connection: close"));
    _wifiClient.println(F("Content-Type: text/xml; charset=\"utf-8\""));
    _wifiClient.println("Host: " + deviceInfo->host.toString() + ":" + String(deviceInfo->actionPort));
    _wifiClient.print(F("SOAPAction: \""));
    _wifiClient.print(deviceInfo->serviceTypeName);
    _wifiClient.print(F("#"));
    _wifiClient.print(soapAction->name);
    _wifiClient.println(F("\""));
    _wifiClient.print(F("Content-Length: "));
    _wifiClient.println(integer_string);
    _wifiClient.println();

    _wifiClient.println(body_tmp);
    _wifiClient.println();

    debugPrintln(body_tmp);

    timeout = millis() + TCP_CONNECTION_TIMEOUT_MS;
    while (_wifiClient.available() == 0) {
        if (millis() > timeout) {
            debugPrintln(F("TCP connection timeout while retrieving port mappings"));
            _wifiClient.stop();
            // TODO: in this case we might not want to add the ports right away
            // might want to try again or only start adding the ports after we definitely
            // did not see them in the router list
            return false;
        }
    }
    return true;
}

void TinyUPnP::removeAllPortMappingsFromIGD() {
    upnpRuleNode *currNode = _headRuleNode;
    while (currNode != NULL) {
        deletePortMapping(&_gwInfo, currNode->upnpRule);
        currNode = currNode->next;
    }
}

// a single try to connect UDP multicast address and port of UPnP (239.255.255.250 and 1900 respectively)
// this will enable receiving SSDP packets after the M-SEARCH multicast message will be broadcasted
boolean TinyUPnP::connectUDP() {
#if defined(ESP8266)
    if (_udpClient.beginMulticast(WiFi.localIP(), ipMulti, 0)) {
        return true;
    }
#else
    if (_udpClient.beginMulticast(ipMulti, UPNP_SSDP_PORT)) {
        return true;
    }
#endif

    debugPrintln(F("UDP connection failed"));
    return false;
}

// broadcast an M-SEARCH message to initiate messages from SSDP devices
// the router should respond to this message by a packet sent to this device's unicast addresss on the
// same UPnP port (1900)
void TinyUPnP::broadcastMSearch(bool isSsdpAll /*=false*/) {
    debugPrint(F("Sending M-SEARCH to ["));
    debugPrint(ipMulti.toString());
    debugPrint(F("] Port ["));
    debugPrint(String(UPNP_SSDP_PORT));
    debugPrintln(F("]"));

#if defined(ESP8266)
    _udpClient.beginPacketMulticast(ipMulti, UPNP_SSDP_PORT, WiFi.localIP());
#else
    uint8_t beginMulticastPacketRes = _udpClient.beginMulticastPacket();
    debugPrint(F("beginMulticastPacketRes ["));
    debugPrint(String(beginMulticastPacketRes));
    debugPrintln(F("]"));
#endif

    const char * const * deviceList = deviceListUpnp;
    if (isSsdpAll) {
        deviceList = deviceListSsdpAll;
    }

    for (int i = 0; deviceList[i]; i++) {
        strcpy_P(body_tmp, PSTR("M-SEARCH * HTTP/1.1\r\n"));
        strcat_P(body_tmp, PSTR("HOST: 239.255.255.250:"));
        sprintf(integer_string, "%d", UPNP_SSDP_PORT);
        strcat_P(body_tmp, integer_string);
        strcat_P(body_tmp, PSTR("\r\n"));
        strcat_P(body_tmp, PSTR("MAN: \"ssdp:discover\"\r\n"));
        strcat_P(body_tmp, PSTR("MX: 2\r\n"));  // allowed number of seconds to wait before replying to this M_SEARCH
        strcat_P(body_tmp, PSTR("ST: "));
        strcat_P(body_tmp, deviceList[i]);
        strcat_P(body_tmp, PSTR("\r\n"));
        strcat_P(body_tmp, PSTR("USER-AGENT: unix/5.1 UPnP/2.0 TinyUPnP/1.0\r\n"));
        strcat_P(body_tmp, PSTR("\r\n"));

        debugPrintln(body_tmp);
        size_t len = strlen(body_tmp);
        debugPrint(F("M-SEARCH packet length is ["));
        debugPrint(String(len));
        debugPrintln(F("]"));

#if defined(ESP8266)
        _udpClient.write(body_tmp);
#else
        _udpClient.print(body_tmp);
#endif
    
        int endPacketRes = _udpClient.endPacket();
        debugPrint(F("endPacketRes ["));
        debugPrint(String(endPacketRes));
        debugPrintln(F("]"));
    }

    debugPrintln(F("M-SEARCH packets sent"));
}

ssdpDeviceNode* TinyUPnP::listSsdpDevices() {
    if (_timeoutMs <= 0) {
        debugPrintln("Timeout must be set when initializing TinyUPnP to use this method, exiting.");
        return NULL;
    }

    unsigned long startTime = millis();
    while (!connectUDP()) {
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrint(F("Timeout expired while connecting UDP"));
            _udpClient.stop();
            return NULL;
        }
        delay(500);
        debugPrint(".");
    }
    debugPrintln("");  // \n
    
    broadcastMSearch(true);
    IPAddress gatewayIP = WiFi.gatewayIP();

    debugPrint(F("Gateway IP ["));
    debugPrint(gatewayIP.toString());
    debugPrintln(F("]"));

    ssdpDeviceNode *ssdpDeviceNode_head = NULL;
    ssdpDeviceNode *ssdpDeviceNode_tail = NULL;
    ssdpDeviceNode *ssdpDeviceNode_ptr = NULL;
    ssdpDevice *ssdpDevice_ptr = NULL;
    while (true) {
        ssdpDevice_ptr = waitForUnicastResponseToMSearch(ipNull);  // NULL will cause finding all SSDP device (not just the IGD)
        if (_timeoutMs > 0 && (millis() - startTime > _timeoutMs)) {
            debugPrintln(F("Timeout expired while waiting for the gateway router to respond to M-SEARCH message"));
            _udpClient.stop();
            delete ssdpDevice_ptr;
            break;
        }

        //ssdpDeviceToString(ssdpDevice_ptr);
        
        if (ssdpDevice_ptr != NULL) {
            if (ssdpDeviceNode_head == NULL) {
                ssdpDeviceNode_head = new ssdpDeviceNode();
                ssdpDeviceNode_head->ssdpDevice = ssdpDevice_ptr;
                ssdpDeviceNode_head->next = NULL;
                ssdpDeviceNode_tail = ssdpDeviceNode_head;
            } else {
                ssdpDeviceNode_ptr = new ssdpDeviceNode();
                ssdpDeviceNode_ptr->ssdpDevice = ssdpDevice_ptr;
                ssdpDeviceNode_tail->next = ssdpDeviceNode_ptr;
                ssdpDeviceNode_tail = ssdpDeviceNode_ptr;
            }
        }

        delay(5);
    }

    // close the UDP connection
    _udpClient.stop();

    // dedup SSDP devices fromt the list - O(n^2)
    ssdpDeviceNode_ptr = ssdpDeviceNode_head;
    while (ssdpDeviceNode_ptr != NULL) {
        ssdpDeviceNode *ssdpDeviceNodePrev_ptr = ssdpDeviceNode_ptr;
        ssdpDeviceNode *ssdpDeviceNodeCurr_ptr = ssdpDeviceNode_ptr->next;

        while (ssdpDeviceNodeCurr_ptr != NULL) {
            if (ssdpDeviceNodeCurr_ptr->ssdpDevice->host == ssdpDeviceNode_ptr->ssdpDevice->host
                && ssdpDeviceNodeCurr_ptr->ssdpDevice->port == ssdpDeviceNode_ptr->ssdpDevice->port
                && ssdpDeviceNodeCurr_ptr->ssdpDevice->path == ssdpDeviceNode_ptr->ssdpDevice->path) {
                // delete ssdpDeviceNode from the list
                ssdpDeviceNodePrev_ptr->next = ssdpDeviceNodeCurr_ptr->next;
                free(ssdpDeviceNodeCurr_ptr->ssdpDevice);
                free(ssdpDeviceNodeCurr_ptr);
                ssdpDeviceNodeCurr_ptr = ssdpDeviceNodePrev_ptr->next;
            } else {
                ssdpDeviceNodePrev_ptr = ssdpDeviceNodeCurr_ptr;
                ssdpDeviceNodeCurr_ptr = ssdpDeviceNodeCurr_ptr->next;
            }
        }
        ssdpDeviceNode_ptr = ssdpDeviceNode_ptr->next;
    }

    return ssdpDeviceNode_head;
}

// Assuming an M-SEARCH message was broadcaseted, wait for the response from the IGD (Internet Gateway Device)
// Note: the response from the IGD is sent back as unicast to this device
// Note: only gateway defined IGD response will be considered, the rest will be ignored
ssdpDevice* TinyUPnP::waitForUnicastResponseToMSearch(IPAddress gatewayIP) {
    int packetSize = _udpClient.parsePacket();

    // only continue if a packet is available
    if (packetSize <= 0) {
        return NULL;
    }

    IPAddress remoteIP = _udpClient.remoteIP();
    // only continue if the packet was received from the gateway router
    // for SSDP discovery we continue anyway
    if (gatewayIP != ipNull && remoteIP != gatewayIP) {
        debugPrint(F("Discarded packet not originating from IGD - gatewayIP ["));
        debugPrint(gatewayIP.toString());
        debugPrint(F("] remoteIP ["));
        debugPrint(ipMulti.toString());
        debugPrintln(F("]"));
        return NULL;
    }

    debugPrint(F("Received packet of size ["));
    debugPrint(String(packetSize));
    debugPrint(F("]"));
    debugPrint(F(" ip ["));
    for (int i = 0; i < 4; i++) {
        debugPrint(String(remoteIP[i]));  // Decimal
        if (i < 3) {
            debugPrint(F("."));
        }
    }
    debugPrint(F("] port ["));
    debugPrint(String(_udpClient.remotePort()));
    debugPrintln(F("]"));

    // sanity check
    if (packetSize > UDP_TX_RESPONSE_MAX_SIZE) {
        debugPrint(F("Received packet with size larged than the response buffer, cannot proceed."));
        return NULL;
    }
  
    int idx = 0;
    while (idx < packetSize) {
        memset(packetBuffer, 0, UPNP_UDP_TX_PACKET_MAX_SIZE);
        int len = _udpClient.read(packetBuffer, UPNP_UDP_TX_PACKET_MAX_SIZE);
        if (len <= 0) {
            break;
        }
        debugPrint(F("UDP packet read bytes ["));
        debugPrint(String(len));
        debugPrint(F("] out of ["));
        debugPrint(String(packetSize));
        debugPrintln(F("]"));
        memcpy(responseBuffer + idx, packetBuffer, len);
        idx += len;
    }
    responseBuffer[idx] = '\0';

    debugPrintln(F("Gateway packet content:"));
    debugPrintln(responseBuffer);

    const char * const * deviceList = deviceListUpnp;
    if (gatewayIP == ipNull) {
        deviceList = deviceListSsdpAll;
    }

    // only continue if the packet is a response to M-SEARCH and it originated from a gateway device
    // for SSDP discovery we continue anyway
    if (gatewayIP != ipNull) {  // for the use of listSsdpDevices
        boolean foundIGD = false;
        for (int i = 0; deviceList[i]; i++) {
            if (strstr(responseBuffer, deviceList[i]) != NULL) {
                foundIGD = true;
                debugPrint(F("IGD of type ["));
                debugPrint(deviceList[i]);
                debugPrintln(F("] found"));
                break;
            }
        }

        if (!foundIGD) {
            debugPrintln(F("IGD was not found"));
            return NULL;
        }
    }

    String location = "";
    char* location_indexStart = strstr(responseBuffer, "location:");
    if (location_indexStart == NULL) {
        location_indexStart = strstr(responseBuffer, "Location:");
    }
    if (location_indexStart == NULL) {
        location_indexStart = strstr(responseBuffer, "LOCATION:");
    }
    if (location_indexStart != NULL) {
        location_indexStart += 9;  // "location:".length()
        char* location_indexEnd = strstr(location_indexStart, "\r\n");
        if (location_indexEnd != NULL) {
            int urlLength = location_indexEnd - location_indexStart;
            int arrLength = urlLength + 1;  // + 1 for '\0'
            // converting the start index to be inside the packetBuffer rather than responseBuffer
            char locationCharArr[arrLength];
            memcpy(locationCharArr, location_indexStart, urlLength);
            locationCharArr[arrLength - 1] = '\0';
            location = String(locationCharArr);
            location.trim();
        } else {
            debugPrintln(F("ERROR: could not extract value from LOCATION param"));
            return NULL;
        }
    } else {
        debugPrintln(F("ERROR: LOCATION param was not found"));
        return NULL;
    }
    
    debugPrint(F("Device location found ["));
    debugPrint(location);
    debugPrintln(F("]"));
  
    IPAddress host = getHost(location);
    int port = getPort(location);
    String path = getPath(location);

    ssdpDevice *newSsdpDevice_ptr = new ssdpDevice();
    
    newSsdpDevice_ptr->host = host;
    newSsdpDevice_ptr->port = port;
    newSsdpDevice_ptr->path = path;
    
    // debugPrintln(host.toString());
    // debugPrintln(String(port));
    // debugPrintln(path);

    return newSsdpDevice_ptr;
}

// a single trial to connect to the IGD (with TCP)
boolean TinyUPnP::connectToIGD(IPAddress host, int port) {
    debugPrint(F("Connecting to IGD with host ["));
    debugPrint(host.toString());
    debugPrint(F("] port ["));
    debugPrint(String(port));
    debugPrintln(F("]"));
    if (_wifiClient.connect(host, port)) {
        debugPrintln(F("Connected to IGD"));
        return true;
    }
    return false;
}

// updates deviceInfo with the commands' information of the IGD
boolean TinyUPnP::getIGDEventURLs(gatewayInfo *deviceInfo) {
    debugPrintln(F("called getIGDEventURLs"));
    debugPrint(F("deviceInfo->actionPath ["));
    debugPrint(deviceInfo->actionPath);
    debugPrint(F("] deviceInfo->path ["));
    debugPrint(deviceInfo->path);
    debugPrintln(F("]"));

    // make an HTTP request
    _wifiClient.print(F("GET "));
    _wifiClient.print(deviceInfo->path);
    _wifiClient.println(F(" HTTP/1.1"));
    _wifiClient.println(F("Content-Type: text/xml; charset=\"utf-8\""));
    //_wifiClient.println(F("Connection: close"));
    _wifiClient.println("Host: " + deviceInfo->host.toString() + ":" + String(deviceInfo->actionPort));
    _wifiClient.println(F("Content-Length: 0"));
    _wifiClient.println();
    
    // wait for the response
    unsigned long timeout = millis();
    while (_wifiClient.available() == 0) {
        if (millis() - timeout > TCP_CONNECTION_TIMEOUT_MS) {
            debugPrintln(F("TCP connection timeout while executing getIGDEventURLs"));
            _wifiClient.stop();
            return false;
        }
    }
    
    // read all the lines of the reply from server
    boolean upnpServiceFound = false;
    boolean urlBaseFound = false;
    while (_wifiClient.available()) {
        String line = _wifiClient.readStringUntil('\r');
        int index_in_line = 0;
        debugPrint(line);
        if (!urlBaseFound && line.indexOf(F("<URLBase>")) >= 0) {
            // e.g. <URLBase>http://192.168.1.1:5432/</URLBase>
            // Note: assuming URL path will only be found in a specific action under the 'controlURL' xml tag
            String baseUrl = getTagContent(line, "URLBase");
            if (baseUrl.length() > 0) {
                baseUrl.trim();
                IPAddress host = getHost(baseUrl);  // this is ignored, assuming router host IP will not change
                int port = getPort(baseUrl);
                deviceInfo->actionPort = port;

                debugPrint(F("URLBase tag found ["));
                debugPrint(baseUrl);
                debugPrintln(F("]"));
                debugPrint(F("Translated to base host ["));
                debugPrint(host.toString());
                debugPrint(F("] and base port ["));
                debugPrint(String(port));
                debugPrintln(F("]"));
                urlBaseFound = true;
            }
        }

        // to support multiple <serviceType> tags
        int service_type_index_start = 0;
        
        for (int i = 0; deviceListUpnp[i]; i++) {
            int service_type_index = line.indexOf(UPNP_SERVICE_TYPE_TAG_START + deviceListUpnp[i]);
            if (service_type_index >= 0) {
                debugPrint(F("["));
                debugPrint(deviceInfo->serviceTypeName);
                debugPrint(F("] service_type_index ["));
                debugPrint(String(service_type_index));
                debugPrintln("]");
                service_type_index_start = service_type_index;
                service_type_index = line.indexOf(UPNP_SERVICE_TYPE_TAG_END, service_type_index_start);
            }
            if (!upnpServiceFound && service_type_index >= 0) {
                index_in_line += service_type_index;
                upnpServiceFound = true;
                deviceInfo->serviceTypeName = getTagContent(line.substring(service_type_index_start), UPNP_SERVICE_TYPE_TAG_NAME);
                debugPrint(F("["));
                debugPrint(deviceInfo->serviceTypeName);
                debugPrint(F("] service found! deviceType ["));
                debugPrint(deviceListUpnp[i]);
                debugPrintln(F("]"));
                break;  // will start looking for 'controlURL' now
            }
        }
        
        if (upnpServiceFound && (index_in_line = line.indexOf("<controlURL>", index_in_line)) >= 0) {
            String controlURLContent = getTagContent(line.substring(index_in_line), "controlURL");
            if (controlURLContent.length() > 0) {
                deviceInfo->actionPath = controlURLContent;

                debugPrint(F("controlURL tag found! setting actionPath to ["));
                debugPrint(controlURLContent);
                debugPrintln(F("]"));
                
                // clear buffer
                debugPrintln(F("Flushing the rest of the response"));
                while (_wifiClient.available()) {
                    _wifiClient.read();
                }
                
                // now we have (upnpServiceFound && controlURLFound)
                return true;
            }
        }
    }

    return false;
}

// assuming a connection to the IGD has been formed
// will add the port mapping to the IGD
boolean TinyUPnP::addPortMappingEntry(gatewayInfo *deviceInfo, upnpRule *rule_ptr) {
    debugPrintln(F("called addPortMappingEntry"));

    // connect to IGD (TCP connection) again, if needed, in case we got disconnected after the previous query
    unsigned long timeout = millis() + TCP_CONNECTION_TIMEOUT_MS;
    if (!_wifiClient.connected()) {
        while (!connectToIGD(_gwInfo.host, _gwInfo.actionPort)) {
            if (millis() > timeout) {
                debugPrintln(F("Timeout expired while trying to connect to the IGD"));
                _wifiClient.stop();
                return false;
            }
            delay(500);
        }
    }

    debugPrint(F("deviceInfo->actionPath ["));
    debugPrint(deviceInfo->actionPath);
    debugPrintln(F("]"));

    debugPrint(F("deviceInfo->serviceTypeName ["));
    debugPrint(deviceInfo->serviceTypeName);
    debugPrintln(F("]"));

    strcpy_P(body_tmp, PSTR("<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:AddPortMapping xmlns:u=\""));
    strcat_P(body_tmp, deviceInfo->serviceTypeName.c_str());
    strcat_P(body_tmp, PSTR("\"><NewRemoteHost></NewRemoteHost><NewExternalPort>"));
    sprintf(integer_string, "%d", rule_ptr->externalPort);
    strcat_P(body_tmp, integer_string);
    strcat_P(body_tmp, PSTR("</NewExternalPort><NewProtocol>"));
    strcat_P(body_tmp, rule_ptr->protocol.c_str());
    strcat_P(body_tmp, PSTR("</NewProtocol><NewInternalPort>"));
    sprintf(integer_string, "%d", rule_ptr->internalPort);
    strcat_P(body_tmp, integer_string);
    strcat_P(body_tmp, PSTR("</NewInternalPort><NewInternalClient>"));
    IPAddress ipAddress = (rule_ptr->internalAddr == ipNull) ? WiFi.localIP() : rule_ptr->internalAddr;
    strcat_P(body_tmp, ipAddress.toString().c_str());
    strcat_P(body_tmp, PSTR("</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>"));
    strcat_P(body_tmp, rule_ptr->devFriendlyName.c_str());
    strcat_P(body_tmp, PSTR("</NewPortMappingDescription><NewLeaseDuration>"));
    sprintf(integer_string, "%d", rule_ptr->leaseDuration);
    strcat_P(body_tmp, integer_string);
    strcat_P(body_tmp, PSTR("</NewLeaseDuration></u:AddPortMapping></s:Body></s:Envelope>"));

    sprintf(integer_string, "%d", strlen(body_tmp));
    
    _wifiClient.print(F("POST "));
    _wifiClient.print(deviceInfo->actionPath);
    _wifiClient.println(F(" HTTP/1.1"));
    //_wifiClient.println(F("Connection: close"));
    _wifiClient.println(F("Content-Type: text/xml; charset=\"utf-8\""));
    _wifiClient.println("Host: " + deviceInfo->host.toString() + ":" + String(deviceInfo->actionPort));
    //_wifiClient.println(F("Accept: */*"));
    //_wifiClient.println(F("Content-Type: application/x-www-form-urlencoded"));
    _wifiClient.print(F("SOAPAction: \""));
    _wifiClient.print(deviceInfo->serviceTypeName);
    _wifiClient.println(F("#AddPortMapping\""));

    _wifiClient.print(F("Content-Length: "));
    _wifiClient.println(integer_string);
    _wifiClient.println();

    _wifiClient.println(body_tmp);
    _wifiClient.println();
    
    debugPrint(F("Content-Length was: "));
    debugPrintln(integer_string);
    
    debugPrintln(body_tmp);
  
    timeout = millis();
    while (_wifiClient.available() == 0) {
        if (millis() - timeout > TCP_CONNECTION_TIMEOUT_MS) {
            debugPrintln(F("TCP connection timeout while adding a port mapping"));
            _wifiClient.stop();
            return false;
        }
    }

    // TODO: verify success
    boolean isSuccess = true;
    while (_wifiClient.available()) {
        String line = _wifiClient.readStringUntil('\r');
        if (line.indexOf(F("errorCode")) >= 0) {
            isSuccess = false;
        }
        debugPrintln(line);
    }
    debugPrintln("");  // \n
    
    if (!isSuccess) {
        _wifiClient.stop();
    }

    return isSuccess;
}

boolean TinyUPnP::printAllPortMappings() {
    // verify gateway information is valid
    // TODO: use this _gwInfo to skip the UDP part completely if it is not empty
    if (!isGatewayInfoValid(&_gwInfo)) {
        debugPrintln(F("Invalid router info, cannot continue"));
        return false;
    }
    
    upnpRuleNode *ruleNodeHead_ptr = NULL;
    upnpRuleNode *ruleNodeTail_ptr = NULL;
    auto cleanup_rule_nodes_fn = [&ruleNodeHead_ptr] () {
        upnpRuleNode *curr_ptr = ruleNodeHead_ptr;
        while (curr_ptr != NULL) {
            upnpRuleNode *del_prt = curr_ptr;
            curr_ptr = curr_ptr->next;
            delete del_prt->upnpRule;
            delete del_prt;
        }
    };

    unsigned long startTime = millis();
    boolean reachedEnd = false;
    int index = 0;
    while (!reachedEnd) {
        // connect to IGD (TCP connection) again, if needed, in case we got disconnected after the previous query
        unsigned long timeout = millis() + TCP_CONNECTION_TIMEOUT_MS;
        if (!_wifiClient.connected()) {
            while (!connectToIGD(_gwInfo.host, _gwInfo.actionPort)) {
                if (millis() > timeout) {
                    debugPrint(F("Timeout expired while trying to connect to the IGD"));
                    _wifiClient.stop();
                    cleanup_rule_nodes_fn();
                    return false;
                }
                delay(1000);
            }
        }
        
        debugPrint(F("Sending query for index ["));
        debugPrint(String(index));
        debugPrintln(F("]"));

        strcpy_P(body_tmp, PSTR("<?xml version=\"1.0\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
            "<s:Body>"
            "<u:GetGenericPortMappingEntry xmlns:u=\""));
        strcat_P(body_tmp, _gwInfo.serviceTypeName.c_str());
        strcat_P(body_tmp, PSTR("\">"
            "  <NewPortMappingIndex>"));

        sprintf(integer_string, "%d", index);
        strcat_P(body_tmp, integer_string);
        strcat_P(body_tmp, PSTR("</NewPortMappingIndex>"
            "</u:GetGenericPortMappingEntry>"
            "</s:Body>"
            "</s:Envelope>"));
        
        sprintf(integer_string, "%d", strlen(body_tmp));
        
        _wifiClient.print(F("POST "));
        _wifiClient.print(_gwInfo.actionPath);
        _wifiClient.println(F(" HTTP/1.1"));
        _wifiClient.println(F("Connection: keep-alive"));
        _wifiClient.println(F("Content-Type: text/xml; charset=\"utf-8\""));
        _wifiClient.println("Host: " + _gwInfo.host.toString() + ":" + String(_gwInfo.actionPort));
        _wifiClient.print(F("SOAPAction: \""));
        _wifiClient.print(_gwInfo.serviceTypeName);
        _wifiClient.println(F("#GetGenericPortMappingEntry\""));

        _wifiClient.print(F("Content-Length: "));
        _wifiClient.println(integer_string);
        _wifiClient.println();

        _wifiClient.println(body_tmp);
        _wifiClient.println();
  
        timeout = millis() + TCP_CONNECTION_TIMEOUT_MS;
        while (_wifiClient.available() == 0) {
            if (millis() > timeout) {
                debugPrintln(F("TCP connection timeout while retrieving port mappings"));
                _wifiClient.stop();
                cleanup_rule_nodes_fn();
                return false;
            }
        }
        
        while (_wifiClient.available()) {
            String line = _wifiClient.readStringUntil('\r');
            debugPrint(line);
            if (line.indexOf(PORT_MAPPING_INVALID_INDEX) >= 0) {
                reachedEnd = true;
            } else if (line.indexOf(PORT_MAPPING_INVALID_ACTION) >= 0) {
                debugPrint(F("Invalid action while reading port mappings"));
                reachedEnd = true;
            } else if (line.indexOf(F("HTTP/1.1 500 ")) >= 0) {
                debugPrint(F("Internal server error, likely because we have shown all the mappings"));
                reachedEnd = true;
            } else if (line.indexOf(F("GetGenericPortMappingEntryResponse")) >= 0) {
                upnpRule *rule_ptr = new upnpRule();
                rule_ptr->index = index;
                rule_ptr->devFriendlyName = getTagContent(line, "NewPortMappingDescription");
                String newInternalClient = getTagContent(line, "NewInternalClient");
                if (newInternalClient == "") {
                    delete rule_ptr;
                    continue;
                }
                rule_ptr->internalAddr.fromString(newInternalClient);
                rule_ptr->internalPort = getTagContent(line, "NewInternalPort").toInt();
                rule_ptr->externalPort = getTagContent(line, "NewExternalPort").toInt();
                rule_ptr->protocol = getTagContent(line, "NewProtocol");
                rule_ptr->leaseDuration = getTagContent(line, "NewLeaseDuration").toInt();
                        
                upnpRuleNode *currRuleNode_ptr = new upnpRuleNode();
                currRuleNode_ptr->upnpRule = rule_ptr;
                currRuleNode_ptr->next = NULL;
                if (ruleNodeHead_ptr == NULL) {
                    ruleNodeHead_ptr = currRuleNode_ptr;
                    ruleNodeTail_ptr = currRuleNode_ptr;
                } else {
                    ruleNodeTail_ptr->next = currRuleNode_ptr;
                    ruleNodeTail_ptr = currRuleNode_ptr;
                }
            }
        }
        
        index++;
        delay(250);
    }
    
    // print nicely and free heap memory
    debugPrintln(F("IGD current port mappings:"));
    upnpRuleNode *curr_ptr = ruleNodeHead_ptr;
    upnpRuleNode *del_prt = ruleNodeHead_ptr;
    while (curr_ptr != NULL) {
        upnpRuleToString(curr_ptr->upnpRule);
        del_prt = curr_ptr;
        curr_ptr = curr_ptr->next;
        delete del_prt->upnpRule;
        delete del_prt;
    }
    
    debugPrintln("");  // \n

    _wifiClient.stop();
    
    return true;
}

void TinyUPnP::printPortMappingConfig() {
    debugPrintln(F("TinyUPnP configured port mappings:"));
    upnpRuleNode *currRuleNode = _headRuleNode;
    while (currRuleNode != NULL) {
        upnpRuleToString(currRuleNode->upnpRule);
        currRuleNode = currRuleNode->next;
    }

    debugPrintln("");  // \n
}

// TODO: remove use of String
void TinyUPnP::upnpRuleToString(upnpRule *rule_ptr) {
    String index = String(rule_ptr->index);
    debugPrint(index);
    debugPrint(".");
    debugPrint(getSpacesString(5 - (index.length() + 1)));  // considering the '.' too

    String devFriendlyName = rule_ptr->devFriendlyName;
    debugPrint(devFriendlyName);
    debugPrint(getSpacesString(30 - devFriendlyName.length()));

    IPAddress ipAddress = (rule_ptr->internalAddr == ipNull) ? WiFi.localIP() : rule_ptr->internalAddr;
    String internalAddr = ipAddress.toString();
    debugPrint(internalAddr);
    debugPrint(getSpacesString(18 - internalAddr.length()));

    String internalPort = String(rule_ptr->internalPort);
    debugPrint(internalPort);
    debugPrint(getSpacesString(7 - internalPort.length()));

    String externalPort = String(rule_ptr->externalPort);
    debugPrint(externalPort);
    debugPrint(getSpacesString(7 - externalPort.length()));
    
    String protocol = rule_ptr->protocol;
    debugPrint(protocol);
    debugPrint(getSpacesString(7 - protocol.length()));

    String leaseDuration = String(rule_ptr->leaseDuration);
    debugPrint(leaseDuration);
    debugPrint(getSpacesString(7 - leaseDuration.length()));

    debugPrintln("");
}

void TinyUPnP::printSsdpDevices(ssdpDeviceNode* ssdpDeviceNode_head) {
    ssdpDeviceNode *ssdpDeviceNodeCurr = ssdpDeviceNode_head;
    while (ssdpDeviceNodeCurr != NULL) {
        ssdpDeviceToString(ssdpDeviceNodeCurr->ssdpDevice);
        ssdpDeviceNodeCurr = ssdpDeviceNodeCurr->next;
    }
}

void TinyUPnP::ssdpDeviceToString(ssdpDevice* ssdpDevice) {
    debugPrint(F("SSDP device ["));
    debugPrint(ssdpDevice->host.toString());
    debugPrint(F("] port ["));
    debugPrint(String(ssdpDevice->port));
    debugPrint(F("] path ["));
    debugPrint(ssdpDevice->path);
    debugPrintln(F("]"));
}

String TinyUPnP::getSpacesString(int num) {
    if (num < 0) {
        num = 1;
    }
    String spaces = "";
    for (int i = 0; i < num; i++) {
        spaces += " ";
    }
    return spaces;
}

/*
char* TinyUPnP::ipAddressToCharArr(IPAddress ipAddress) {
    char s[17];
    sprintf(s, "%d.%d.%d.%d", ipAddress[0], ipAddress[1], ipAddress[2], ipAddress[3]);
    s[16] = '\0';
    return s;
}*/

IPAddress TinyUPnP::getHost(String url) {
    IPAddress result(0,0,0,0);
    if (url.indexOf(F("https://")) != -1) {
        url.replace("https://", "");
    }
    if (url.indexOf(F("http://")) != -1) {
        url.replace("http://", "");
    }
    int endIndex = url.indexOf('/');
    if (endIndex != -1) {
        url = url.substring(0, endIndex);
    }
    int colonsIndex = url.indexOf(':');
    if (colonsIndex != -1) {
        url = url.substring(0, colonsIndex);
    }
    result.fromString(url);
    return result;
}

int TinyUPnP::getPort(String url) {
    int port = -1;
    if (url.indexOf(F("https://")) != -1) {
        url.replace("https://", "");
    }
    if (url.indexOf(F("http://")) != -1) {
        url.replace("http://", "");
    }
    int portEndIndex = url.indexOf("/");
    if (portEndIndex == -1) {
        portEndIndex = url.length();
    }
    url = url.substring(0, portEndIndex);
    int colonsIndex = url.indexOf(":");
    if (colonsIndex != -1) {
        url = url.substring(colonsIndex + 1, portEndIndex);
        port = url.toInt();
    } else {
        port = 80;
    }
    return port;
}

String TinyUPnP::getPath(String url) {
    if (url.indexOf(F("https://")) != -1) {
        url.replace("https://", "");
    }
    if (url.indexOf(F("http://")) != -1) {
        url.replace("http://", "");
    }
    int firstSlashIndex = url.indexOf("/");
    if (firstSlashIndex == -1) {
        debugPrint(F("ERROR: Cannot find path in url ["));
        debugPrint(url);
        debugPrintln(F("]"));
        return "";
    }
    return url.substring(firstSlashIndex, url.length());
}

String TinyUPnP::getTagContent(const String &line, String tagName) {
    int startIndex = line.indexOf("<" + tagName + ">");
    if (startIndex == -1) {
        debugPrint(F("ERROR: Cannot find tag content in line ["));
        debugPrint(line);
        debugPrint(F("] for start tag [<"));
        debugPrint(tagName);
        debugPrintln(F(">]"));
        return "";
    }
    startIndex += tagName.length() + 2;
    int endIndex = line.indexOf("</" + tagName + ">", startIndex);
    if (endIndex == -1) {
        debugPrint(F("ERROR: Cannot find tag content in line ["));
        debugPrint(line);
        debugPrint(F("] for end tag [</"));
        debugPrint(tagName);
        debugPrintln(F(">]"));
        return "";
    }
    return line.substring(startIndex, endIndex);
}