---
name: add-profile-option
description: Amatsukazeのプロファイル設定（ProfileSetting）に新しいオプションを追加する。WPF GUI・Web UI・C#サーバー・C++ CLI/コア処理まで一連のレイヤーに過不足なく実装するための手順書。bool/int/string/enum等のシンプルなプロパティ追加を対象。
disable-model-invocation: true
---

## 目的

Amatsukazeのプロファイル設定に新しいオプションを 1 つ追加する。プロパティ追加は WPF GUI・Web UI・C# サーバー・C++ CLI・C++ コア処理まで複数レイヤーを横断するため、抜けやすい。本skillはチェックリスト形式で漏れを防ぐ。

## 編集ファイル一覧（チェックリスト）

新オプション 1 つ追加するために、最低でも以下のファイル群を編集する。**1 つでも漏れると UI 操作が反映されない / CLI に伝わらない / text dump に出ない等の不具合**になる。

### 必須編集ファイル（典型的にはこの 12 ファイル）

**C++ 側（6 ファイル）**

| # | ファイル | 編集内容 |
|---|---|---|
| 1 | `Amatsukaze/Amatsukaze/TranscodeSetting.h` | `Config` 構造体にメンバ追加、`ConfigWrapper` に getter 宣言追加、必要なら自由関数（`makeMuxerArgs` 等）の引数追加 |
| 2 | `Amatsukaze/Amatsukaze/TranscodeSetting.cpp` | `ConfigWrapper::getXxx()` の実装追加、`makeMuxerArgs` 等の自由関数の処理修正 |
| 3 | `Amatsukaze/Amatsukaze/AmatsukazeCLI.hpp` | ヘルプテキスト追加、デフォルト値設定、`--xxx` パース処理追加 |
| 4 | `Amatsukaze/Amatsukaze/Encoder.h` | エンコーダ処理に影響する場合、関連クラスのコンストラクタ引数追加 |
| 5 | `Amatsukaze/Amatsukaze/Encoder.cpp` | エンコーダ処理での効果実装、**並列エンコード経路全てに伝搬** |
| 6 | `Amatsukaze/Amatsukaze/Muxer.cpp` | muxer 処理に影響する場合、`makeMuxerArgs` への引数伝搬 |

**C# 側（5 ファイル）**

| # | ファイル | 編集内容 |
|---|---|---|
| 7 | `AmatsukazeServer/Server/EncodeServerData.cs` | `ProfileSetting` クラスに `[DataMember]` プロパティ追加、`ToLongString()` に text dump 1 行追加 |
| 8 | `AmatsukazeServer/Server/EncodeServer.cs` | プロファイル → CLI 引数生成箇所で `--xxx` フラグ付与 |
| 9 | `AmatsukazeGUI/Models/DisplayData.cs` | `DisplayProfile` に変更通知付きラッパープロパティ追加 |
| 10 | `AmatsukazeGUI/Models/ClientModel.cs` | **DisplayProfile → ProfileSetting の手動コピー処理に 1 行追加（最も忘れやすい）** |
| 11 | `AmatsukazeGUI/Views/ProfileSettingPanel.xaml` | WPF UI コントロール追加（CheckBox/TextBox 等） |

**Web UI 側（1 ファイル）**

| # | ファイル | 編集内容 |
|---|---|---|
| 12 | `AmatsukazeWebUI/Pages/ProfileSettings.razor` | テーブル行 `<tr>` 追加 |

### 編集対象外（基本変更不要）

- `ServerSupport.NormalizeProfile` (`AmatsukazeServer/Server/Misc.cs`): bool型の default は false で自動初期化されるため通常追加不要
- `ServerSupport.DeepCopy` (`Misc.cs`): `DataContractSerializer` 経由なので `[DataMember]` 付きは自動で含まれる
- `defaults/` 配下: デフォルト設定として明示したい場合のみ

### オプションの種類による追加考慮箇所

- **エンコーダ動作を変える**: `Encoder.cpp` の `Y4MWriter`/`Y4MEncodeWriter` 系、または `TranscodeSetting.cpp::makeEncoderArgs()`
- **muxer 動作を変える**: `TranscodeSetting.cpp::makeMuxerArgs()`
- **フィルタ動作を変える**: `FilteredSource.cpp`
- **タスク制御に影響**: `TranscodeManager.cpp`
- **CMチェック/字幕処理等**: 該当する処理ファイル

## 全体像（編集が必要なレイヤー）

