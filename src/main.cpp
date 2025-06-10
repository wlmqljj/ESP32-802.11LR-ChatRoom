#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>

// 硬件定义
#define LED_PIN_MODE 12
#define LED_PIN_BUSY 13

#define BOOTBUTTON 9

#define MAX_HISTORY_SIZE 102400  // 100KB历史记录

// 网络配置
const char* ap_ssid = "ESP32-LR-Chat";
const char* ap_password = "12345678";
const uint16_t udp_port = 8888;
IPAddress broadcast_ip(192, 168, 4, 255);

// 全局变量
WiFiUDP udp;
bool is_ap_mode = false;
String message_history;
size_t history_size = 0;

// 协议命令定义
const char CMD_NEW_CLIENT[] = "@@NEW_CLIENT";
const char CMD_HISTORY_START[] = "@@HISTORY_START";
const char CMD_HISTORY_END[] = "@@HISTORY_END";
const char CMD_ACK[] = "@@ACK";  // ACK确认命令

// 配置LR模式并启动网络
void setup_network() {
  Serial.print("Setting up LR mode...");
  
  if (is_ap_mode) {
    WiFi.mode(WIFI_AP);
    esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_LR);
    WiFi.softAP(ap_ssid, ap_password);
    Serial.println("\nAP IP: " + WiFi.softAPIP().toString());
  } else {
    WiFi.mode(WIFI_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR);
    WiFi.begin(ap_ssid, ap_password);
    
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    
    // 通知AP有新客户端加入
    udp.beginPacket(WiFi.gatewayIP(), udp_port);
    udp.write((const uint8_t*)CMD_NEW_CLIENT, strlen(CMD_NEW_CLIENT));
    udp.endPacket();
  }
  
  udp.begin(udp_port);
  digitalWrite(LED_PIN_MODE, LOW);
}

// 添加消息到历史记录
void add_to_history(const String& message) {
  // 计算新消息大小
  size_t msg_size = message.length() + 1;  // +1 for newline
  
  // 循环删除旧记录直到有足够空间
  while (history_size + msg_size > MAX_HISTORY_SIZE && message_history.length() > 0) {
    int pos = message_history.indexOf('\n');
    if (pos == -1) break;
    
    size_t remove_size = pos + 1;
    message_history = message_history.substring(pos + 1);
    history_size -= remove_size;
  }
  
  // 添加新消息
  message_history += message + '\n';
  history_size += msg_size;
}

// 发送历史记录给客户端
void send_history(IPAddress client_ip) {
  Serial.println("Sending history to " + client_ip.toString());
  
  udp.beginPacket(client_ip, udp_port);
  udp.write((const uint8_t*)CMD_HISTORY_START, strlen(CMD_HISTORY_START));
  udp.endPacket();
  
  int start_index = 0;
  while (start_index < message_history.length()) {
    String chunk = message_history.substring(start_index, start_index + 500);
    start_index += chunk.length();
    
    udp.beginPacket(client_ip, udp_port);
    udp.write((const uint8_t*)chunk.c_str(), chunk.length());
    udp.endPacket();
    delay(10);
  }
  
  udp.beginPacket(client_ip, udp_port);
  udp.write((const uint8_t*)CMD_HISTORY_END, strlen(CMD_HISTORY_END));
  udp.endPacket();
}

// 广播消息给所有客户端
void broadcast_message(const String& message) {
  udp.beginPacket(broadcast_ip, udp_port);
  udp.write((const uint8_t*)message.c_str(), message.length());
  udp.endPacket();
  digitalWrite(LED_PIN_BUSY, !digitalRead(LED_PIN_BUSY));
}

// 发送ACK确认
void send_ack(IPAddress sender_ip, const String& msg_id) {
  String ack_msg = CMD_ACK + String(" ") + msg_id;
  udp.beginPacket(sender_ip, udp_port);
  udp.write((const uint8_t*)ack_msg.c_str(), ack_msg.length());
  udp.endPacket();
}

