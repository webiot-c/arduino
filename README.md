# arduino

CPRSS 位置情報送信デバイスのプログラム。緯度・経度・ノードIDを配信サーバーへ送信する。

## 必要なファイル

.inoファイルと同じ階層に`config.h`を作成します。このファイルは、wifiのssidやパスワードを保存するものです。

config.h には、以下のように記述してください。

```
//wifi
#define WIFI_SSID ""	//wifiのssid
#define WIFI_PASSWD ""	//wifiのパスワード
```