# term25f
VM間でOBSを用いた動画配信を成功させるための手順の全体像を、設定した内容に基づいてまとめて解説します。

## 1\. ネットワーク環境の構築とIPアドレスの設定

VM間で相互に通信できるようにするため、VirtualBoxの「内部ネットワーク」機能を使用し、両方のVMに**静的IPv4アドレス**を設定します。

| 設定項目 | 配信VM（OBS側） | 受信VM（RTMPサーバー側） |
| :--- | :--- | :--- |
| **VirtualBox設定** | ネットワークアダプターを**内部ネットワーク**に設定し、**名前を一致**させる | ネットワークアダプターを**内部ネットワーク**に設定し、**名前を一致**させる |
| **IPアドレス** | **`10.0.0.20/24`** | **`10.0.0.10/24`** |
| **コマンド例** | `sudo ip addr add 10.0.0.20/24 dev enp0s3` | `sudo ip addr add 10.0.0.10/24 dev enp0s3` |

### 💡 補足

  * `10.0.0.x`のアドレスは、VMの再起動で消えることがあるため、再起動後は`ip a`で確認し、必要に応じて上記コマンドを再実行してください。
  * この設定により、配信VMから受信VMへ **`ping 10.0.0.10`** が通るようになります。

## 2\. RTMPサーバー（Nginx）の設定

配信データを受け付け、視聴者に中継するRTMPサーバーを\*\*受信VM（`10.0.0.10`）\*\*に構築します。

1.  **NginxとRTMPモジュールのインストール:**

    ```bash
    sudo apt update
    sudo apt install nginx libnginx-mod-rtmp -y
    ```

2.  **Nginx設定ファイル（`/etc/nginx/nginx.conf`）の編集:**
    ファイルの末尾に以下のRTMPブロックを追加します。

    ```nginx
    rtmp {
        server {
            listen 1935;             # RTMP標準ポート
            chunk_size 4096;
            application live {       # アプリケーション名（配信URLの一部）
                live on;
            }
        }
    }
    ```

3.  **Nginxの再起動:**
    設定を反映させ、サービスを起動します。

    ```bash
    sudo systemctl daemon-reload
    sudo systemctl restart nginx
    ```

4.  **動作確認:**
    ポート $1935$ が待ち受け状態になっていることを確認します。

    ```bash
    sudo ss -tulnp | grep 1935
    # 出力例: 0.0.0.0:1935 LISTEN
    ```

-----

## 3\. OBSからの配信設定と開始

\*\*配信VM（`10.0.0.20`）\*\*のOBSで、配信先をカスタムRTMPサーバーに設定します。

1.  **OBS「設定」→「配信」タブの設定:**

      * **サービス:** **カスタム...**
      * **サーバー (URL):** **`rtmp://10.0.0.10:1935/live`**
      * **ストリームキー:** **`myvideo`** （任意のキーを設定）

2.  **Luaスクリプトの設定:**
    動画の6回ループと自動停止を制御するLuaスクリプトを読み込み、\*\*「メディアソースを選択」\*\*のプロパティを正しく設定します。

3.  **配信開始:**
    OBSメイン画面の\*\*「配信開始」\*\*ボタンを押すと、RTMPサーバーへ動画ストリームが送信されます。

「サーバーへの接続に失敗しました」などと表示されるときはOSやOBSを再起動すると配信できるようになる。

-----

## 4\. 配信を視聴する方法（VLC）

RTMPサーバーと同じVM内で、ストリームを受信・再生します。

1.  **VLC Media Playerのインストール:**

    ```bash
    sudo apt install vlc -y
    ```

