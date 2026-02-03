/*
 * =============================================================
 * 狼人殺控制系統 (Werewolf Controller) - V1.3.0 Full Expansion
 * -------------------------------------------------------------
 * 1. 角色新增：獵人(Hunter)、守衛(Guard)、白痴(Idiot)
 * 2. 規則擴充：同守同救死亡、獵人毒死禁射、白痴翻牌機制
 * 3. 優化：動態角色分配 (6-15人)
 * =============================================================
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <DFRobotDFPlayerMini.h>
#include <map>
#include <vector>
#include <set>

// --- 硬體引腳 ---
#define OLED_SDA      21
#define OLED_SCL      22
#define JOYSTICK_X    36/*  */
#define JOYSTICK_SW    4
#define BELL_PIN      14
#define DF_BUSY_PIN   18 

// --- 物件實例 ---
HardwareSerial dfSerial(2); 
DFRobotDFPlayerMini myDFPlayer;
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, -1);
DNSServer dnsServer;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- 網路設定 ---
IPAddress apIP(192, 168, 4, 1);
const byte DNS_PORT = 53;

// --- 遊戲變數 ---
std::map<String, String> playerRoleMap;      
std::map<String, int> playerIndexMap;        
std::map<uint32_t, String> clientIdToDeviceId; 
std::vector<String> deadPlayers;             
std::vector<String> lastNightDeadPlayers;    // V1.5: 紀錄昨晚死亡玩家
std::set<String> restartVotes;               

int targetPlayerCount = 7;                   
int currentPlayerCount = 0;                  
bool gameStarted = false;                    
bool isStartingCountdown = false;            
unsigned long countdownStartTime = 0;
bool confirmPressed = false;                 
bool gameOver = false;                       
bool adminApprovedReset = false;             
String winner = "NONE";                      

int nightPhase = -1;      // -1:等待, 4:守衛, 0:狼人, 1:預言家, 2:女巫, 3:白天
int roundCount = 1;       
String wolfTargetId = ""; 
String witchPoisonId = "";
bool witchHasHeal = true; 
bool witchHasPoison = true;

// --- 新角色變數 ---
String lastGuardedId = "";    // 守衛上一晚守的人
String currentGuardedId = ""; // 守衛今晚守的人
bool hunterCanShoot = true;   // 獵人是否有子彈
bool idiotRevealed = false;   // 白痴是否已翻牌免死
String idiotId = "";          // 記錄誰是白痴

unsigned long phaseStartTime = 0;            
bool isPhaseLocked = false; 
unsigned long seerCheckDelayStart = 0;  
bool isSeerCheckPending = false;        
unsigned long audioPlayStartTime = 0;    
bool isAudioPlaying = false;             

// --- V1.4 新增變數 ---
bool hunterActionPending = false; // 是否正在等待獵人行動
unsigned long phaseDelayStartTime = 0; // 用於已死亡神職的假性延遲

// --- V1.9 Stability 新增變數 (非阻塞狀態機用) ---
bool hunterTransitionPending = false;
unsigned long hunterTransitionStart = 0;
int godDeathState = 0; // 0:閒置, 1:等待音效結束
unsigned long godDeathAudioStart = 0;
int godDeathNextPhase = -1;

void playVoice(int fileID, bool wait) {
    Serial.printf("Audio: Playing #%d\n", fileID);
    myDFPlayer.play(fileID);
    delay(50); // Add a small delay for command stability
    audioPlayStartTime = millis();  
    isAudioPlaying = wait;          
}

void triggerBuzzer(int type) {
    if (type == 1) tone(BELL_PIN, 1000, 100); 
    if (type == 2) tone(BELL_PIN, 800, 500);  
}

bool isAlive(String id) {
    for (String d : deadPlayers) { if (d == id) return false; }
    return true;
}

bool isRoleAlive(String roleName) {
    for (auto const& p : playerRoleMap) {
        if (p.second == roleName && isAlive(p.first)) return true;
    }
    return false;
}

