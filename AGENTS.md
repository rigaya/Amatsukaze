## 構成

Amatsukazeはクライアント・サーバーアーキテクチャに基づくアプリケーションです。

Amatsukazeは以下のようにして構成されます。

- ```AmatsuakzeServer/```

  Amatsukazeのサーバー。キューを持ち、AmatsuakzeCLIを駆動して、キューに登録されたタスクの処理を行う。
  
  - C# (.NET Core)
  - Windows/Linux両対応

- ```AmatsuakzeGUI/```

  Amatsuakzeのフロントエンドの実装。
  
  - C# (WPF)
  - Windows専用
  
  - 実行方法

    - ```AmatsukazeGUI.exe -l server```

      AmatsuakzeServerのGUI版が起動。
    
    - ```AmatsukazeGUI.exe -l client```

      AmatsukazeGUIがclientとしてのみ起動。別途起動したserverに接続して使用。このサーバーは ```AmatsukazeGUI.exe -l server``` あるいは ```AmatsuakzeServerCLI``` で起動したもの。
    
    - ```AmatsukazeGUI.exe -l standalone```

    スタンドアロン版。実態はclientとserverが両方起動。

- ```AmatsuakzeServerCLI/```

  サーバーのコンソールアプリ版。
  
  - C# (.NET Core)
  - Windows/Linux両対応

- ```AmatsuakzeServerWin/```

  AmatsukazeServerのWPFが必要な処理の実装。
  
  - C# (WPF)
  - Windows専用

- ```AmatsuakzeAddTask/```

  タスクをサーバーに追加するためのコンソールアプリ。
  
  - C# (.NET Core)
  - Windows/Linux両対応

- ```AmatsukazeShared/```

  REST API向けDTOとクライアント共通APIラッパーを提供する共有ライブラリ。
  
  - C# (.NET)
  - Windows/Linux両対応

- ```AmatsukazeWebUI/```

  Blazor WebAssemblyによるWebフロントエンド。AmatsukazeServerのREST APIを利用する。
  
  - C# (Blazor WASM)
  - Windows/Linux両対応（ブラウザ動作）

- ```Amatsukaze/```
   タスク処理の実体部分。Amatsukaze[.dll,.so]/Amatsukaze2[.dll,.so]としてビルドされ、AmatsuakzeCLIにロードされる。
  
  - C++17
  - Windows/Linux両対応
  - TCHARでUnicode対応

- ```AmatsuakzeCLI/```

  タスク処理を行うコンソールアプリ。
  Amatsukaze[.dll,.so]/Amatsukaze2[.dll,.so]をロードして実行。

  - C++17
  - Windows/Linux両対応

- ```common/```
 C++のutility関数等。```Amatsukaze/```で使用される。OS互換ラッパー等を含む。C++17までの標準機能を優先しつつ、場合によってはWindows側によせるためのラッパー実装。
  
  - C++17
  - Windows/Linux両対応
  
- ```ScriptCommand/```

  ```AmatsuakzeServer```, ```AmatsuakzeCLI``` の行うユーザーバッチ処理実行で、使用可能なコンソールアプリ。```AmatsuakzeServer```から情報取得等が可能。
  
  - C++17
  - Windows/Linux両対応
  
- ```ScriptCommandWrapper/```

  ```ScriptCommand```を呼び出す補助コンソールアプリ。
  
  - C++17
  - Windows用
  
- ```doc/```

  ドキュメント類。
  
- ```data/```

  ドキュメントその他で必要な画像等の静的リソース。
  
- ```scripts/```

  ビルドや配布アーカイブ生成に必要なスクリプト類。
  
- ```docker/```

  - ```Dockerfile```
  - ```compose.sample.yml```
  - ```setup.sh```
  - ```readme.md```
    AmatsukazeServer実行用のdockerコンテナの作成用。```readme.md```がその使用方法
	
  - ```docker_ubuntu2004```
  
    Linux用配布パッケージのネイティブ側 (C++) のビルド用のdockerfile。CIで使用。
    ここでビルドしたものは glibc 2.31 以降のディストリで動くため、配布パッケージは
    OSバージョン別に分けず1本にしている。
    .NET側は .NET 10 SDK のある Ubuntu 24.04 のベースイメージで別途ビルドする。
  
  - ```docker_ubuntu2204```
  - ```docker_ubuntu2404```
  
    OSバージョン別にビルドしていた頃のdockerfile。現在CIでは使用しない。
  
