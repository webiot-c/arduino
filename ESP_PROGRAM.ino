/*-------ヘッダのインクルード-------*/
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h> 
#include <WiFiUDP.h>
#include <Wire.h>
#include <Ticker.h>
#include <SoftwareSerial.h>
#include <FS.h>
#include "ADXL345.h"
#include "DFPlayer_Mini_Mp3.h"
#include "TinyGPS++.h"
#include "config.h"

/*-------ESP-WROOM-02のピン設定-------*/
#define SUPPLY_CONTROL_PIN 1    //GPSモジュール，DFPlayer Mini給電制御用ピン
#define SDA_PIN 4               //I2Cバス参加用ピン（SDA）
#define SCL_PIN 5               //I2Cバス参加用ピン（SCL）
#define ADXL_INTERRUPT_PIN 12   //加速度センサ割込み検知用ピン
#define SW_INTERRUPT_PIN 14     //蓋開閉検知スイッチ割込み検知用ピン
#define SWSERIAL_RX_PIN 16      //ソフトウェアシリアル用RXピン
#define SWSERIAL_TX_PIN 13      //ソフトウェアシリアル用TXピン

/*-------通信ボーレートの速度設定-------*/
#define UART_SPEED 9600     //UARTボーレート
#define I2C_SPEED 40000     //I2Cボーレート

/*-------DFPlayer Miniに関する設定-------*/
#define MUSIC_VOLUME 15           //DFPlayer Miniの音量
#define ERROR_VOICE 1             //「エラーが発生しました」再生
#define I_AM_OPERATER_VOICE 2     //「オペレータです」再生
#define CHANGE_OPERATER_VOICE 3   //「オペレータに代わります」再生
#define SEND_VOICE 4              //「送信しました」再生

/*-------GPS情報に関する設定-------*/
#define READ_GPS 6              //読み取りバイト数
#define POLLING_PERIOD 10000    //ポーリング周期

/*-------経過時間の計測に関する設定-------*/
#define TRESH_TIME 18   //閾値

/*-------加速度センサに関する設定-------*/
#define READ_ADXL 25        //読み取りバイト数
#define DATA_FORMAT 0x31    //DATA_FORMATのアドレス
#define POWER_CTL 0x2d      //POWER_CTLのアドレス
#define THRESH_ACT 0x24     //THRESH_ACTのアドレス
#define ACT_INACT_CTL 0x27  //ACT_INACT_CTLのアドレス
#define INT_MAP 0x2f        //INT_MAPのアドレス
#define INT_ENABLE 0x2e     //INT_ENABLEのアドレス

/*-------ネットワークに関する設定-------*/
const IPAddress my_ip(192, 168, 1, 100);        //ESP-WROOM-02のIPアドレス
const IPAddress server_ip(192, 168, 1, 150);    //管理サーバのIPアドレス
const IPAddress gateway(192,168, 1, 1);         //デフォルトゲートウェイ
const IPAddress subnetmask(255, 255, 255, 0);   //サブネットマスク
const IPAddress dns_ip(192, 168, 1, 1);         //DNSサーバのIPアドレス

//wifiのパスワードとか
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWD;
const String data_pass = "/request.py";          //取得データを保存するPHPスクリプトのパス

/*
const char *ssid = "hama_system";                 //無線APのESSID
const char *password = "hama_net";                //無線AP接続パスワード
const String data_pass = "/iot.php";              //取得データを保存するPHPスクリプトのパス
*/

/*-------その他の設定および変数群-------*/
//GPS情報変換用インスタンス
TinyGPSPlus gps;

//タイマー割込みインスタンス
Ticker periodic_interrupt;

//ソフトウェアUARTの宣言（RX, TX）
SoftwareSerial music_serial(SWSERIAL_RX_PIN, SWSERIAL_TX_PIN);

//加速度センサのデバイスアドレス(スレーブ)
uint8_t ADXL_ADDRESS = 0x53; 

//計測データ格納用変数
String node_id;       //ノードID格納用
String longtitude;    //経度情報格納用
String latitude;      //緯度情報格納用

//SPIFFSで読み書きするテキストファイル名
const String data_file = "/node_data.txt";