void checkVictory() {
    if (!gameStarted || isStartingCountdown || gameOver) return;
    int wolves = 0, humans = 0;
    for (auto const& p : playerRoleMap) {
        if (!isAlive(p.first)) continue; 
        if (p.second == "狼人") wolves++;
        else if (p.second != "Joined" && p.second != "旁觀者") humans++;
    }

    bool justOver = false;
    if (wolves == 0) {
        if (!gameOver) { // 確保只觸發一次
            gameOver = true; winner = "HUMANS"; justOver = true;
        }
    }
    else if (wolves >= humans) {
        if (!gameOver) { // 確保只觸發一次
            gameOver = true; winner = "WOLVES"; justOver = true;
        }
    }

    if (justOver) {
        // V1.5: 播放勝利音效 (請替換為您的音檔ID)
        if (winner == "HUMANS") playVoice(20, false); // 20: 好人勝利音效
        else if (winner == "WOLVES") playVoice(21, false); // 21: 狼人勝利音效
    }
}

void setupRoles() {
    int seq = 1;
    idiotId = ""; idiotRevealed = false; hunterCanShoot = true;
    for (auto &p : playerRoleMap) if (p.second != "旁觀者") playerIndexMap[p.first] = seq++;
    
    // 動態配制角色池
    std::vector<String> rPool = {"狼人", "狼人", "預言家", "女巫"};
    if(targetPlayerCount >= 7) rPool.push_back("獵人");
    if(targetPlayerCount >= 9) rPool.push_back("狼人");
    if(targetPlayerCount >= 10) rPool.push_back("守衛");
    if(targetPlayerCount >= 12) rPool.push_back("狼人");
    if(targetPlayerCount >= 13) rPool.push_back("白痴");
    
    while(rPool.size() < (size_t)targetPlayerCount) rPool.push_back("平民");
    
    // 洗牌
    for(int i = rPool.size() - 1; i > 0; i--) { 
        int j = random(0, i + 1); String t = rPool[i]; rPool[i] = rPool[j]; rPool[j] = t; 
    }
    
    int rIdx = 0;
    for (auto &p : playerRoleMap) {
        if (p.second != "旁觀者") {
            p.second = rPool[rIdx++];
            if(p.second == "白痴") idiotId = p.first;
        }
    }
}

void resetGame() {
    Serial.println("DEBUG: resetGame() called.");
    gameStarted = false; gameOver = false; isStartingCountdown = false; 
    adminApprovedReset = false; winner = "NONE";
    nightPhase = -1; 
    roundCount = 1;
    deadPlayers.clear(); lastNightDeadPlayers.clear(); restartVotes.clear(); confirmPressed = false;
    for (auto &p : playerRoleMap) { if (p.second != "旁觀者") p.second = "Joined"; }
    wolfTargetId = ""; witchPoisonId = ""; witchHasHeal = true; witchHasPoison = true;
    lastGuardedId = ""; currentGuardedId = ""; hunterCanShoot = true; idiotRevealed = false;
    isPhaseLocked = false; isSeerCheckPending = false;
}

// --- WebSocket 處理 ---

