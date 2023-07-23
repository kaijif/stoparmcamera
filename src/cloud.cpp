#include "appGlobals.h"
// #include <ESP32Ping.h>
#include <string>
#include <HTTPClient.h>

static fs::FS fp = STORAGE;
WiFiClientSecure net = WiFiClientSecure();
PubSubClient s3MqttClient(net);
bool isUploadingToS3 = false;
const char *boundary =  "----------------------------dharmannsucks\r\n";
enum awsState {
    AWS_CONNECTION_SUCCESS,
    AWS_CONNECTION_FAILED_NO_WIFI,
    AWS_CONNECTION_FAILED_SERVER_UNREACHABLE,
    AWS_CONNECTION_FAILED_SERVER_REACHABLE,
    AWS_CONNECTION_FAILED_EXOTIC_NETWORK_STATE
};

char payloadBuffer[3000];

class S3HttpClient : public HTTPClient {
    public:
        int POSTtoS3(char* prelude, const char* tail, File file) {
                // File file1 = fp.open("/data/configs.txt");
                const char *type = "POST";
                size_t fileSize = file.size();
                // size_t fileSize = file1.size();
                size_t size = strlen(prelude) + fileSize + strlen(tail);
                int code;
                bool redirect = false;
                uint16_t redirectCount = 0;
                do {
                    // wipe out any existing headers from previous request
                    for(size_t i = 0; i < _headerKeysCount; i++) {
                        if (_currentHeaders[i].value.length() > 0) {
                            _currentHeaders[i].value.clear();
                        }
                    }

                    log_d("request type: '%s' redirCount: %d\n", type, redirectCount);
                    
                    // connect to server
                    if(!connect()) {
                        LOG_INF("No connection, trying again");
                        const TickType_t xDelay = 5000 / portTICK_PERIOD_MS;
                        vTaskDelay(xDelay);
                        if (!connect()) {
                            LOG_ERR("Still no connection");
                            return returnError(HTTPC_ERROR_CONNECTION_REFUSED);
                        }
                    }

                    if(size > 0) {
                        addHeader(F("Content-Length"), String(size));
                    }

                    // add cookies to header, if present
                    String cookie_string;
                    if(generateCookieString(&cookie_string)) {
                        addHeader("Cookie", cookie_string);
                    }

                    // send Header
                    if(!sendHeader(type)) {
                        return returnError(HTTPC_ERROR_SEND_HEADER_FAILED);
                    }
                    
                    size_t prelude_sent = _client->write(prelude);
                    if (prelude_sent > 0) {
                        LOG_INF("8");
                    }
                    uint32_t writeBytes = 0, progCnt = 0; 
                    uint32_t uploadStart = millis();
                    size_t readLen, writeLen;
                    byte uploadChunk[5 * 1024];
                    float_t s3PercentLoaded = 0;
                    do {
                    // upload file in chunks
                        readLen = file.read(uploadChunk, 5 * 1024);
                        if (readLen) {
                            writeLen = _client->write((const uint8_t*)uploadChunk, readLen);
                            writeBytes += writeLen;
                            if (writeLen == 0) {
                                LOG_INF("Done writing for file");
                                break;
                            } else {
                                Serial.print('=');
                            }
                        }
                    } while (readLen > 0);
                    size_t tail_written = _client->write(tail);

                    if (tail_written > 0) {
                        Serial.println("D");
                    }


                    code = handleHeaderResponse();
                    log_d("sendRequest code=%d\n", code);

                    // Handle redirections as stated in RFC document:
                    // https://www.w3.org/Protocols/rfc2616/rfc2616-sec10.html
                    //
                    // Implementing HTTP_CODE_FOUND as redirection with GET method,
                    // to follow most of existing user agent implementations.
                    //
                    redirect = false;
                    if (
                        _followRedirects != HTTPC_DISABLE_FOLLOW_REDIRECTS && 
                        redirectCount < _redirectLimit &&
                        _location.length() > 0
                    ) {
                        switch (code) {
                            // redirecting using the same method
                            case HTTP_CODE_MOVED_PERMANENTLY:
                            case HTTP_CODE_TEMPORARY_REDIRECT: {
                                if (
                                    // allow to force redirections on other methods
                                    // (the RFC require user to accept the redirection)
                                    _followRedirects == HTTPC_FORCE_FOLLOW_REDIRECTS ||
                                    // allow GET and HEAD methods without force
                                    !strcmp(type, "GET") || 
                                    !strcmp(type, "HEAD")
                                ) {
                                    redirectCount += 1;
                                    log_d("following redirect (the same method): '%s' redirCount: %d\n", _location.c_str(), redirectCount);
                                    if (!setURL(_location)) {
                                        log_d("failed setting URL for redirection\n");
                                        // no redirection
                                        break;
                                    }
                                    // redirect using the same request method and payload, diffrent URL
                                    redirect = true;
                                }
                                break;
                            }
                            // redirecting with method dropped to GET or HEAD
                            // note: it does not need `HTTPC_FORCE_FOLLOW_REDIRECTS` for any method
                            case HTTP_CODE_FOUND:
                            case HTTP_CODE_SEE_OTHER: {
                                redirectCount += 1;
                                log_d("following redirect (dropped to GET/HEAD): '%s' redirCount: %d\n", _location.c_str(), redirectCount);
                                if (!setURL(_location)) {
                                    log_d("failed setting URL for redirection\n");
                                    // no redirection
                                    break;
                                }
                                // redirect after changing method to GET/HEAD and dropping payload
                                type = "GET";
                                size = 0;
                                redirect = true;
                                break;
                            }

                            default:
                                break;
                        }
                    }

                } while (redirect);
                // handle Server Response (Header)
                return returnError(code);
        }
        ~S3HttpClient()
        {
            if(_client) {
                _client->stop();
            }
            if(_currentHeaders) {
                delete[] _currentHeaders;
            }
            if(_tcpDeprecated) {
                _tcpDeprecated.reset(nullptr);
            }
            // if(_transportTraits) {
            //     _transportTraits.reset(nullptr);
            // }
        }
};

 bool gotMessage = false;
 bool sentMessage = false;
 bool error = false;