//蓋状態判断用フラグ
volatile boolean exe_flag = false;      //実行フラグ（true：蓋開状態， false：蓋閉状態）
volatile boolean impact_flag = false;   //衝撃検知割込みフラグ
volatile boolean polling_flag = false;  //定周期タイマー割込みフラグ
/*-------ここまでその他の設定および変数群*/


/*-------ここから割込みに関する関数-------*/
//（1）衝撃検知割込みシーケンス（加速度センサからの外部割込みで起動）
void ICACHE_RAM_ATTR impact_interrupt(){
  impact_flag = true;     //衝撃検知割込みフラグを立てる
    
  return;
}


//（2）定周期タイマー割込みシーケンス（10秒経過ごとに起動）
void polling_interrupt(){
  polling_flag = true;    //定周期タイマー割込みフラグを立てる

  return;
}
/*-------ここまで割込みに関する関数-------*/


/*-------ここからESP-WROOM-02本体に関する関数-------*/
//ESP-WROOM-02の初期設定シーケンス
void setup() {
  //ハードウェアUART開始する
  Serial.begin(UART_SPEED);

  //ソフトウェアUART開始する
  music_serial.begin(UART_SPEED); 

  //ファイル入出力インタフェース開始する
  SPIFFS.begin();

  //WiFi接続シーケンスを起動する
  wifi_connection();

  //ピンのモードを設定する
  pinMode(ADXL_INTERRUPT_PIN, INPUT_PULLUP);
  pinMode(SW_INTERRUPT_PIN, INPUT);

  //割込みを受けたときのハンドラを登録する
  attachInterrupt(digitalPinToInterrupt(ADXL_INTERRUPT_PIN), impact_interrupt, RISING);   //衝撃検知割込みシーケンス

  //定周期タイマー割込みを受けたときのハンドラを登録する
  periodic_interrupt.attach_ms(POLLING_PERIOD, polling_interrupt);   //定周期タイマー割込みシーケンス

  //ノードIDと前回の位置情報をテキストファイルから読み取る
  File read_file = SPIFFS.open(data_file, "r");
  node_id = read_file.readStringUntil('\n');       //ノードID
  node_id.trim();
  longtitude = read_file.readStringUntil('\n');    //前回の最終経度情報
  longtitude.trim();
  latitude = read_file.readStringUntil('\n');      //前回の最終緯度情報
  latitude.trim();
  read_file.close();

  //加速度センサ初期化シーケンスを実行する
  set_adxl_resister();

  //DFPlayer Miniとの通信設定，音量設定を行う
  mp3_set_serial (music_serial);
  mp3_set_volume (MUSIC_VOLUME);

  return;
}


//ESP-WROOM-02のループシーケンス
void loop() {

  //蓋開閉検知スイッチがON・衝撃検知フラグがON・実行フラグがON
  if(digitalRead(SW_INTERRUPT_PIN) == HIGH && impact_flag == true && exe_flag == false){
    exe_flag = true;                                       //実行フラグを立てる
    http_access(node_id, longtitude, latitude, "START");   //前回記録した最終位置情報を送信する
    impact_flag = false;                                   //衝撃検知フラグを折る
  }

  //蓋開閉検知スイッチがOFF・実行フラグがON
  if(digitalRead(SW_INTERRUPT_PIN) == LOW && exe_flag == true){
    exe_flag = false;                                      //実行フラグを折る
    http_access(node_id, longtitude, latitude, "END");     //今回記録した最終位置情報を送信する
    
    //ノードIDと前回の最終位置情報をテキストファイルに書き込む
    File write_file = SPIFFS.open(data_file, "w");
    write_file.println(node_id);
    write_file.println(longtitude);
    write_file.print(latitude);
    write_file.close();    
  }

  //定周期タイマー割込みフラグがON・実行フラグがON
  if(polling_flag == true && exe_flag == true){
    http_access(node_id, longtitude, latitude, "POLLING");   //位置情報を送信する
    polling_flag = false;                                    //定周期タイマー割込みフラグを折る
  }
  
  //GPS信号の受信処理
  while(Serial.available() > 0){
    if(gps.encode(Serial.read())){                           //読み込んだデータをエンコードする
      if(gps.location.isValid()){                             //有効な位置情報か判断する
        latitude = String(gps.location.lat(), READ_GPS);      //緯度情報を取得
        longtitude = String(gps.location.lng(), READ_GPS);    //経度情報を取得        
      }
    }
  }

  //加速度センサの割込みフラグを定期リセットする
  Wire.requestFrom(ADXL_ADDRESS, READ_ADXL);
  
  return;
}
/*-------ここまでESP-WROOM-02本体に関する関数-------*/