void syncGameState() {
    // V1.8 Memory-Debug: 在每次同步狀態時印出剩餘記憶體，用於觀察記憶體洩漏或碎片化問題
    Serial.printf("Sync State - Free Heap: %u bytes\n", ESP.getFreeHeap());

    checkVictory();
    
    // 自動跳過無人職位 (V1.4 - 增加延遲)
    if (gameStarted && !gameOver && !isPhaseLocked && phaseDelayStartTime == 0 && !hunterActionPending) {
        if ((nightPhase == 4 && !isRoleAlive("守衛")) ||
            (nightPhase == 1 && !isRoleAlive("預言家")) ||
            (nightPhase == 2 && !isRoleAlive("女巫"))) {
            phaseDelayStartTime = millis();
            isPhaseLocked = true; // 鎖定介面，顯示「天黑請閉眼」
        }
    }

    DynamicJsonDocument targetDoc(2048);
    JsonArray targets = targetDoc.to<JsonArray>();
    for (auto const& p : playerIndexMap) {
        if (isAlive(p.first)) {
            JsonObject obj = targets.createNestedObject();
            obj["id"] = p.first; obj["index"] = p.second;
        }
    }

    int cdSec = (isStartingCountdown) ? max(0, (int)(3 - (millis() - countdownStartTime) / 1000)) : 0;

    for (auto const& cp : clientIdToDeviceId) {
        DynamicJsonDocument m(3000);
        String devId = cp.second;
        m["type"] = "update";
        m["role"] = playerRoleMap[devId];
        m["index"] = playerIndexMap[devId];
        m["isDead"] = !isAlive(devId);
        m["phase"] = nightPhase;
        m["gameOver"] = gameOver;
        m["winner"] = winner;
        m["adminApproved"] = adminApprovedReset;
        m["targets"] = targets;
        m["isPhaseLocked"] = isPhaseLocked || hunterActionPending; 
        m["hunterActionPending"] = hunterActionPending;
        m["countdown"] = cdSec;
        m["isStarting"] = isStartingCountdown;
        m["idiotRevealed"] = (devId == idiotId && idiotRevealed);

        // V1.6: 新增等待玩家狀態標記與計數
        m["waitingForPlayers"] = (!gameStarted && confirmPressed && !isStartingCountdown);
        m["currentCount"] = currentPlayerCount;
        m["targetCount"] = targetPlayerCount;

        // V1.4 BUGFIX: 傳送續局投票者列表
        JsonArray votedPlayers = m.createNestedArray("votedPlayers");
        if (gameOver && adminApprovedReset) {
            for (const String& voterId : restartVotes) {
                votedPlayers.add(voterId);
            }
        }

        // 獵人開槍判斷
        m["canShoot"] = (playerRoleMap[devId] == "獵人" && !isAlive(devId) && hunterCanShoot);
        
        if (nightPhase == 3) {
            // V1.5: 產生昨晚死亡報告
            // V1.8 Memory-Fix: 優化字串拼接以減少記憶體碎片。預先申請64位元組空間。
            String deathNoteStr;
            deathNoteStr.reserve(64); 

            if (lastNightDeadPlayers.empty()) {
                deathNoteStr = "昨晚是平安夜。";
            } else {
                deathNoteStr = "昨晚死亡的玩家是：";
                for (size_t i = 0; i < lastNightDeadPlayers.size(); ++i) {
                    deathNoteStr += playerIndexMap[lastNightDeadPlayers[i]];
                    deathNoteStr += "號";
                    if (i < lastNightDeadPlayers.size() - 1) deathNoteStr += "、";
                }
                deathNoteStr += "。";
            }
            m["deathNote"] = deathNoteStr;
        }
        
        if (nightPhase == 4 && playerRoleMap[devId] == "守衛") m["lastGuardedId"] = lastGuardedId;
        if (nightPhase == 2 && playerRoleMap[devId] == "女巫") {
            m["hasHeal"] = witchHasHeal; m["hasPoison"] = witchHasPoison;
            m["wolfTargetIndex"] = (wolfTargetId != "") ? playerIndexMap[wolfTargetId] : 0;
            m["wolfTargetId"] = wolfTargetId;
        }
        
        String out; serializeJson(m, out);
        ws.text(cp.first, out);
    }

    // OLED 顯示
    u8g2.clearBuffer();
    if (isStartingCountdown) {
        u8g2.drawStr(0, 20, "READYING...");
        u8g2.setCursor(60, 50); u8g2.print(cdSec);
    } else if (gameOver) {
        u8g2.drawStr(0, 15, "GAME OVER!");
        u8g2.setCursor(0, 35); u8g2.print("Win: "); u8g2.print(winner);
        if (adminApprovedReset) {
            u8g2.drawStr(0, 55, "Ready: ");
            u8g2.setCursor(70, 55); u8g2.print(restartVotes.size());
            u8g2.print("/"); u8g2.print(targetPlayerCount);
        } else {
            u8g2.drawStr(0, 55, "> PRESS SW <");
        }
    } else if (!gameStarted) {
        // V1.6: 區分設定人數與等待連線狀態 (OLED)
        if (confirmPressed) {
            u8g2.drawStr(0, 20, "WAITING JOIN...");
            u8g2.setCursor(30, 50); u8g2.print(currentPlayerCount); 
            u8g2.print(" / "); u8g2.print(targetPlayerCount);
        } else {
            u8g2.drawStr(0, 20, "SET PLAYER:"); u8g2.setCursor(70, 20); u8g2.print(targetPlayerCount);
            u8g2.setCursor(0, 50); u8g2.print("Joined: "); u8g2.print(currentPlayerCount);
        }
    } else {
        u8g2.setCursor(0, 15); u8g2.print("Day: "); u8g2.print(roundCount);
        String pName = "UNKNOWN";
        if(nightPhase==4) pName = "GUARD ACTING";
        else if(nightPhase==0) pName = "WOLF ACTING";
        else if(nightPhase==1) pName = "SEER ACTING";
        else if(nightPhase==2) pName = "WITCH ACTING";
        else pName = "VOTING TIME";
        u8g2.drawStr(0, 40, pName.c_str());
    }
    u8g2.sendBuffer();
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType t, void *arg, uint8_t *d, size_t l){
    if(t!=WS_EVT_DATA) return;
    DynamicJsonDocument doc(1024);
    deserializeJson(doc,(char*)d,l);
    String action=doc["action"];
    String devId=doc["deviceId"];

    if(action=="connect"){
        clientIdToDeviceId[c->id()]=devId;
        if(!playerRoleMap.count(devId)){
            playerRoleMap[devId]=gameStarted?"旁觀者":"Joined";
            currentPlayerCount++;
        }
        syncGameState();
    }
    else if(action=="restart"){
        if (adminApprovedReset) { // 僅在GM同意後才接受續局投票
            restartVotes.insert(devId);
            if(restartVotes.size() >= (size_t)targetPlayerCount){
                // 人數到齊，自動開局
                resetGame(); 
                setupRoles(); 
                isStartingCountdown = true; 
                countdownStartTime = millis(); 
                triggerBuzzer(2);
            }
        }
        syncGameState();
    }
    else if (action == "guardProtect") {
        currentGuardedId = doc["targetId"].as<String>();
        lastGuardedId = currentGuardedId; // 更新禁守紀錄
        playVoice(13, true); // 守衛閉眼
        nightPhase = 0; phaseStartTime = millis(); isPhaseLocked = true; syncGameState();
    }
    else if (action == "wolfKill") {
        wolfTargetId = doc["targetId"].as<String>();
        playVoice(3, true); 
        nightPhase = 1; phaseStartTime = millis(); isPhaseLocked = true; syncGameState();
    }
    else if (action == "seerCheck") {
        if (isSeerCheckPending) return; // V1.4 BUGFIX: 防止重複查驗
        String tRole = playerRoleMap[doc["targetId"].as<String>()];
        // V1.9 Stability-Fix: 使用 ArduinoJson 取代字串拼接，減少記憶體碎片
        DynamicJsonDocument seerDoc(128);
        seerDoc["type"] = "seerResult";
        seerDoc["role"] = tRole;
        String output;
        serializeJson(seerDoc, output);
        c->text(output);
        isSeerCheckPending = true; 
        seerCheckDelayStart = millis();
        isPhaseLocked = true; // V1.4 BUGFIX: 立即鎖定介面
        syncGameState();
    }
    else if (action == "witchHeal" || action == "witchPoison" || action == "witchSkip") {
        bool healed = false;
        if(action == "witchHeal") { 
            witchHasHeal = false; healed = true; 
        } else if(action == "witchPoison") { 
            witchHasPoison = false; 
            witchPoisonId = doc["targetId"].as<String>(); 
            if(playerRoleMap[witchPoisonId] == "獵人") hunterCanShoot = false; // 毒殺不能開槍
        }
        
        // V1.4 - 結算死亡並檢查獵人
        std::vector<String> newly_dead;
        if (healed) {
            if (wolfTargetId == currentGuardedId) newly_dead.push_back(wolfTargetId); // 同守同救 -> 死
        } else {
            if (wolfTargetId != "" && wolfTargetId != currentGuardedId) newly_dead.push_back(wolfTargetId);
        }
        if (witchPoisonId != "") newly_dead.push_back(witchPoisonId);

        lastNightDeadPlayers = newly_dead; // V1.5: 記錄昨晚死者

        bool hunterDiedThisNight = false;
        for(String id : newly_dead) {
            deadPlayers.push_back(id);
            if(playerRoleMap[id] == "獵人" && hunterCanShoot) {
                hunterDiedThisNight = true;
            }
        }

        playVoice(8, true); // 女巫閉眼
        
        if(hunterDiedThisNight) {
            hunterActionPending = true; // 鎖定UI，等待獵人行動
            playVoice(14, false); // V1.4 MOD: 播放獵人行動提示音 (ID 14 為示意)
        } else {
            nightPhase = 3; // 正常進入白天
            phaseStartTime = millis(); 
            isPhaseLocked = true;
        }
        syncGameState();
    }
    else if (action == "champExile") {
        String exId = doc["targetId"].as<String>();
        bool hunterExiled = false;

        if (exId != "") {
            if (exId == idiotId && !idiotRevealed) {
                idiotRevealed = true; // 白痴翻牌免死
            } else {
                deadPlayers.push_back(exId);
                if (playerRoleMap[exId] == "獵人" && hunterCanShoot) {
                    hunterExiled = true;
                }
            }
        }
        
        if (hunterExiled) {
            hunterActionPending = true; // 鎖定UI，等待獵人
            playVoice(14, false); // V1.4 MOD: 播放獵人行動提示音 (ID 14 為示意)
        } else {
            wolfTargetId = ""; witchPoisonId = ""; currentGuardedId = ""; lastNightDeadPlayers.clear(); // V1.5: 進入新夜晚，清空死者名單
            playVoice(1, true); // V1.4 BUGFIX: 進入新夜晚時播放天黑音效
            // V1.4 MOD: 根據守衛是否存在決定下一晚的起始
            if (isRoleAlive("守衛")) {
                nightPhase = 4;
            } else {
                nightPhase = 0;
            }
            roundCount++; // 進入下一晚
            phaseStartTime = millis(); isPhaseLocked = true;
        }
        syncGameState();
    }
    else if (action == "hunterShoot") {
        String shotId = doc["targetId"].as<String>();
        if (shotId != "") {
            deadPlayers.push_back(shotId);
        }
        hunterCanShoot = false;
        playVoice(15, false); // V1.4 MOD: 獵人開槍音效 (ID 15 為示意, 請更換為實際音檔)
        triggerBuzzer(2);

        if(hunterActionPending) {
            hunterActionPending = false;
            // 判斷獵人死亡的時間點以決定下一階段
            if(nightPhase == 3) { // 獵人在白天被投票出局，準備進入新夜晚
                // V1.9 Stability: 改為非阻塞標記，移至 loop() 處理，避免卡死網路
                hunterTransitionPending = true;
                hunterTransitionStart = millis();
            } else { // 獵人在晚上死亡 (可能是 守/狼/預/巫 階段)
                nightPhase = 3; // 進入白天階段
                phaseStartTime = millis(); isPhaseLocked = true;
            }
        }
        syncGameState();
    }
}