// BLOCKING!!!
awsState connectToAWS() {

    // Configure WiFiClientSecure to use the AWS IoT device credentials
    net.setCACert(AWS_CERT_CA);
    net.setCertificate(AWS_CERT_CRT);
    net.setPrivateKey(AWS_CERT_PRIVATE);

    // Connect to the MQTT broker on the AWS endpoint we defined earlier
    s3MqttClient.setServer(AWS_IOT_ENDPOINT, 8883);

    const TickType_t xDelay = 5000 / portTICK_PERIOD_MS;

    // uint8_t pingAttempts = 0;
    // while (!Ping.ping(AWS_IOT_ENDPOINT)) {
    //     if (WiFi.status() == WL_CONNECTED) {
    //         LOG_ERR("Ping failed, trying again soon");
    //         vTaskDelay(xDelay);
    //         pingAttempts++;
    //         if (pingAttempts < 5) {
    //             LOG_ERR("Too many attempts, leaving...");
    //             return AWS_CONNECTION_FAILED_SERVER_REACHABLE;
    //         }
    //     } else {
    //         return AWS_CONNECTION_FAILED_NO_WIFI;
    //     }
    // }

    uint8_t mqttAttempts = 0;
    while (!s3MqttClient.connect(THINGNAME)) {
        vTaskDelay(xDelay);
        if (WiFi.status() == WL_CONNECTED) {
            LOG_ERR("Failed to connect MQTT client but WiFi connection was present, is AWS down?");
            mqttAttempts++;
            if (mqttAttempts < 5) {
                LOG_ERR("Too many attempts, leaving...");
                return AWS_CONNECTION_FAILED_SERVER_REACHABLE;
            }
        } else {
            LOG_ERR("WiFi disconnected");
            return AWS_CONNECTION_FAILED_EXOTIC_NETWORK_STATE;
        }
    }

    return AWS_CONNECTION_SUCCESS;
}



