/*

MQTT MODULE

Copyright (C) 2016-2017 by Xose Pérez <xose dot perez at gmail dot com>

*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <vector>
#include <ArduinoJson.h>
#include<Mutex.h>
#include<thread>
//using namespace std;  //8.9
Mutex mu;
vector<int> vector_status;//8.9
vector<unsigned long> vector_time;//8.9

const char *mqtt_user = "ynnhrqtf";
const char *mqtt_pass = "H9yFT-KhiIgY";
const char *mqtt_host="m21.cloudmqtt.com";

#if MQTT_USE_ASYNC
#include <AsyncMqttClient.h>
AsyncMqttClient mqtt;
#else
#include <PubSubClient.h>
WiFiClient mqttWiFiClient;
PubSubClient mqtt(mqttWiFiClient);
bool _mqttConnected = false;
#endif

String mqttTopic;
bool _mqttForward;
char *_mqttUser = 0;
char *_mqttPass = 0;
std::vector<void (*)(unsigned int, const char *, const char *)> _mqtt_callbacks;
#if MQTT_SKIP_RETAINED
    unsigned long mqttConnectedAt = 0;
#endif
int count=0;
//String currentchip;
String bugchip;

//long startTime=-1;
//long stopTime=-1;
uint32_t current_time;

os_timer_t sntp_read_timer;
void ICACHE_FLASH_ATTR
sntp_read_timer_callback(void *arg)
{
    current_time = sntp_get_current_timestamp();
    current_time=current_time-8*3600+30;
    Serial.println(current_time);
    Serial.println(sntp_get_real_time(current_time));
    mqttSend("time",String(current_time).c_str());
    mqttSend("time",String(sntp_get_real_time(current_time)).c_str());
}

void ICACHE_FLASH_ATTR
my_sntp_init(void)
{
    sntp_setservername(0,"0.cn.pool.ntp.org");
    sntp_setservername(1,"1.cn.pool.ntp.org");
    sntp_setservername(2,"2.cn.pool.ntp.org");
    sntp_init();

    os_timer_disarm(&sntp_read_timer);
    os_timer_setfn(&sntp_read_timer, sntp_read_timer_callback , NULL);
    os_timer_arm(&sntp_read_timer,1000,1);
}


// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool mqttConnected() {
    return mqtt.connected();
}

void mqttDisconnect() {
    mqtt.disconnect();
}

void buildTopics() {
    // Replace identifier
    mqttTopic = getSetting("mqttTopic", MQTT_TOPIC);
    bugchip=String(ESP.getChipId())+"";
    mqttTopic.replace("{identifier}",bugchip.c_str());
    if (!mqttTopic.endsWith("/")) mqttTopic = mqttTopic + "/";
}

bool mqttForward() {
    return _mqttForward;
}

String mqttSubtopic(char * topic) {

    String response;

    String t = String(topic);
    String mqttSetter = getSetting("mqttSetter", MQTT_USE_SETTER);

    if (t.startsWith(mqttTopic) && t.endsWith(mqttSetter)) {
        response = t.substring(mqttTopic.length(), t.length() - mqttSetter.length());
    }

    return response;

}

void mqttSendRaw(const char * topic, const char * message) {
    if (mqtt.connected()) {
        DEBUG_MSG_P(PSTR("[MQTT] Sending %s => %s\n"), topic, message);
        #if MQTT_USE_ASYNC
            mqtt.publish(topic, MQTT_QOS, MQTT_RETAIN, message);
        #else
            mqtt.publish(topic, message, MQTT_RETAIN);
        #endif
    }
}

void mqttSend(const char * topic, const char * message) {
    String mqttGetter = getSetting("mqttGetter", MQTT_USE_GETTER);
    String path = mqttTopic + String(topic) + mqttGetter;
    mqttSendRaw(path.c_str(), message);
}

void mqttSend(const char * topic, unsigned int index, const char * message) {
    String mqttGetter = getSetting("mqttGetter", MQTT_USE_GETTER);
    String path = mqttTopic + String(topic) + String ("/") + String(index) + mqttGetter;;
    mqttSendRaw(path.c_str(), message);
}

void mqttSubscribeRaw(const char * topic) {
    if (mqtt.connected() && (strlen(topic) > 0)) {
        DEBUG_MSG_P(PSTR("[MQTT] Subscribing to %s\n"), topic);
        mqtt.subscribe(topic, MQTT_QOS);
    }
}

void mqttSubscribe(const char * topic) {
    String mqttSetter = getSetting("mqttSetter", MQTT_USE_SETTER);
    String path = mqttTopic + String(topic) + mqttSetter;
    mqttSubscribeRaw(path.c_str());
}

// -----------------------------------------------------------------------------
// Callbacks
// -----------------------------------------------------------------------------

void mqttRegister(void (*callback)(unsigned int, const char *, const char *)) {
    _mqtt_callbacks.push_back(callback);
}

void _mqttOnConnect() {

    DEBUG_MSG_P(PSTR("[MQTT] Connected!\n"));

    #if MQTT_SKIP_RETAINED
        mqttConnectedAt = millis();
    #endif

    // Build MQTT topics
    buildTopics();

    // Send first Heartbeat
    heartbeat();

    // Subscribe to system topics
    mqttSubscribe(MQTT_TOPIC_ACTION);
    String path = mqttTopic + String("send/#");
    mqttSubscribeRaw(path.c_str());



    // Send connect event to subscribers
    for (unsigned char i = 0; i < _mqtt_callbacks.size(); i++) {
        (*_mqtt_callbacks[i])(MQTT_CONNECT_EVENT, NULL, NULL);
    }

}

void _mqttOnDisconnect() {

    DEBUG_MSG_P(PSTR("[MQTT] Disconnected!\n"));

    // Send disconnect event to subscribers
    for (unsigned char i = 0; i < _mqtt_callbacks.size(); i++) {
        (*_mqtt_callbacks[i])(MQTT_DISCONNECT_EVENT, NULL, NULL);
    }

}

void _mqttOnMessage(char* topic, char* payload, unsigned int len) {

    char message[len + 1];
    strlcpy(message, (char *) payload, len + 1);


    DEBUG_MSG_P(PSTR("[MQTT] Received %s => %s"), topic, message);
    #if MQTT_SKIP_RETAINED
        if (millis() - mqttConnectedAt < MQTT_SKIP_TIME) {
			DEBUG_MSG_P(PSTR(" - SKIPPED\n"));
			return;
		}
    #endif
	DEBUG_MSG_P(PSTR("\n"));

    // Check system topics
    String t = mqttSubtopic((char *) topic);
    if (t.equals(MQTT_TOPIC_ACTION)) {
        if (strcmp(message, MQTT_ACTION_RESET) == 0) {
            ESP.restart();
        }
    }

    // Send message event to subscribers
    for (unsigned char i = 0; i < _mqtt_callbacks.size(); i++) {
        (*_mqtt_callbacks[i])(MQTT_MESSAGE_EVENT, topic, message);
    }

}


void mqttConnect() {

    if (!mqtt.connected()) {

        if (getSetting("mqttServer", MQTT_SERVER).length() == 0) return;

        // Last option: reconnect to wifi after MQTT_MAX_TRIES attemps in a row
      /*  #if MQTT_MAX_TRIES > 0
            static unsigned int tries = 0;
            static unsigned long last_try = millis();
            if (millis() - last_try < MQTT_TRY_INTERVAL) {
                if (++tries > MQTT_MAX_TRIES) {
                    DEBUG_MSG_P(PSTR("[MQTT] MQTT_MAX_TRIES met, disconnecting from WiFi\n"));
                    wifiDisconnect();
                    tries = 0;
                    return;
                }
            } else {
                tries = 0;
            }
            last_try = millis();
        #endif*/

        mqtt.disconnect();

        if (_mqttUser) free(_mqttUser);
        if (_mqttPass) free(_mqttPass);

        char * host = new char[strlen(mqtt_host)+1];
        strcpy_P(host,mqtt_host);//strdup(getSetting("mqttServer", MQTT_SERVER).c_str());
        unsigned int port = 18487 ;//getSetting("mqttPort", MQTT_PORT).toInt();61615
        _mqttUser=new char[strlen(mqtt_user)+1];
        strcpy_P(_mqttUser,mqtt_user); //strdup(getSetting("mqttUser").c_str());
        _mqttPass =new char[strlen(mqtt_pass)+1];
        strcpy_P(_mqttPass,mqtt_pass);// strdup(getSetting("mqttPassword").c_str());
        char * will = strdup((mqttTopic + MQTT_TOPIC_STATUS).c_str());

        DEBUG_MSG_P(PSTR("[MQTT] Connecting to broker at %s:%d"), host, port);
        mqtt.setServer(host, port);

        #if MQTT_USE_ASYNC

            mqtt.setKeepAlive(MQTT_KEEPALIVE).setCleanSession(true);
            mqtt.setWill(will, MQTT_QOS, true, "0");
          //  mqtt.setWill(("iss/"+String(ESP.getChipId())+"/send/#").c_str(), MQTT_QOS, MQTT_RETAIN, "0");
          //  mqtt.subscribe(("iss/"+String(ESP.getChipId())+"/send/#").c_str(), MQTT_QOS);
            if ((strlen(_mqttUser) > 0) && (strlen(_mqttPass) > 0)) {
                DEBUG_MSG_P(PSTR(" as user '%s'."), _mqttUser);
                mqtt.setCredentials(_mqttUser, _mqttPass);
            }
            DEBUG_MSG_P(PSTR("\n"));

            mqtt.connect();

        #else

            bool response;

            if ((strlen(_mqttUser) > 0) && (strlen(_mqttPass) > 0)) {
                DEBUG_MSG_P(PSTR(" as user '%s'\n"), _mqttUser);
                response = mqtt.connect(getIdentifier().c_str(), _mqttUser, _mqttPass, will, MQTT_QOS, MQTT_RETAIN, "0");
            } else {
                DEBUG_MSG_P(PSTR("\n"));
				response = mqtt.connect(getIdentifier().c_str(), will, MQTT_QOS, MQTT_RETAIN, "0");
            }

            if (response) {
                _mqttOnConnect();
                _mqttConnected = true;
            } else {
                DEBUG_MSG_P(PSTR("[MQTT] Connection failed\n"));
            }

        #endif

        free(host);
        free(will);

        String mqttSetter = getSetting("mqttSetter", MQTT_USE_SETTER);
        String mqttGetter = getSetting("mqttGetter", MQTT_USE_GETTER);
        _mqttForward = !mqttGetter.equals(mqttSetter);
    }
}