// --- 程式入口 ---

void setup() {
    Serial.begin(115200);
    dfSerial.begin(9600, SERIAL_8N1, 16, 17);
    
    pinMode(BELL_PIN, OUTPUT); 
    pinMode(JOYSTICK_SW, INPUT_PULLUP);
    pinMode(DF_BUSY_PIN, INPUT_PULLUP); 

    Wire.begin(OLED_SDA, OLED_SCL); 
    u8g2.begin(); u8g2.setFont(u8g2_font_6x10_tf);

    if (!myDFPlayer.begin(dfSerial)) {
        // V1.8 Memory-Debug: 在啟動時印出初始記憶體狀態
        Serial.println("--- Initial State ---");
        Serial.printf("Total Heap: %u bytes\n", ESP.getHeapSize());
        Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
        Serial.println("DF Error");
    } else {
        myDFPlayer.volume(25);
        myDFPlayer.reset(); // Reset the player to a known state
    }

    // V1.9 Stability: 初始化 LittleFS
    if(!LittleFS.begin(true)){
        Serial.println("An Error has occurred while mounting LittleFS");
        return;
    }

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("Werewolf_V130", "12345678", 1, 0, 15); // 支援到 15 人
    WiFi.setSleep(false); // V1.9 Stability: 關閉 WiFi 省電模式，大幅提升連線穩定度與降低延遲
    dnsServer.start(DNS_PORT, "*", apIP);
    ws.onEvent(onWsEvent); server.addHandler(&ws);

    server.on("/generate_204", [](AsyncWebServerRequest *r){ r->redirect("http://192.168.4.1"); });
    // V1.9 Stability: 改為從 LittleFS 讀取 HTML，大幅減少 RAM 佔用
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    
    server.begin();
    // --- 強制初始化顯示設定畫面 ---
    gameStarted = false;
    isStartingCountdown = false;
    confirmPressed = false; 
    
    u8g2.clearBuffer();
    syncGameState(); // 確保開機第一時間顯示 SET PLAYER 畫面
}

