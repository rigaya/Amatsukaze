namespace Amatsukaze.Shared
{
    // エンコーダフィルタ設定のポップアップヘルプ文言
    // WPF(AmatsukazeGUI)とWebUI(AmatsukazeWebUI)の両方から参照して文言を共有する
    // 各メソッドの引数はenum名の文字列（WebUIはenum型を直接参照できないため）
    public static class EncoderFilterHelp
    {
        // インターレース解除
        public const string Deinterlace =
            "インターレース解除フィルタを有効にします。\n" +
            "選択するアルゴリズムによって出力フレームレートが変わります。";

        public static string DeinterlaceAlgorithm(string algorithm)
        {
            switch (algorithm)
            {
                case "Afs":
                    return "自動フィールドシフト (--vpp-afs)。\n" +
                        "比較的高速かつ24fps/30fpsが混在する状況にも対応できるため、\n" +
                        "iGPUなどでは迷ったらこれを選びます。\n" +
                        "右のプリセットで挙動が大きく変わります。";
                case "KFM":
                    return "KFM (--vpp-kfm)。\n" +
                        "逆テレシネと24/30/60fps混在への\n" +
                        "対応を行う高品質なインターレース解除です。\n" +
                        "dGPUではこちらがおすすめです。";
                case "NNEDI":
                    return "nnedi (--vpp-nnedi)。\n" +
                        "ニューラルネットによる補間で\n" +
                        "インターレース解除を行います。\n" +
                        "逆テレシネは行いません。";
                case "Yadif":
                    return "yadif (--vpp-yadif)。\n" +
                        "軽量な汎用インターレース解除です。\n" +
                        "逆テレシネは行いません。";
                case "Bwdif":
                    return "bwdif (--vpp-bwdif)。\n" +
                        "yadifの改良版で、yadifより若干高品質かつ同程度に高速です。\n" +
                        "逆テレシネは行いません。";
                case "Decomb":
                    return "decomb (--vpp-decomb)。\n" +
                        "フレーム単位で縞を検出し、縞のあるフレームだけを解除します。";
                case "IVTC":
                    return "逆テレシネ (--vpp-ivtc)。\n" +
                        "3:2プルダウンされたソフト/ハードテレシネ素材を24fpsに戻します。";
                default:
                    return Deinterlace;
            }
        }

        // インターレース解除アルゴリズムごとの追加パラメータ
        public static string DeinterlaceParam(string algorithm)
        {
            switch (algorithm)
            {
                case "Afs":
                    return "afsのプリセット。カッコ内は出力フレームレート。\n" +
                        "\n" +
                        "  default      … 標準 (30fps固定)\n" +
                        "  triple       … 動き重視・三重化 (30fps固定)\n" +
                        "  double       … 二重化 (VFR)\n" +
                        "  anime        … アニメ向け (VFR)\n" +
                        "  cinema       … 映画向け (VFR)\n" +
                        "  min_afterimg … 残像最小化 (VFR)\n" +
                        "  24fps        … 24fps化を強制 (24fps固定)\n" +
                        "  30fps        … フィールドシフトなし (30fps固定)\n" +
                        "\n" +
                        "VFRになるプリセットでは間引きが行われ、タイムコードが\n" +
                        "出力されてmux時に反映されます。";
                case "KFM":
                    return "kfmの出力モード。\n" +
                        "\n" +
                        "  vfr … 24/30/60fpsの混在をそのまま可変フレームレートで出力 (推奨)\n" +
                        "  60  … 60fps固定で出力\n" +
                        "  24  … 24fps固定で出力";
                case "NNEDI":
                case "Yadif":
                case "Bwdif":
                    return "  normal … 入力と同じフレームレートで出力します (60i→30p)\n" +
                        "  bob    … 2倍のフレームレートで出力します (60i→60p)\n" +
                        "           ファイルサイズとエンコード時間が増えます。";
                default:
                    return "";
            }
        }

        // ノイズ除去
        public const string Denoise =
            "ノイズ除去フィルタを有効にします。\n" +
            "地デジのブロックノイズやフィルムグレインの低減に使いますが、\n" +
            "かけすぎるとディテールが失われるので注意してください。";

        public static string DenoiseAlgorithm(string algorithm)
        {
            switch (algorithm)
            {
                case "PMD":
                    return "正則化PMD法 (--vpp-pmd)。輪郭を保持しつつ弱めに\n" +
                        "ノイズを除去します。軽量で副作用が少なく、\n" +
                        "地デジ素材の標準的な選択肢です。";
                case "KNN":
                    return "K近傍法 (--vpp-knn)。pmdより強めにノイズを除去します。\n" +
                        "強くしすぎるとディテールが潰れます。";
                case "NLMeans":
                    return "Non Local Means (--vpp-nlmeans)。品質の高い\n" +
                        "ノイズ除去ですが、処理は重めです。";
                case "HQDN3D":
                    return "HQDN3D (--vpp-hqdn3d)。空間方向と時間方向の\n" +
                        "両方でノイズを除去します。\n" +
                        "このアルゴリズムには強度指定がなく、既定値で動作します。";
                case "DenoiseDct":
                    return "DCTベースのノイズ除去 (--vpp-denoise-dct)。\n" +
                        "高品質ですが処理は重めです。\n" +
                        "強くすると輪郭がぼける副作用があります。";
                case "Smooth":
                    return "DCTベースの平滑化 (--vpp-smooth)。\n" +
                        "ブロックノイズやモスキートノイズの低減に有効です。";
                case "FFT3D":
                    return "FFTベースのノイズ除去 (--vpp-fft3d)。時間方向も含めた\n" +
                        "高品質な処理を行いますが、処理は重めです。";
                case "Convolution3D":
                    return "3次元ノイズ除去 (--vpp-convolution3d)。\n" +
                        "前後フレームを参照するため、動きの少ない映像で\n" +
                        "効果的です。";
                case "MSmooth":
                    return "ディテール保持型スムージング (--vpp-msmooth)。\n" +
                        "エッジを検出してエッジ以外だけを平滑化します。";
                default:
                    return Denoise;
            }
        }

        // ノイズ除去の強度指定欄
        public static string DenoiseValue(string algorithm)
        {
            switch (algorithm)
            {
                case "PMD":
                    return "1回あたりのフィルタの強さ。既定値は100。値が小さいほど弱くなります。";
                case "KNN":
                    return "フィルタの強さ。既定値は0.08。大きいほど強く除去されます。";
                case "NLMeans":
                    return "ノイズの分散。既定値は0.005。大きいほど強く除去されます。";
                case "DenoiseDct":
                    return "フィルタの強さ。既定値は4.0。大きいほど強いですが輪郭がぼけます。";
                case "Smooth":
                    return "フィルタの強さ(量子化パラメータ)。既定値は12。大きいほど強くなります。";
                case "FFT3D":
                    return "フィルタ強度。既定値は1.0。大きいほど強く除去されます。";
                case "Convolution3D":
                    return "輝度・色差の閾値。既定値は3。大きいほど強く除去されます。";
                case "MSmooth":
                    return "平滑化の反復回数。既定値は3。大きいほど強い平滑化になります。";
                default:
                    return "";
            }
        }

        // リサイズ
        public const string Resize =
            "出力解像度を指定します (--output-res)。\n" +
            "指定しない場合は入力解像度のまま出力されます。";

        // エッジ強調
        public const string EdgeEnhance =
            "輪郭強調フィルタを有効にします。\n" +
            "かけすぎるとリンギング(輪郭のギザつき)が発生し、\n" +
            "かえってビットレートを消費するので控えめの設定を推奨します。";

        public static string EdgeAlgorithm(string algorithm)
        {
            switch (algorithm)
            {
                case "EdgeLevel":
                    return "エッジレベル調整 (--vpp-edgelevel)。\n" +
                        "シュートを防ぎつつ輪郭を強調します。";
                case "Unsharp":
                    return "unsharp (--vpp-unsharp)。\n" +
                        "輪郭だけでなく細かいディテール全体を強調します。\n" +
                        "強さの既定値は0.5です。\n" +
                        "強くかけすぎると副作用が大きいため注意してください。";
                case "WarpSharp":
                    return "warpsharp (--vpp-warpsharp)。\n" +
                        "輪郭を細線化してシャープに見せるフィルタです。\n" +
                        "深度の既定値は16.0で、大きいほど効果が強まります。\n" +
                        "強くかけすぎると副作用が大きいため注意してください。";
                case "MSharpen":
                    return "エッジ選択型シャープニング (--vpp-msharpen)。\n" +
                        "エッジを検出してエッジ部分だけをシャープ化するため、\n" +
                        "平坦部のノイズを強調しにくいのが特徴です。\n" +
                        "強度の既定値は1.0です。";
                default:
                    return EdgeEnhance;
            }
        }

        // バンディング低減
        public const string Deband =
            "バンディング(階調飛び)低減フィルタを有効にします (--vpp-deband)。\n" +
            "空や暗いシーンのグラデーションに出る縞状のムラを軽減します。\n" +
            "\n" +
            "出力ビット深度を10bitにすると、より効果的に階調飛びを\n" +
            "抑えられます。";

        // 出力ビット深度
        // isSvtAv1がtrueのときのみ、svt-av1の入力ビット深度が優先される旨を追記する
        public static string OutputDepth(bool isSvtAv1)
        {
            var text =
                "エンコーダフィルタの出力ビット深度を指定します。\n" +
                "指定しない場合は入力のビット深度がそのまま維持されます。\n" +
                "10bitを指定するとフィルタ処理による階調の劣化を抑えられますが、" +
                "本エンコーダが10bit入力に対応している必要があります。\n" +
                "バンディング低減を併用する場合は10bitを推奨します。";
            if (isSvtAv1)
            {
                text +=
                    "\n\n※エンコーダにsvt-av1を選択している場合は例外として、" +
                    "この設定より「入力ビット深度」の指定が優先されます。" +
                    "「入力ビット深度」が「自動」のときのみ、この設定が使われます。";
            }
            return text;
        }

        // フィルタオプション(追加コマンド)
        public const string FilterOption =
            "--vpp-* 等のフィルタオプションを直接記述します。\n" +
            "ここでの指定は上の固定フィルタ設定より後ろに置かれるため、\n" +
            "同じオプションを指定した場合はこちらが優先されます。\n" +
            "\n" +
            "エンコーダがこのフィルタと同じ場合は、エンコーダオプションに\n" +
            "結合されて1プロセスで処理されます。";
    }
}