- ```defaults/```

  配布用デフォルト設定。

- ```.github/workflows```

  - ```build_base_image.yml```
  
    ```build_packages.yml```で使用するLinux用配布パッケージをビルド・作成するためのdocker imageを作成する。
  
  - ```build_packages.yml```
  
    ```build_base_image.yml```で作成したimage上で、Linux用配布パッケージをビルド・作成する。
  
  - ```build_amatsukazeaddtask.yml```
  
    AmatsuakzeAddTaskのみのパッケージの作成を行う。

- ```TVCaption2Mod/```
- ```libfaad/```

  ```Amatsukaze/```の依存モジュール。原則ビルドするのみで開発/変更は行わない。
  
## ビルド

- Windows

  ```publish.bat``` でビルド

- Linux

  ```doc/BuildLinux.md``` を参照

### Windows専用プロジェクトのコンパイル検証 (Linux/WSLから)

```AmatsukazeGUI``` / ```AmatsukazeServerWin``` は ```net10.0-windows``` + WPF だが、
Linux/WSL からでも ```EnableWindowsTargeting``` を付ければコンパイル検証ができる。

```
dotnet build AmatsukazeGUI/AmatsukazeGUI.csproj -p:EnableWindowsTargeting=true
```

- XAMLのマークアップコンパイルも実際に走るため、XAMLの型解決やプロパティ名の誤りもここで検出できる。
- 実行はできない。バインドパス等の実行時に解決される部分は検証されないので、動作確認はWindows実機で行うこと。
- WPF側のコードを変更した場合は、コミット前にこれを通すこと。

## WindowsのC#標準試験

リポジトリ直下から実行する。展開器はLinuxのCIと同じ版を取得し、SHA-256照合後に試験用の一時ディレクトリへ置く。準備に失敗した場合は試験を開始する前にエラーで終了する。

```powershell
powershell.exe -NoProfile -File .\scripts\test-csharp-windows.ps1
```

Release構成は`-Configuration Release`を付ける。TRXとハングダンプは既定で`build/csharp-test-results`配下の今回の実行専用ディレクトリに保存する。

## 試験の作成方針

試験(`tests/native/`、`AmatsukazeServerTest/`、非公開の`cli_itest`)を書く・直す・レビューするときに守ること。2026-11に、実時間の期限、ホスト環境、並列実行に結果が左右される試験をまとめて直した。その原因と決まりをここにまとめる。

### 判定

- **試験は100%成功しなければならない。** 合否は最初の実行の結果で決める。再実行、期限の延長、負荷を下げての実行で通ったものは合格にしない。落ちたら原因を特定して直し、直したツリーで最初から実行し直す。
- **確かめるのは終わったときの状態。** 戻り値、終了コード、出力の内容、EOF、成果物で判定する。「ある時間内に起きた/起きなかった」では判定しない。所要時間は記録してよいが、合否に使わない。
- 一度だけ起きた異常終了(例: `SIGABRT`)を偶然と片付けない。2026-11の`SIGABRT`は、`EventBaseSubProcess`が派生型の構築前に読み取りスレッドを起動していた製品の不具合だった。

### 実時間で待たない

終わるはずの処理は上限を付けずに待つ。止まった試験の検出は試験の外で行う。

| 種類 | 止まった試験の検出 |
|---|---|
| ネイティブ(Meson) | `timeout : 0`。CIのジョブの`timeout-minutes` |
| C#(xUnit) | `dotnet test --blame-hang-timeout 5m` |
| cli_itest | 起動元の監視(例: `timeout --foreground 45m ./tools/cli_itest/run.py ...`)。ケースに秒数を書かない |

- 試験の中では次のものを使わない。
  - 実時間の期限が付いた待ち: `WaitAsync(TimeSpan)`、`CancellationTokenSource(TimeSpan)`、`CancelAfter`、時間付きの`WaitForExit`、`wait_for`/`wait_until`
  - 時間で待つもの: `Task.Delay`、`Thread.Sleep`、`std::this_thread::sleep_for`、シェルの`sleep`
  - ポーリング: 条件が成り立つまで繰り返し確かめる処理(ファイルの出現待ち、ポートの接続試行の繰り返しなど)
- 「一定時間、何も起きないこと」を確かめる試験は書かない。次の応答、終了、EOFなどの決まった出来事まで進めてから、記録に現れていないことを確かめる。
- `TimeSpan`や秒数を書いてよいのは、偽の時計に渡す値と、時刻やフレーム間隔のような「データとしての時間」だけ。