void loop() {
    dnsServer.processNextRequest();
    ws.cleanupClients();

    // --- V1.9 Stability: 獵人開槍後的非阻塞轉場 ---
    if (hunterTransitionPending) {
        // 等待音效播放完畢 (BUSY PIN 為 HIGH 代表閒置) 或超時 3.5秒
        if (digitalRead(DF_BUSY_PIN) == HIGH || (millis() - hunterTransitionStart > 3500)) {
            hunterTransitionPending = false;
            
            wolfTargetId = ""; witchPoisonId = ""; currentGuardedId = ""; lastNightDeadPlayers.clear();
            playVoice(1, true); // 播放天黑音效
            
            if (isRoleAlive("守衛")) {
                nightPhase = 4;
            } else {
                nightPhase = 0;
            }
            roundCount++; 
            phaseStartTime = millis(); isPhaseLocked = true;
            syncGameState();
        }
    }

    // --- V1.9 Stability: 神職死亡延遲的非阻塞狀態機 ---
    // 狀態 0: 等待初始 3秒 延遲
    if (phaseDelayStartTime > 0 && godDeathState == 0) {
        if ((millis() - phaseDelayStartTime) >= 3000) {
            int currentPhase = nightPhase;
            godDeathNextPhase = -1;

            // V1.9 Stability: 修正重複的邏輯區塊與語法錯誤，使用非阻塞狀態機
            if (currentPhase == 4) { // 守衛
                playVoice(13, false); godDeathNextPhase = 0;
            } else if (currentPhase == 1) { // 預言家
                playVoice(5, false); godDeathNextPhase = 2;
            } else if (currentPhase == 2) { // 女巫
                playVoice(8, false); godDeathNextPhase = 3;
                if (wolfTargetId != "" && wolfTargetId != currentGuardedId) {
                    lastNightDeadPlayers.push_back(wolfTargetId);
                    deadPlayers.push_back(wolfTargetId);
                }
            }

            if (godDeathNextPhase != -1) {
                godDeathState = 1; // 進入等待音效狀態
                godDeathAudioStart = millis();
            } else {
                phaseDelayStartTime = 0; // 異常復位
            }
        }
        
        phaseStartTime = millis();
        isPhaseLocked = true; // 確保能觸發下一階段的睜眼音效
        syncGameState();
    }
    // 狀態 1: 等待閉眼音效播放完畢
    else if (godDeathState == 1) {
        if (digitalRead(DF_BUSY_PIN) == HIGH || (millis() - godDeathAudioStart > 5000)) {
            nightPhase = godDeathNextPhase;
            godDeathState = 0;
            phaseDelayStartTime = 0; // 完成流程，清除計時器
            
            phaseStartTime = millis();
            isPhaseLocked = true;
            syncGameState();
        }
    }
    
    // --- 1. 音效與非阻塞延遲處理 (V1.4 BUGFIX: 修正 BUSY PIN 邏輯) ---
    if (isAudioPlaying && digitalRead(DF_BUSY_PIN) == HIGH) isAudioPlaying = false;
    
    if (isSeerCheckPending && (millis() - seerCheckDelayStart >= 5500)) {
        isSeerCheckPending = false;
        playVoice(5, true); 
        nightPhase = 2; phaseStartTime = millis(); isPhaseLocked = true; syncGameState();
    }

    // --- 2. 遊戲進行中的狀態處理 ---
    if (gameStarted && !gameOver && isPhaseLocked && !isSeerCheckPending && (millis() - phaseStartTime >= 2000)) {
        if (digitalRead(DF_BUSY_PIN) == HIGH) { 
            if (nightPhase == 4) playVoice(12, false);
            else if (nightPhase == 0) playVoice(2, false);
            else if (nightPhase == 1) playVoice(4, false);
            else if (nightPhase == 2) playVoice(6, false);
            else if (nightPhase == 3) playVoice(9, true);
            isPhaseLocked = false;
            syncGameState();
        }
    }

    // --- 3. 人數設定與開局觸發 (解決鎖定 14 人與不顯示畫面的重點) ---
    if (!gameStarted && !isStartingCountdown) {
        if (!confirmPressed) {
            // 讀取搖桿並增加死區 (Deadzone) 判斷，避免數值浮動
            int xVal = analogRead(JOYSTICK_X);
            bool swBtn = (digitalRead(JOYSTICK_SW) == LOW);

            if (xVal > 3600 && targetPlayerCount < 15) { 
                targetPlayerCount++; 
                triggerBuzzer(1); 
                delay(200); // 增加延遲避免跳太快
                syncGameState();
            }
            else if (xVal < 400 && targetPlayerCount > 6) { 
                targetPlayerCount--; 
                triggerBuzzer(1); 
                delay(200); 
                syncGameState();
            }
            
            if (swBtn) { 
                confirmPressed = true; 
                triggerBuzzer(2); 
                delay(500); 
                syncGameState();
            }
        } 
        // 只有在 confirmPressed 之後，才判斷人數是否達標開局
        else if (currentPlayerCount >= targetPlayerCount) {
            setupRoles(); 
            isStartingCountdown = true;
            countdownStartTime = millis(); 
            triggerBuzzer(2);
            syncGameState();
        }
    }

    // --- 4. 倒數計時與結束處理 ---
    if (isStartingCountdown && (millis() - countdownStartTime >= 4000)) {
        isStartingCountdown = false; 
        gameStarted = true; 
        // V1.4 MOD: 根據守衛是否存在決定夜晚的起始階段
        if (isRoleAlive("守衛")) {
            nightPhase = 4; // 有守衛從守衛開始
        } else {
            nightPhase = 0; // 無守衛則跳過，直接從狼人開始
        }
        playVoice(1, true); 
        phaseStartTime = millis(); 
        isPhaseLocked = true;
        syncGameState();
    }

    if (gameOver && !adminApprovedReset && (digitalRead(JOYSTICK_SW) == LOW)) { 
        adminApprovedReset = true;
        restartVotes.clear();
        delay(500); 
        syncGameState();
    }
    
    // --- 定時刷新 ---
    if (!gameStarted) { // 在設定階段與倒數階段都進行刷新
        static unsigned long lastOledRefresh = 0;
        if (millis() - lastOledRefresh > 500) {
            lastOledRefresh = millis();
            syncGameState();
        }
    }

    delay(10);
}