2.  **VLCでの視聴URL入力:**
    VLCを起動し、「メディア」→「ネットワークストリームを開く」を選択します。同じVM内のNginxにアクセスするため、**ローカルホストアドレス**を使用するのが最も確実です。

    **視聴URL:**
    $$\text{rtmp://127.0.0.1:1935/live/myvideo}$$

    （IPアドレスを `127.0.0.1` に、ポート、アプリケーション、ストリームキーはOBSの設定と一致させてください。）


## ipアドレスの設定
- [【Linux】IPアドレス固定＆アドレス変更方法](https://qiita.com/mtn_kt/items/633bd5e3e00732af564e) を参考に設定する。
 手動→IPアドレス
-  途切れる問題:  設定→ネットワーク→有線→Ipv4→手動　で適用

## TCコマンドの設定
    enp0s3の出力帯域を50Mbpsに制限、50msの遅延と10msのジッタを発生
    
     ```bash
     sudo tc qdisc add dev enp0s3 root handle 1:0 tbf rate 50mbit burst 25kb limit 250kb
     sudo tc qdisc add dev enp0s3 parent 1:1 handle 10:1 netem delay 50ms 10ms distribution normal
     sudo tc qdisc show dev enp0s3
     ```


 ### TC設定の削除
     
     sudo tc qdisc del dev enp0s3 root
     
## 📝 送信データ量（KiB）計測手順のまとめ
OBSの統計から見れる
### ステップ 1: パケットキャプチャの実行と取得

1.  [cite_start]**輻輳制御アルゴリズムの適用:** 実験で評価したい輻輳制御アルゴリズム（SACCまたはCubic）がRTMP通信のソケットに正しく適用されていることを確認します 。
2. **`tc`の設定:** 評価したいケースのネットワーク制限（帯域幅、遅延、ジッター）を配信側VMのネットワークインターフェースに`tc`コマンドで適用します。
3. **キャプチャの開始:** 配信側VMのRTMP通信を行うインターフェースで`tcpdump`を開始し、PCAPファイルとして保存します [cite: 71, 72]。
    ```bash
    sudo tcpdump -i <インターフェース名> tcp port 1935 -w rtmp_capture_X.pcap
    ```
4. **ライブストリーミング実行:** キャプチャ開始と同時にOBSで配信を開始し、60秒間（素材動画の再生時間）実行します。
5.  **キャプチャの停止:** 60秒経過後、配信と`tcpdump`を停止し、PCAPファイルを取得します。

### ステップ 2: Wiresharkによる分析とバイト数の特定

1.  **PCAPファイルを開く:** 取得したPCAPファイルをホストマシンなどでWiresharkで開きます。
2.  **RTMP通信の特定:** フィルタバーに `tcp.port == 1935` を入力し、RTMPパケットのみを表示します。
3.  **TCPストリームの追跡:** 表示されたRTMPパケットの一つを右クリックし、**「追跡 (Follow)」** → \*\*「TCPストリーム (TCP Stream)」\*\*を選択して、該当するセッションを分離します。
4.  **「会話」統計ウィンドウを開く:** メニューバーから **「統計 (Statistics)」** → **「会話 (Conversations)」を選択し、「TCP」タブ**に切り替えます。
5.  **送信バイト数の確認:** リストの中からポート1935を含む会話を探します。
      * 配信側VMのIPアドレス（Address A）から受信側VMのIPアドレス（Address B）へ流れたデータ量を示す、**「Bytes A $\to$ B」の値を記録します。この値はバイト（Bytes）単位の総データ量**です。

### ステップ 3: KiBへの単位換算

1. **換算式の適用:** ステップ2で確認した\*\*総バイト数（Bytes）**を、論文の単位である**KiB（キロバイト）\*\*に変換します。

$$\text{送信データ量 (KiB)} = \frac{\text{Bytes A} \to \text{B}}{1024}$$

# eBPF TCP Congestion Control Switcher

このプロジェクトは、eBPFの `struct_ops` を利用して独自のTCP輻輳制御アルゴリズムをカーネルに登録し、`sock_ops` プログラムを用いて特定の通信に対してそのアルゴリズムを動的に適用するものです。

## 実行手順とコマンド解説

### 1. 輻輳制御アルゴリズムのコンパイルと登録

独自の輻輳制御ロジック（`my_rtmp_cc.c`）をカーネルにロードします。

```bash
# BPFバイトコードへのコンパイル
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I/home/ubuntu-send/libbpf/src -c my_rtmp_cc.c -o my_rtmp_cc.o

# カーネルの struct_ops として登録（TCPスタックの一部として認識させる）
sudo bpftool struct_ops register my_rtmp_cc.o

```

* **意味**: カーネルに `my_rtmp_cc` という新しいTCPアルゴリズムを「プラグイン」として追加します。

---

### 2. SockOps プログラムのロード

ソケットの状態変化（接続開始など）を監視し、アルゴリズムを切り替えるプログラムを準備します。

```bash
# SockOpsプログラムのコンパイル
clang -g -O2 -target bpf -D__TARGET_ARCH_x86 -I/home/ubuntu-send/libbpf/src -c congestion_control_2.bpf.c -o congestion_control_2.bpf.o

# プログラムをBPFファイルシステムにピン留め（永続化）
sudo bpftool prog load congestion_control_2.bpf.o /sys/fs/bpf/rtmp_sockops

```

* **意味**: `rtmp_sockops` という名前で、ソケット操作を制御するプログラムをカーネル内にロードします。

---

### 3. Cgroupへの適用（有効化）、ログ

ロードしたプログラムを特定のCgroup（プロセスグループ）に紐付けて、実際に動作を開始させます。

```bash
# プログラムを cgroup_sock_ops としてアタッチ
sudo bpftool cgroup attach /sys/fs/cgroup/ sock_ops pinned /sys/fs/bpf/rtmp_sockops

```

* **意味**: これにより、このCgroup配下で発生するTCP接続に対して、`congestion_control_2` のロジック（RTMPなら `my_rtmp_cc` を使う等）が自動適用されます。

```bash
#rtmp_cc.logにログを書き込みながらターミナルにリアルタイムでログを表示する
sudo cat /sys/kernel/debug/tracing/trace_pipe | tee rtmp_cc.log
```

---

### 4. 動作確認

実際にアルゴリズムが切り替わっているかをネットワーク統計コマンドで確認します。

```bash
# TCPソケットの詳細情報を表示
ss -tin

```

* **確認ポイント**: 出力結果に `my_rtmp_cc` という文字列が含まれていれば、独自アルゴリズムが適用されています。

---

### 5. プログラムの解除（無効化）

テスト終了後、元の設定（`cubic` 等）に戻すためにプログラムを切り離します。

```bash
# CgroupからSockOpsプログラムをデタッチ
sudo bpftool cgroup detach /sys/fs/cgroup/ sock_ops pinned /sys/fs/bpf/rtmp_sockops

```

* **意味**: 動的な切り替えロジックを停止します。既存の接続は維持されますが、新規接続は標準設定に戻ります。



---

## トラブルシューティング

* **`Error: too few parameters`**: `bpftool` の引数順序（cgroupパス、アタッチタイプ、プログラム指定）が正しいか確認してください。
* **`can't find in /etc/fstab`**: `mount` コマンドでBPFファイルシステムをマウントしようとする際に、`/etc/fstab` に記載がないと発生します。通常、最近の環境では `/sys/fs/bpf` は自動でマウントされています。

### 1. Cgroupからのデタッチ（実行の停止）

まず、プログラムがネットワークトラフィックを制御している状態を解除します。

```bash
sudo bpftool cgroup detach /sys/fs/cgroup/ sock_ops pinned /sys/fs/bpf/rtmp_sockops

```

* **解説**: これにより、新しいTCP接続に対してプログラムが介入しなくなります。

---

### 2. ピン留めされたファイルの削除（メモリからの解放）

`bpftool prog load` コマンドで `/sys/fs/bpf/rtmp_sockops` に作成したファイルを削除します。

```bash
sudo rm /sys/fs/bpf/rtmp_sockops

```

* **解説**: BPFプログラムは、参照カウントが0になると自動的にカーネルメモリから解放されます。「デタッチ」し、さらに「ピン留め（ファイル）」を消すことで、どこからも参照されなくなり、**完全にアンロード**されます。

---

### 3. (参考) struct_ops の登録解除

輻輳制御アルゴリズム自体の登録も消したい場合は、以下の手順で行います。

まず、登録されている `id` を確認します。

```bash
sudo bpftool struct_ops show

```

（あなたのログでは `id 31` でした）

次に、そのID（または名前）を指定してアンレジスタします。

```bash
sudo bpftool struct_ops unregister name my_rtmp_cc_ops
# または
sudo bpftool struct_ops unregister id 31

```

---

### まとめ：一括削除用コマンド

一連の作業をリセットしたい場合は、以下の3行を実行してください。

```bash
# 1. 実行を止める
sudo bpftool cgroup detach /sys/fs/cgroup/ sock_ops pinned /sys/fs/bpf/rtmp_sockops 2>/dev/null

# 2. プログラム本体をカーネルから消す
sudo rm /sys/fs/bpf/rtmp_sockops

# 3. アルゴリズムの登録を消す
sudo bpftool struct_ops unregister name my_rtmp_cc_ops

```