```
[UI層]
  ├── WPF: ProfileSettingPanel.xaml                  ... CheckBox/TextBox 等のコントロール追加
  └── Web: ProfileSettings.razor                     ... テーブル行追加
       ↓ バインディング
[ViewModel層 / クライアント]
  ├── DisplayData.cs (DisplayProfile)                ... ラッパープロパティ + RaisePropertyChanged
  └── ClientModel.cs                                 ... DisplayProfile → ProfileSetting 手動コピー
       ↓ サーバー送信
[Model層]
  └── EncodeServerData.cs (ProfileSetting)           ... [DataMember] プロパティ + ToLongString
       ↓
[サーバー → CLI 変換]
  └── EncodeServer.cs                                ... プロパティ値 → CLI フラグ文字列に変換
       ↓ コマンドライン
[CLI パース]
  └── AmatsukazeCLI.hpp                              ... ヘルプ、デフォルト値、--option パース
       ↓
[C++ Config]
  ├── TranscodeSetting.h (Config構造体, ConfigWrapper)  ... メンバ追加 + getter 宣言
  └── TranscodeSetting.cpp (ConfigWrapper)           ... getter 実装
       ↓ 各処理での参照
[C++ コア処理]
  └── Encoder.cpp / Muxer.cpp / TranscodeSetting.cpp ... 実際にオプションの効果を実装
```

## 既存パターンの参考実装

新オプション追加時は既存の **シンプルなboolオプション** をテンプレートとして真似るのが最も確実。

- **`MuxerAddEncoderCmd`** (`--muxer-add-encoder-cmd`): bool型の標準例。UI〜CLI〜C++ Configまで一通り追っている。
- **`SARInContainerOnly`** (`--sar-in-container-only`): 上記に加え C++ コア処理（Encoder.cpp/Muxer.cpp/TranscodeSetting.cpp の makeMuxerArgs）まで踏み込む例。

新オプション追加時は、まず該当パターンを grep で全箇所抽出して、その隣に同形式で追加するのが安全。

```bash
grep -rn "MuxerAddEncoderCmd" Amatsukaze/Amatsukaze AmatsukazeServer AmatsukazeGUI AmatsukazeWebUI
```

## 手順チェックリスト

仮にプロパティ名を `NewOption`（bool型）、CLI名を `--new-option` として記述する。実際の追加時は適切な名称に置き換えること。

### 1. C++ Config（基盤）

**`Amatsukaze/Amatsukaze/TranscodeSetting.h`**

- `struct Config` にメンバ追加（`MuxerAddEncoderCmd` の隣あたり、コメント付き）
  ```cpp
  // 新オプションの説明
  bool newOption;
  ```
- `class ConfigWrapper` に getter 宣言追加
  ```cpp
  bool getNewOption() const;
  ```
- もし `makeMuxerArgs` 等の自由関数に渡す必要があるなら、宣言（引数リスト）にも追加

**`Amatsukaze/Amatsukaze/TranscodeSetting.cpp`**

- `ConfigWrapper::getNewOption()` の実装追加
  ```cpp
  bool ConfigWrapper::getNewOption() const {
      return conf.newOption;
  }
  ```

### 2. C++ CLI パース

**`Amatsukaze/Amatsukaze/AmatsukazeCLI.hpp`**

- ヘルプテキスト追加（`--muxer-add-encoder-cmd` の隣）
  ```cpp
  "  --new-option         新オプションの説明\n"
  ```
- `parseArgs()` 内のデフォルト値設定
  ```cpp
  conf.newOption = false;
  ```
- オプション解析の `else if` 分岐追加
  ```cpp
  } else if (key == _T("--new-option")) {
      conf.newOption = true;
  ```
- 値を取る引数の場合は `getParam(argc, argv, i++)` で取得

### 3. C++ コア処理（オプションの効果実装）

オプションの種類によって編集箇所は異なる。代表例:

- **エンコーダ引数に影響**: `Encoder.cpp` の `Y4MEncodeWriter` / `Y4MWriter` コンストラクタ、または `TranscodeSetting.cpp` の `makeEncoderArgs()`
- **muxer引数に影響**: `TranscodeSetting.cpp` の `makeMuxerArgs()`
- **フィルタ処理に影響**: `FilteredSource.cpp`
- **タスク制御に影響**: `TranscodeManager.cpp`

注意点:
- `setting_->getNewOption()` や `setting_.getNewOption()` で取得
- 引数経由で下流に渡す場合、**並列エンコード経路（`encodeSWParallel`, `StdinHeaderWriter`, `PipeY4MWriter` 等）にも漏れなく伝搬すること**
- `VideoFormat` 等の入力データ構造を書き換えると下流に影響が波及するため、**局所変数で処理する** のが安全

### 4. C# サーバー側 Model

**`AmatsukazeServer/Server/EncodeServerData.cs`**

- `class ProfileSetting` にプロパティ追加（`MuxerAddEncoderCmd` の隣）
  ```csharp
  /// <summary>新オプションの説明</summary>
  [DataMember]
  public bool NewOption { get; set; }
  ```
  `[DataMember]` 必須。`IExtensibleDataObject` のおかげで旧プロファイルXMLとの互換性は保たれる（bool は default false）。

- `ProfileSettingExtensions.ToLongString()` 内の text dump に追加（`keyValueBool` 例）
  ```csharp
  keyValueBool("新オプションの説明", profile.NewOption);
  ```
  これは `.profile.txt` ファイルやGUIのクリップボード出力に反映される。