void setup() {
  // 硬件初始化
  pinMode(LED_PIN_MODE, OUTPUT);
  pinMode(LED_PIN_BUSY, OUTPUT);
  digitalWrite(LED_PIN_MODE, HIGH);
  digitalWrite(LED_PIN_BUSY, LOW);
  
  Serial.begin(115200);
  delay(1000);

  // // 检测启动模式 (使用GPIO9作为模式选择)
  // pinMode(BOOTBUTTON, INPUT_PULLUP);
  // is_ap_mode = (digitalRead(BOOTBUTTON) == LOW);
  
  Serial.println(is_ap_mode ? "Starting in AP mode" : "Starting in STA mode");
  
  // 网络初始化
  setup_network();
  
  // 添加系统消息到历史
  if (is_ap_mode) {
    add_to_history("System: AP started at " + WiFi.softAPIP().toString());
  } else {
    add_to_history("System: Client " + WiFi.localIP().toString() + " joined");
  }
}

void loop() {
  // 处理串口输入
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    if (input.length() > 0) {
      String formatted_msg = is_ap_mode ? 
        "[AP]: " + input : 
        "[" + WiFi.localIP().toString() + "]: " + input;
      
      // 添加消息ID (时间戳+发送者IP)
      String msg_id = String(millis()) + "-" + 
                     (is_ap_mode ? "AP" : WiFi.localIP().toString());
      
      String full_msg = "@@" + msg_id + "@@" + formatted_msg;
      
      add_to_history(formatted_msg);
      broadcast_message(full_msg);
      Serial.println(">> " + formatted_msg);
      
      // 初始化ACK等待列表
      if (!is_ap_mode) {
        Serial.println("Waiting for ACKs...");
      }
    }
  }
  
  // 处理UDP数据包
  int packet_size = udp.parsePacket();
  if (packet_size) {
    char buffer[512];
    int len = udp.read(buffer, sizeof(buffer) - 1);
    
    if (len > 0) {
      buffer[len] = '\0';
      String message = String(buffer);
      IPAddress remote_ip = udp.remoteIP();
      
      // 处理协议命令
      if (is_ap_mode) {
        if (message == CMD_NEW_CLIENT) {
          Serial.println("New client: " + remote_ip.toString());
          send_history(remote_ip);
          add_to_history("System: " + remote_ip.toString() + " joined");
          broadcast_message("System: " + remote_ip.toString() + " joined");
        } 
        // 处理ACK确认
        else if (message.startsWith(CMD_ACK)) {
          String msg_id = message.substring(strlen(CMD_ACK) + 1);
          Serial.println("ACK from " + remote_ip.toString() + " for: " + msg_id);
        }
        else {
          // 提取消息ID和实际内容
          int id_start = message.indexOf("@@");
          int id_end = message.indexOf("@@", id_start + 2);
          
          if (id_start != -1 && id_end != -1) {
            String msg_id = message.substring(id_start + 2, id_end);
            String actual_msg = message.substring(id_end + 2);
            
            // 普通消息广播
            add_to_history(actual_msg);
            broadcast_message(message); // 转发完整消息
            
            // 发送ACK给原始发送者
            send_ack(remote_ip, msg_id);
          }
        }
      } else {
        // 客户端模式
        if (message.startsWith(CMD_HISTORY_START)) {
          Serial.println("Receiving history...");
        } else if (message.startsWith(CMD_HISTORY_END)) {
          Serial.println("History received");
        } 
        // 处理ACK确认
        else if (message.startsWith(CMD_ACK)) {
          String msg_id = message.substring(strlen(CMD_ACK) + 1);
          Serial.println("ACK received for: " + msg_id);
        }
        else if (message != CMD_NEW_CLIENT) {
          // 提取消息ID和实际内容
          int id_start = message.indexOf("@@");
          int id_end = message.indexOf("@@", id_start + 2);
          
          if (id_start != -1 && id_end != -1) {
            String msg_id = message.substring(id_start + 2, id_end);
            String actual_msg = message.substring(id_end + 2);
            
            // 修复重复显示：跳过自己发送的消息
            if (!actual_msg.startsWith("[" + WiFi.localIP().toString() + "]")) {
              Serial.println(actual_msg);
              
              // 发送ACK确认
              send_ack(udp.remoteIP(), msg_id);
            }
          }
        }
      }
    }
  }
  
  // AP模式心跳
  if (is_ap_mode) {
    static unsigned long last_broadcast = 0;
    if (millis() - last_broadcast > 30000) {
      last_broadcast = millis();
      
      // 添加消息ID到心跳
      String msg_id = String(millis()) + "-AP";
      String heartbeat = "@@" + msg_id + "@@System: AP heartbeat";
      
      Serial.println(heartbeat);
      broadcast_message(heartbeat);
    }
  }
  
  delay(10);
}
