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
	
  - ```docker_ubuntu2204```
  - ```docker_ubuntu2404```
  
    Linux用配布パッケージのビルド用のdockerfile。CIで使用。
  
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
  
- ```AmatsukazeUnitTest/```
- ```BatchHashChecker/```
- ```FileCutter/```
- ```googletest/```
- ```NicoJK18Client/```

  改造版では開発/変更は行わない。
  
## ビルド

- Windows

  ```publish.bat``` でビルド

- Linux

  ```doc/BuildLinux.md``` を参照

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
- コーディングスタイルは、変更箇所周辺のコードスタイルに合わせる
- コードが縦に長くなりすぎないよう配慮すること (過剰に改行しない)
- (C++) constを積極的に使用する