### 5. C# サーバー側 CLI 引数生成

**`AmatsukazeServer/Server/EncodeServer.cs`**

CLI コマンドライン構築箇所（`MuxerAddEncoderCmd` の隣あたり）に追加:
```csharp
if (profile.NewOption)
{
    sb.Append(" --new-option");
}
```

### 6. C# クライアント側 ViewModel

**`AmatsukazeGUI/Models/DisplayData.cs`**

`class DisplayProfile` 内に変更通知付きラッパープロパティを追加（`MuxerAddEncoderCmd` の隣）:
```csharp
#region NewOption変更通知プロパティ
public bool NewOption
{
    get { return Data.NewOption; }
    set
    {
        if (Data.NewOption == value)
            return;
        Data.NewOption = value;
        RaisePropertyChanged();
    }
}
#endregion
```

### 7. C# クライアント側 コピー処理（**忘れやすい**）

**`AmatsukazeGUI/Models/ClientModel.cs`**

`DisplayProfile.Data` → サーバー送信用 `ProfileSetting` への手動コピー処理（`MuxerAddEncoderCmd` の隣）に追加:
```csharp
profile.NewOption = data.Profile.NewOption;
```

このコピー処理を忘れるとUI操作が反映されず、デバッグに時間を取られる。**最優先で確認すべきポイント**。

なお `ServerSupport.DeepCopy` (`Misc.cs`) は `DataContractSerializer` 経由なので `[DataMember]` 付きプロパティは自動的に含まれる → こちらは追加作業不要。

### 8. WPF UI

**`AmatsukazeGUI/Views/ProfileSettingPanel.xaml`**

`MuxerAddEncoderCmd` の `<CheckBox>` の隣に追加:
```xaml
<CheckBox Margin="0,6" Content="新オプションの説明"
          IsChecked="{Binding Model.SelectedProfile.NewOption, Mode=TwoWay}"
          ToolTip="補足説明（適用条件など）"/>
```

### 9. Web UI

**`AmatsukazeWebUI/Pages/ProfileSettings.razor`**

`MuxerAddEncoderCmd` の `<tr>` の隣に追加:
```razor
<tr>
    <th>新オプション<br />の説明</th>
    <td>
        <label style="margin-right: 6px;" title="補足説明">
            <input type="checkbox" checked="@GetBoolValue("NewOption")" @onchange="@(e => OnBoolChanged(e, "NewOption"))" />
            有効
        </label>
        <span class="text-muted small">補足説明</span>
    </td>
</tr>
```

## 型ごとの注意点

| 型 | DisplayData getter/setter | ToLongString | Web UI ヘルパー |
|---|---|---|---|
| bool | `Data.X` 直返し | `keyValueBool("...", profile.X)` | `GetBoolValue` / `OnBoolChanged` |
| int | 同上 | `keyValue("...", profile.X.ToString())` | `GetIntValue` / `OnIntChanged` 等 |
| string | 同上 | `keyValue("...", profile.X ?? "")` | `GetStringValue` / `OnStringChanged` |
| enum | 同上 | `keyValue("...", profile.X.ToString())` | ComboBox + 対応ヘルパー |

実際のヘルパー名は既存の Razor コードを `grep` で確認すること。

## ビルド確認

```bash
(cd /home/rigaya/dev/Amatsukaze/Amatsukaze && ./scripts/build.sh ../build ../build_tmp)
```

C++ / C# / Web UI 全部ビルドが通ることを確認。エラーが出やすい箇所:

- C++ 側で `Encoder.h` のクラス宣言に引数追加を忘れると implementation 側でリンクエラー
- C# 側で `[DataMember]` を忘れるとサーバー/クライアント間で値が伝わらない（実行時バグ。ビルドは通る）
- WPF XAML のバインディング名のtypoはビルドは通るが実行時バインディングエラー

## レビュー時のチェックポイント

実装後の自己レビューで確認すべきこと:

1. **編集ファイルは 8〜12 ファイルになっているか**（少なすぎる場合は漏れの可能性）
2. **`grep -rn "<参考オプション名>"` の出現箇所全てに新オプションも並んでいるか**
3. **`ClientModel.cs` のコピー処理に追加されているか**（最も忘れやすい）
4. **`ToLongString()` に追加されているか**（text dump 漏れ）
5. **C++ コア処理で並列エンコード経路（chunkエンコーダ等）にもフラグが伝搬しているか**
6. **`outfmt` や `VideoFormat` 等の共有データを書き換えていないか**（局所処理に留める）
7. **ビルドが警告ゼロまたは既存警告のみで通るか**

## 動作確認

実機テストでは:

- **オプション無効時**: 旧挙動完全維持。既存のテストケースで差分が出ないこと
- **オプション有効時**: 期待動作になっていること
- **GUI/Web UI 両方** からプロファイル保存→読み込み→再表示で値が保持されること
- **`.profile.txt`** に新項目が出力されていること
- **旧プロファイルXML** を読み込んでもエラーにならない（default 値で読まれる）こと