void constructRequest(char* payload, unsigned int length, char body[], char url[], char fileName[]) {
    DynamicJsonDocument doc(4000);
    DeserializationError deserization_error = deserializeJson(doc, payload, length);
    if (deserization_error) {
        LOG_INF("deserializeJson() failed: %s", deserization_error.f_str());
        error = true;
        return;
    }

    // write file to buffer
    strcpy(url, doc["url"].as<const char*>());
    snprintf(fileName, 64, "/videos/%s", doc["fields"]["key"].as<const char*>());

    // build form data
    char key[128];
    snprintf(key, (size_t)128, "Content-Disposition: form-data; name=\"key\"\r\n\r\n%s\r\n", doc["fields"]["key"].as<const char*>());
    char AWSAccessKeyId[128];
    snprintf(AWSAccessKeyId, 128, "Content-Disposition: form-data; name=\"AWSAccessKeyId\"\r\n\r\n%s\r\n", doc["fields"]["AWSAccessKeyId"].as<const char*>());
    char x_amz_security_token[1024];
    snprintf(x_amz_security_token, 1024, "Content-Disposition: form-data; name=\"x-amz-security-token\"\r\n\r\n%s\r\n", doc["fields"]["x-amz-security-token"].as<const char*>());
    char policy[1600];
    snprintf(policy, 1600, "Content-Disposition: form-data; name=\"policy\"\r\n\r\n%s\r\n", doc["fields"]["policy"].as<const char*>());
    char signature[128];
    snprintf(signature, 100, "Content-Disposition: form-data; name=\"signature\"\r\n\r\n%s\r\n", doc["fields"]["signature"].as<const char*>());
    char fileLine[128];
    snprintf(fileLine, 128, "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n\r\n", doc["fields"]["key"].as<const char*>());

    snprintf(body, 3000, "%s%s%s%s%s%s%s%s%s%s%s%s", boundary, key, boundary, AWSAccessKeyId, boundary, x_amz_security_token, boundary, policy, boundary, signature, boundary, fileLine);

    return;
} 


void uploadTask(void* parameterz){
    gotMessage = true;
    char const *tail = "\r\n----------------------------dharmannsucks--";
    char url_c[50];
    char fileName[64];
    char body[3000];
    constructRequest(payloadBuffer, sizeof(payloadBuffer), body, url_c, fileName);
    if (!fp.exists(fileName)) {
        error = true;
        LOG_ERR("File doesn't exist?");
    }
    if (error) {
        sentMessage = true;
        vTaskDelete(NULL);
    }
    File file = fp.open(fileName);
    S3HttpClient httpClient;
    // httpClient.begin("https://webhook.site/7235a0ae-642d-400e-856f-356fcf3837f1");
    httpClient.begin(url_c);
    httpClient.addHeader("Accept", "*/*");
    httpClient.addHeader("Accept-Encoding", "gzip, deflate");
    httpClient.addHeader("Content-Type", "multipart/form-data; boundary=--------------------------dharmannsucks");
    // httpClient.addHeader("Content-Type", "text/plain");
    httpClient.addHeader("Connection", "keep-alive");
    httpClient.setUserAgent("StopArmCamera");
    LOG_INF("about to upload to s3");
    int uploadSuccess = httpClient.POSTtoS3(body, tail, file);
    file.close();
    httpClient.~S3HttpClient();
    if (uploadSuccess < 0) {
        LOG_ERR("Upload failed with error code %i", uploadSuccess);
        error = true;
        LOG_ERR("Upload failed?");
        LOG_ERR("Client said %s", httpClient.getString());
        sentMessage = true;
        vTaskDelete(NULL);
    } else {
        LOG_INF("Uploaded %s to S3", fileName);
        deleteFolderOrFile(fileName);
        sentMessage = true;
        vTaskDelete(NULL);
    }
}