void mqttSetup()
{
    //static std::mutex m;
    #if MQTT_USE_ASYNC
        mqtt.onConnect([](bool sessionPresent)
        {
            _mqttOnConnect();
        });
        mqtt.onDisconnect([](AsyncMqttClientDisconnectReason reason)
        {
            _mqttOnDisconnect();
        });
        mqtt.onSubscribe([](uint16_t packetId, uint8_t qos)
        {
			       Serial.println("subscribe");
		    });
        mqtt.onMessage([](char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
        {
              if(String(topic).indexOf("send")!=-1)
               {
                 StaticJsonBuffer<200> jsonBuffer;
                 JsonObject& root = jsonBuffer.parseObject(payload);
                 if (!root.success())
                 {
                     Serial.println("parseObject() failed");
                     return;
                 }
                 if(root.containsKey("status")&&root.containsKey("time"))
                 {
                       int status=root["status"];
                       unsigned long time=root["time"];
                       mu.lock();
                       //PT_BEGIN(pt);
                       vector_time.push_back(time);
                       vector_status.push_back(status);
                      mu.unlock();
                      //PT_END(pt);
                       mqttSend("reply",0,"0");
                 }
                else if(root.containsKey("status"))
                 {
                   int status=root["status"];
                   setStatus(status==1);
                   mqttSend("reply",0,"0");
                   //sendStatus(status==1);
                 }
                 return;
               }
            _mqttOnMessage(topic, payload, len);
        });
    #else
        mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
          receive=true;
            _mqttOnMessage(topic, (char *) payload, length);
              Serial.println(topic);
        });
    #endif
    buildTopics();
    my_sntp_init();
}