/*-------以下、自作関数-------*/
//WiFi接続シーケンス
void wifi_connection(){
  //ステーションモードに設定する
  WiFi.mode(WIFI_STA);

  //ネットワークの設定を行う
  WiFi.config(my_ip, gateway, subnetmask);
  delay(100);

  //無線APに接続する
  WiFi.begin(ssid, password);

  //接続が成功するまで待機する
  while(WiFi.status() != WL_CONNECTED) {
      delay(100);
  }  
  return;
}


//HTTP通信シーケンス
void http_access(String id, String lon, String lati, String type){
  //HTTP通信を行うためのインスタンス
  HTTPClient http;
  
  //接続先URLを組み立てる
  String url = "http://";
  url.concat(server_ip.toString());
  url.concat(data_pass);

  //HTTP通信の接続先を指定する
  http.begin(url);

  //HTTPヘッダ情報を追加する
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  //送信データの情報を組み立てる
  String post_param = "node_id=";
  post_param.concat(id);             //ノードID
  post_param.concat("& long_data=");
  post_param.concat(lon);            //経度情報
  post_param.concat("& lat_data=");
  post_param.concat(lati);           //緯度情報
  post_param.concat("& type=");
  post_param.concat(type);           //送信事由情報

  //POST通信を開始する
  int http_code = http.POST(post_param);

  //通信結果を音声報告する
  if(http_code == 200){
    play_voice(SEND_VOICE);     //通信成功「送信しました」
  }else{
    play_voice(ERROR_VOICE);    //通信失敗「エラーが発生しました」
  }
  delay(1);
  
  //HTTP通信を行うインスタンスを解放する
  http.end();
  
  return;  
}


//加速度センサ初期設定シーケンス
void set_adxl_resister(){
  //I2Cバス通信開始（マスターとして参加）
  Wire.begin(SDA_PIN, SCL_PIN);           //(SDA, SCL) 
  Wire.setClockStretchLimit(I2C_SPEED);   //通信速度を制限する
  
  Wire.beginTransmission(ADXL_ADDRESS);
  Wire.write(DATA_FORMAT);    //「DATA_FORMAT」のアドレス
  Wire.write(0x0B);           //各割込みをActive-Hiに、最大分解能モードに、測定レンジを「±16g」に
  Wire.endTransmission();
 
  Wire.beginTransmission(ADXL_ADDRESS);  
  Wire.write(POWER_CTL);      //「POWER_CTL」のアドレス
  Wire.write(0x08);           //測定モードに、スリープ機能を無効に
  Wire.endTransmission();

  Wire.beginTransmission(ADXL_ADDRESS);  
  Wire.write(THRESH_ACT);     //「THRESH_ACT」のアドレス
  Wire.write(0x49);           //加速度閾値を変更
  Wire.endTransmission();

  Wire.beginTransmission(ADXL_ADDRESS);  
  Wire.write(ACT_INACT_CTL);  //「ACT_INACT_CTL」のアドレス
  Wire.write(0x70);           //ACTをDCカップリングにし、対象をXYZ各軸とする
  Wire.endTransmission();

  Wire.beginTransmission(ADXL_ADDRESS);  
  Wire.write(INT_MAP);        //「INT_MAP」のアドレス
  Wire.write(0x00);           //すべての割込みをINT1ピンに転送する
  Wire.endTransmission();

  Wire.beginTransmission(ADXL_ADDRESS);  
  Wire.write(INT_ENABLE);     //「INT_ENABLE」のアドレス
  Wire.write(0x10);           //Activity割込みのみ有効にする
  Wire.endTransmission();

  return;
}


//音声再生シーケンス
void play_voice(int number){
  mp3_play(number);           //MP3を再生する

  return;
}