void messageHandler(char* topic, byte* payload, unsigned int length) {
    LOG_INF("Got response from AWS IOT");
    memcpy(payloadBuffer, (char*)payload, length);
    xTaskCreate(&uploadTask, "upload_task", 12000, NULL, 2, NULL);
    return;
}

void mqttLoop(void* parameters) {
    while (true) {
        if (!s3MqttClient.loop()) {
            LOG_ERR("Loop blew up?");
            error = true;
        }
    }
}

void uploadAllOnWifiConnect(void* parameter) {
    isUploadingToS3 = true;
    File video_folder = fp.open("/videos");
    LOG_INF("opeened videos folder");
    File vid = video_folder.openNextFile();
    LOG_INF("got video");
    s3MqttClient.setBufferSize(4000);
    awsState awsConnected = connectToAWS();
    if (awsConnected != AWS_CONNECTION_SUCCESS) {
        LOG_ERR("Bruh");
        isUploadingToS3 = false;
        vTaskDelete(NULL);
    }
    s3MqttClient = s3MqttClient.setCallback(messageHandler);
    TaskHandle_t mqttLoopTaskHandle;
    xTaskCreate(&mqttLoop, "mqttLoop", 5096, NULL, 1, &mqttLoopTaskHandle);
    if (!(awsConnected == AWS_CONNECTION_SUCCESS)) {
        isUploadingToS3 = false;
        LOG_ERR("Connection failed");
        vTaskDelete(mqttLoopTaskHandle);
        vTaskDelete(NULL); // try again?
    }
    while (true) {
        if (!vid) {
            LOG_INF("Done uploading?");
            isUploadingToS3 = false;
            vTaskDelete(mqttLoopTaskHandle);
            vTaskDelete(NULL);
        } else if (error) {
            LOG_INF("messageHandler reported an error, aborting...");
            isUploadingToS3 = false;
            vTaskDelete(mqttLoopTaskHandle);
            vTaskDelete(NULL);
        }
        const char* vidName = vid.name();
        LOG_INF("Uploading %s", vidName);
        StaticJsonDocument<JSON_OBJECT_SIZE(5)> payload;
        payload["object_name"] = vidName;
        payload["client_name"] = CLIENT_NAME;
        char payloadString[128];
        serializeJson(payload, payloadString);
        checkMemory();
        char subscribeTopic[128];
        snprintf(subscribeTopic, (size_t)128, "stop_arm_cameras/%s/urls/%s" , CLIENT_NAME, vidName);
        bool subbed = s3MqttClient.subscribe(subscribeTopic);
        bool pubbed = s3MqttClient.publish("stop_arm_cameras/request_url", payloadString, false);
        if (pubbed && subbed) {
            LOG_INF("Successfully pubbed and subbed");
        } else {
            LOG_ERR("Did not successfully pub and sub");
            LOG_ERR("Sub reported %s, pub reported %s, connected is %s, state is %i", subbed ? "true" : "false", pubbed ? "true" : "false", s3MqttClient.connected() ? "true" : "false", s3MqttClient.state());
        }
        checkMemory();
        const TickType_t xDelay = 500 / portTICK_PERIOD_MS;
        while (!sentMessage) {
            vTaskDelay(xDelay);
        }
        LOG_INF("MQTT: connected is %s, state is %i", s3MqttClient.connected() ? "true" : "false", s3MqttClient.state());
        LOG_INF("Opening next file...");
        vid.close();
        s3MqttClient.unsubscribe(subscribeTopic);
        vid = video_folder.openNextFile();
        sentMessage = false;
        gotMessage = false;
        const TickType_t xLongDelay = 5000 / portTICK_PERIOD_MS;
        vTaskDelay(xLongDelay);
    }
    video_folder.close();
    isUploadingToS3 = false;
    vTaskDelete(mqttLoopTaskHandle);
    vTaskDelete(NULL);
}