void mqttLoop()
 {
    //static std::mutex m;
    mu.lock();
    std::vector<unsigned long>::iterator itVec;
    std::vector<int>::iterator itVed;
    itVed=vector_status.begin();
    for( itVec = vector_time.begin(); itVec != vector_time.end(); )
    {
           if(abs( *itVec-current_time)<=60 )
           {
                itVec =vector_time.erase( itVec);
                bool sta=(*itVed==1);
                setStatus(sta);
                itVed=vector_status.erase(itVed);
                sendStatus(sta);
           }
           else
           {
                itVec++;
                itVed++;
            }
    }
    mu.unlock();
    static unsigned long lastPeriod = 0;

    if (WiFi.status() == WL_CONNECTED) {

        if (!mqtt.connected()) {

            #if not MQTT_USE_ASYNC
                if (_mqttConnected) {
                    _mqttOnDisconnect();
                    _mqttConnected = false;
                }
            #endif

        	unsigned long currPeriod = millis() / MQTT_RECONNECT_DELAY;
        	if (currPeriod != lastPeriod) {
        	    lastPeriod = currPeriod;
                Serial.println("mqtt reconnecting");
                mqttConnect();
            }


        #if not MQTT_USE_ASYNC
        } else {
            mqtt.loop();
        #endif

        }

    }

}