### 製品の期限

製品には実時間の期限がある(更新のDNS・HTTP・ダウンロード、実行ファイルの確認、自己更新のREADY、プレビューエンジンのREADYとquitなど)。

- **期限を試験しないとき**: 期限なし(`Timeout.InfiniteTimeSpan`)で作る。既定値のまま作らない。
- **期限そのものを試験するとき**: 期限と`TimeProvider`を内部から渡せるようにし、`AmatsukazeServerTest/FakeTimeProvider.cs`と偽の相手(プロセスや通信を使わないもの)で試験する。`ObserveTimers()`でタイマーの登録を受け取ってから時刻を進める。偽の時計と本物のプロセス・通信を同じ試験で混ぜない。
- 製品の既定の期限の値は、試験のために変えない。

### 本物のプロセスと通信

- 本物のプロセスを使うのは、プロセスでしか確かめられない性質(パイプ、EOF、終了コード、プロセスツリーの回収など)の試験に限る。
- 相手が書くものは、相手の終了やほかの出来事を待つ前に読む。パイプのバッファが埋まると、互いを待って止まる。
- 偽の子プロセスに実時間の期限を持たせない。終わる条件は「指示を受け取る」か「入力のEOF」だけにする。
- `EventBaseSubProcess`の派生型は、構築が終わってから`start()`する。構築中に出力の読み取りを始めない。
- 偽のHTTPサーバーは、ポート0で`bind`したソケットをそのまま使う。番号だけ取って閉じ、あとで取り直す方法は、ほかの処理と取り合いになる。
- Windowsでは、強制終了した相手のTCP接続がEOFではなくRSTを返すことがある。切断の判定は両方を扱う。

### ホスト環境と並列実行

- ホストの`PATH`、ホームディレクトリ、共有の素材ディレクトリの中身に結果を左右させない。実行ファイルの探索を確かめる試験は、専用の一時ディレクトリに探索先を作る。2026-11に、WSLからWindows側の`PATH`を繰り返し探索し、ネイティブ試験が数十秒遅れて期限切れになった。
- 一時ファイル、出力先、キャッシュは実行ごとに分ける。同時に起動した別の実行と共有しない。
- 試験の準備のコードは、Windowsでも動くものを使う(`xz`やシェルスクリプトに頼らない)。Bashに渡すスクリプトはLFにする。

### 文字コード

- 出力をリダイレクトしてテキストとして読むときは、文字コードを明示する(C#は`StandardOutputEncoding`/`StandardErrorEncoding`)。指定しないと、日本語のWindows(CP932)と英語のCI(cp1252)で結果が変わる。

### 確認

- 試験を足したり直したりしたら、LinuxとWindowsの両方で回す。ネイティブはFFmpegの2構成(`build_ff612`、`build_ffnk`)の両方。
- 本物のプロセスや同期に関わる試験を変えたら、CPUに負荷をかけた状態でも回し、回数と失敗の件数を記録する。1回でも落ちたら原因を直す。
- 落ちた試験を直すときは、その1件だけで終わらせない。同じ形の箇所をほかの試験からも探して、まとめて直す。

## 実装方針

- 日本語のコメントを多めに記述する
- 既存のコメントはそのまま保持する。ただし、変更によりコメント内容がコードの記述と合わなくなった場合は削除せず、適切に変更する
- ログメッセージを適切に記述すること
- 既存のラッパー関数があれば、それを積極的に使用し、コードの統一性を保つこと
- Windows/Linuxの差異に注意すること
  - 可能な限りWindows/Linux共通実装を心がける。
  - (C++)
    - Windows/Linuxの固有関数を呼び出す必要がある場合、```common/```以下のラッパー関数、次にC++17標準の使用を推奨。
      - 困難な場合は```common/```以下に新たにラッパーを作り、ラッパー経由で呼び出すようにする。
    
- マジックナンバーは使用せず、意味が分かるように定数変数を置いて使用すること
- (C#) DateTime ⇔ 文字列変換では、原則 `CultureInfo.InvariantCulture` を使用すること
- コーディングスタイルは、変更箇所周辺のコードスタイルに合わせる
- コードが縦に長くなりすぎないよう配慮すること (過剰に改行しない)
- (C++) constを積極的に使用する
