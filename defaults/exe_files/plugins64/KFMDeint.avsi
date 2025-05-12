# KFMとKTGMCを使ったインタレース解除
# requires AvisynthNeo

# SVP用
SetFilterMTMode("SVSuper",1)
SetFilterMTMode("SVAnalyse",1)
SetFilterMTMode("SVSmoothFps",1)

function KFMDeint(clip src, int "mode", int "pass", string "preset", float "thswitch", bool "ucf", bool "nr", bool "svp",\
                            string "filepath", bool "cuda", int "dev", int "threads", bool "is120", bool "show") {

    mode = default(mode, 0) # 0:VFR 1:60fps 2:24fps, 3:3clips(24p,30p,60p), 4:VFR(60fps)
    pass = default(pass, 0) # 0:realtime 1:pre process(mode:0or2or4) 2:only timing(mode:0or4) 3:generate images
    preset = default(preset, (Height(src) >= 720) ? "Faster" : "Slower") # QTGMC preset
    thswitch = default(thswitch, 0.5)
    ucf = default(ucf, false) # Enable DecombUCF
    nr = default(nr, false) # Enable Noise Reduction with (K)SMDegrain
    svp = default(svp, false) # Enable SVP
    filepath = default(filepath, "kfmdeint")
    cuda = default(cuda, false) # Enable CUDA
    dev = default(dev, 0) # CUDA device index
    threads = default(threads, 6) # Number of CPU threads
    is120 = default(is120, false) # VFR frame timing
    show = default(show, false) # Show information
    
    ivtc = Select(mode, true, false, true, true, true) # Enable Inverse Telecine
    out24 = (mode == 2) # output 24fps
    out3clips = (mode == 3) # output 3 clips
    decimate = (mode == 0) # output decimated frames
    
    flagcrop = 6 # 上下のノイズ部分は除外する
    nrblk = (Height(src) >= 720) ? 32 : 16 # SMDegrain block size
    
    Assert(!(mode == 1 && pass != 0), "60fps mode does not support multi-pass")
    Assert(!(mode == 2 && pass == 2), "24fps mode does not support timing output pass")
    Assert(!(mode == 3 && pass == 2), "3clips mode does not support timing output pass")
    Assert(mode == 2 || mode == 4 || !svp, "only mode 2 and 4 support SVP")

    # CUDAデバイスチェック
    if(cuda) {
        cc = CUDAComputeCapability(dev)
        Assert(cc >= 35, "[KFM CUDA Error] Specified GPU: " + CUDADeviceName(dev) + " is not usable. (CC=" + string(cc) + " < 35)")
    }
    
    # SVP
    SmoothFps = function[threads](clip c, int num, int den) {
        c
        super_params="{pel:2,gpu:1}"
        analyse_params="""{block:{w:32,h:32}, 
            main:{search:{coarse:{distance:-10}}},
            refine:[{thsad:200}]}"""
        smoothfps_params="{rate:{num:"+string(num)+",den:"+string(den)+"},algo:21,cubic:1}"

        super = SVSuper(super_params)
        vectors = SVAnalyse(super, analyse_params).Prefetch(threads)
        SVSmoothFps(super, vectors, smoothfps_params, mt=1)
    }
    
    OnDEV = cuda ? function[dev](clip c, int "pre") { c.OnCUDA(default(pre, 1), dev) } : function(clip c) { c }

    # KFMPadは上下を4ピクセル（合計8ピクセル）拡張する
    pad = src.KFMPad()
    
    # キャッシュ節約のためsrcはpadベースにする
    src = pad.Crop(0,4,0,-4)

    ## 60fpsクリップを作る ##

    # QTGMC/KTGMCを呼び出す useFlagは 0:通常 1:前のフィールドだけを使って補間 2:後ろのフィールドだけを使って補間
    # useFlagはKTGMCのみ対応（QTGMCは無視される）
    # 動きベクトルなどは再利用するので0を最初に呼び出してから1,2を呼び出すこと
    xTGMC = function[preset, cuda, dev](clip c, int useFlag) {
        prevGlobals = (useFlag == 0) ? "Replace" : "Reuse"
        cuda ? c.KTGMC(Preset=preset, dev=dev, PrevGlobals=prevGlobals, useFlag=useFlag) : c.QTGMC(Preset=preset)
    }

    # 16 == (Blocksize - Overlap): ブロックサイズに合わせる
    call_xTGMC = function[preset, src, pad, xTGMC](int useFlag) {
        is_pad = (preset == "Faster" && (Height(src) % 16) == 8)
        srcq = is_pad ? pad : src
        p60 = srcq.xTGMC(useFlag)
        is_pad ? p60.Crop(0,4,0,-4) : p60
    }
    
    # 静止部分補間用
    stt = src.KAnalyzeStatic(30, 15, pad)

    clip60 = call_xTGMC(0).KMergeStatic(src, stt)
    
    if(ucf) {
        # ノイズ情報クリップ作成関数
        UCFNoise = function[pad, cuda, OnDev](clip src) {
            if(cuda) {
                # ノイズ判定の準備。外周4ピクセルは判定から除外。Crop後にCUDAで処理するのにAlign必須
                fields = src.SeparateFields().Crop(4,4,-4,-4).Align()
                # オリジナルのDecombUCFだとCrop前にbinomialBlurを掛けていたが、CropとAlignが多くなるので
                # Crop後に掛けちゃう。多少結果が変わるかもしれないけどあまり変わらないと思う
                # オリジナルのbinomialBlurは理論的にはp=2.5相当（ただ見比べるとフィルタ範囲の違いよる差は結構出てる）
                noise = fields.KGaussResize(p=2.5)
                # ノイズ判定のための情報を集計。KAnalyzeNoiseの結果はCPUからアクセスされるのでOnCUDAしておく
                src.KAnalyzeNoise(fields.KNoiseClip(noise), pad).OnDEV()
            }
            else {
                fields = src.SeparateFields().Crop(4,4,-4,-4)
                w = fields.Width()
                h = fields.Height()
                epsilon = 0.0001
                noise = fields.GaussResize( w,h, 0,0, w+epsilon,h+epsilon, p=2.5 )
                src.KAnalyzeNoise(fields.KNoiseClip(noise), pad)
            }
        }

        # DecombUCFパラメータ
        ucfparam = KDecombUCFParam()
        # ノイズ情報クリップ
        noise30 = src.UCFNoise()
        
        if(cuda) {
            before60 = call_xTGMC(1).KMergeStatic(src, stt)
            after60 = call_xTGMC(2).KMergeStatic(src, stt)
        }
        else {
            # CPU版は対応していないのでTDeintを使う
            #before60 = src.TDeint(chroma=true,mode=1,edeint=src.nnedi3(field=-2),emask=src.TMM2(mode=1, order=-1, field=-1))
            # と思ったけど用意するのが面倒なのでYadifmod2で代用
            before60 = src.Yadifmod2( mode=1, order=-1, field=-1, edeint=src.nnedi3(field=-2) )
            after60 = before60
        }
        
        if(show) {
            before60 = before60.Print("*** BOB BEFORE ***", y=20)
            after60 = after60.Print("*** BOB AFTER ***", y=20)
        }
    }
    
    if(ivtc) {

        if(show) {
            clip60 = clip60.Print("****** BOB ******", y=20)
        }

        ## 逆テレシネ解析 ##

        # KFM解析用クリップ
        super = src.KFMSuper(pad).KCleanSuper()

        # 30fpsのKFM解析用クリップ
        super30 = super.SelectEven()

        # テレシネ解析判定 KFMCycleAnalyzeはCUDA非対応なので注意
        mode = (pass >= 3) ? 2 : pass
        fmclip = super.KPreCycleAnalyze()
        fmclip = cuda ? fmclip.OnDEV() : fmclip.Prefetch(threads)
        fmclip = fmclip.KFMCycleAnalyze(src, mode=mode, filepath=filepath).OnCPU(1)
        
        if(pass == 1) {
            return fmclip
        }

        # 逆テレシネしたKFM解析用クリップ
        super24 = super.KTelecineSuper(fmclip)

        # 逆テレシネ 縞除去はノイズ低減のため
        #clip24 = src.KTelecine(fmclip).KFMPad().KRemoveCombe(super24)
        clip24 = pad.KTelecine(fmclip).KRemoveCombe(super24)

        if(ucf) {
            # DoubleWeaveしたクリップ 縞除去はノイズ低減のため
            #dweave = src.DoubleWeave().KFMPad().KRemoveCombe(super)
            dweave = pad.DoubleWeave().KRemoveCombe(super)

            if(show) {
                dweave = dweave.Print("** DOUBLE WEAVE **", y=20)
            }

            # DecombUCFを適用(24fps)
            clip24 = clip24.KDecombUCF24(ucfparam, fmclip, noise30, before60, after60, dweave)
        }

        if(nr) {
            if(cuda) {
                # 24fpsは軽めの設定で行う
                    clip24 = clip24.KSMDegrain(tr=1, blksize=nrblk, binomial=True, dev=dev)
                }
            else {
                clip24 = clip24.SMDegrain(tr=1, blksize=nrblk)
            }
        }

        # 24fps/60fps 切り替えマスク（maskとccをセットで使う）
        flag24 = super24.KSwitchFlag()
        mask24 = clip24.KCombeMask(flag24)
        cc24 = flag24.Crop(0,flagcrop,0,-flagcrop).KContainsCombe().OnDEV() # CPUからアクセスされるのでOnCUDAしておく

        if(out24) {
            clip24.KPatchCombe(clip60, fmclip, mask24, cc24)
        }
        else {
            clip30 = show ? src.Print("****** SOURCE *****", y=20) : src
            if(ucf) {
                # DecombUCFを適用(30fps)
                clip30 = clip30.KDecombUCF(ucfparam, noise30, before60, after60)
            }
            if(nr) {
                if(cuda) {
                    clip30 = clip30.KSMDegrain(tr=1, blksize=nrblk, binomial=True, dev=dev)
                }
                else {
                    clip30 = clip30.SMDegrain(tr=1, blksize=nrblk)
                }
            }

            # 30fps/60fps 切り替えマスク（maskとccをセットで使う）
            flag30 = super30.KSwitchFlag()
            mask30 = clip30.KCombeMask(flag30)
            cc30 = flag30.Crop(0,flagcrop,0,-flagcrop).KContainsCombe().OnDEV() # CPUからアクセスされるのでOnCUDAしておく

            # DecombUCFを適用(60fps)
            ucfc = ucf ? clip60.KDecombUCF60(ucfparam, noise30, before60, after60) : Undefined()
            
            if(out3clips) {
                # クリップを3つ用意
                clip24 = clip24.KPatchCombe(clip60, fmclip, mask24, cc24)
                clip30 = clip30.KPatchCombe(clip60, fmclip, mask30, cc30)
                clip60 = ucf ? ucfc : clip60
            }
            else {
				# SVPを適用
				if(svp) {
					# SVPをマルチスレッドで呼び出すとバグるので注意
					clip24 = clip24.OnDEV().SmoothFps(5, 2).OnCPU(1)
					clip30 = clip30.OnDEV().SmoothFps(2, 1).OnCPU(1)
				}
				else if(!cuda) {
					clip24 = clip24.Prefetch(threads)
					clip30 = clip30.Prefetch(threads)
				}
            
				if(!cuda) {
					# KFMSwitch後にPrefetchすると効率が落ちるのでここでやる
					clip60 = clip60.Prefetch(threads)
					mask24 = mask24.Prefetch(threads / 2)
					cc24 = cc24.Prefetch(mode == 2 ? threads : (threads / 2))
					mask30 = mask30.Prefetch(threads / 2)
					cc30 = cc30.Prefetch(mode == 2 ? threads : (threads / 2))
					ucfc = ucf ? ucfc.Prefetch(threads / 2) : Undefined()
				}
            
				mode = (pass == 2) ? 2 : 0
				clip60.KFMSwitch(fmclip, clip24, mask24, cc24, clip30, mask30, cc30, ucfc, thswitch=thswitch, mode=mode, filepath=filepath, is120=is120, show=show)
				
				if(decimate && pass == 3) {
					KFMDecimate(filepath=filepath)
				}
            }
        }
    }
    else {
        ucf ? clip60.KDecombUCF60(ucfparam, noise30, before60, after60) : clip60
    }
    
    if(cuda) {
        if(out3clips) {
            clip24 = clip24.OnDEV(2)
            clip30 = clip30.OnDEV(2)
            clip60 = clip60.OnDEV(2)
        }
        else {
			OnDEV(2)
        }
    }
    else if(out24) {
        # 24fps出力時のSVPはここで適用
        svp ? SmoothFps(5, 2) : Prefetch(threads)
    }
    else {
        if(out3clips) {
            clip24 = clip24.Prefetch(4)
            clip30 = clip30.Prefetch(4)
            clip60 = clip60.Prefetch(4)
        }
        else {
			Prefetch(1, 4)
        }
    }
    
    if(out3clips) {
        return function[clip24, clip30, clip60](int fps) {
            if(fps == 24) {
                return clip24
            }
            else if(fps == 30) {
                return clip30
            }
            else if(fps == 60) {
                return clip60
            }
            Assert(false, "fps must be 24 or 30 or 60")
        }
    }
}
