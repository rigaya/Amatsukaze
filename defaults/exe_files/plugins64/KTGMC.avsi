#-------------------------------------------------------------------#
#                                                                   #
#                  KTGMC 0.2, by Nekopanda, 2017                    #
#                                                                   #
#        base on QTGMC 3.357, by Vit, 2012, 2017 mod by A.SONY      #
#                                                                   #
#          CUDA accelerated high quality deinterlacer               #
#         using motion-compensated temporal smoothing,              #
#       with a range of features for quality and convenience        #
#          Originally based on TempGaussMC_beta2 by Didée           #
#                                                                   #
#-------------------------------------------------------------------#
#
# Full documentation is in the 'QTGMC' html file that comes with this script
#
# --- LATEST CHANGES ---
#
# KTGMC v0.2
# - add parameter "dev" and "analyzeBatch"
#
# KTGMC v0.1
# - initial release of KTGMC
#
# v3.357s
# - high bit depth now basically work in avs+
#
# v3.354s
# - added optional chikuzen PlanarTools for YUY2 using nonyuy2clipin/nonyuy2clipout in AnimeIVTC() v2.20
#
# v3.353s
# - added ndjamena suggestion for maximum temporal radius
# - added DftDither to controle dfttest dither Parameter
#
# v3.352s
# - added useEdiExt
# - added tr0=-1 to use the EdiExt clip (if there is no EdiExt clip then it will not work)
# - rep0 now will be 0 if tr0 < 1
#
# v3.347s
# - fix SourceMatch (bug after new YUY2 method, thanks to ndjamena for report it)
#
# v3.346s
# - EEDI3+NNEDI3 bug fix by ndjamena
# - Renamed DftThreads parameter to FftThreads, to set the ncpu argument of FFT3DFilter (from VS)
# - FFT3DFilter will work now in YUY2, use it with Denoiser="fft3df" (or use NoisePreset)
#
# v3.345s
# - work with SMDegrain_KNLMeansCL now to process chroma too
#
# v3.343s
# - edit in ChromaEdi
# - others
#
# v3.34s (mod) 2016 12 18 (ravewulf idea in another way)
# - Added yadifmod2 (if you need old one use it like this EdiMode="cYadif" or EdiMode="RepcYadif")
# - fast and clean up YUY2 (will work in x64 now, no need to SSETools)
#
# v3.33s (mod) 2016 12 06
# - Add TR2=4 and TR2=5
# - add TV_range bool and dither_luma_rebuild (from SMDegrain) Str and Amp
# - for avsplus now QTGMC_Bob is ready for any 420, not only yv12
#
# v3.33s (mod) 2016 04 11
# - Add KNLMeansCL as another Denoiser to NoiseProcess
# - others
#
# v3.33s (mod) 2016 01 19
# - make QTGMC_deflate/QTGMC_inflate work with YUY2 using masktool2 for avs 2.60
#
# v3.33s (mod) 2015 10 23
# - revert to QTGMC_deflate/QTGMC_inflate quoted from Dogway
#
# v3.33s (mod) 2015 9 10
# - fix Multiple QTGMC Calls
# - others fix
#
# v3.33s (mod) 2015 8 6
# - some changes in bob to speed up
#
# v3.33s (mod) 2015 8 4
# - fix bug in YUY2 with SourceMatch
# - add slice=false to ditherpost to avoid artefacts
#
# v3.33s (mod)
# - fix bob chroma
# - work with new masktool2 for avs 2.60 in YUY2
#
# v3.33d (mod)
# - Added 32 bit precision option through the lsbd (for dfttest and knlmeanscl) and lsb (mdegrain) options.
# - optimized some mask handling a bit as originally suggested by Vit
# - others
#
# v3.33
# - Increased maximum value for Rep0, Rep1 and Rep2 to 7 (from 5). Higher values help with flicker on static detail, potential for minor motion blur
# - Bug fix for the fact that Bob always outputs a BFF clip regardless of field order of input (thanks ajp_anton)
# - Improved generation of noise (NoiseDeint="Generate") for noise bypass / EZKeepGrain
# - Minor change to denoising
#
# v3.32
# - Bugfix with shutter blur and ChromaMotion (thanks Heaud)
# - Tweaked vector recalculation for shutter motion blur
# - Changed defaults for TR2 when using source-match
# - Minor bugfix with SLMode/SLRad on pass-through settings
#
# --- REQUIREMENTS ---
#
# Input colorspaces: YV12, YUY2, others as plugins supports
#
# Core plugins:
#	MVTools2 (2.7.5.22 or above)
#	MaskTools2 (v2.2.5 or above)
#	NNEDI3 (0.9.4.6 or above)
#	RgTools
#
# Core scripts:
#	SMDegrain (3.1.2.91s or above) unless you set tv_range=false
#	AnimeIVTC (2.20 or above) if you work with YUY2 source
#
# Additional plugins:
#	NNEDI2, NNEDI, EEDI3, EEDI2, TDeInt - if selected directly or via a source-match preset
#	yadifmod2 - for Preset="Ultra Fast" or if selected directly
#	FFT3DFilter - if selected for noise processing
#	dfttest (1.9.4 or above) - if selected for noise processing
#		For FFT3DFilter & ddftest you also need the FFTW3 library (FFTW.org). On Windows the file needed for both is libfftw3f-3.dll. However, for FFT3DFilter
#		the file needs to be called FFTW3.dll, so you will need two copies and rename one. On Windows put the files in your System32 or SysWow64 folder
#	KNLMeansCL - if selected for noise processing
#	AddGrainC - if NoiseDeint="Generate" selected for noise bypass
#	chikuzen PlanarTools - if you work with YUY2 source (not necessarily required but may improve speed)


# --- GETTING STARTED ---
#
# Install AviSynth and ensure you have at least the core plugins listed in the requirements section above. Put them in the plugins autoload folder.
# To use QTGMC write a script like this:
#	YourSource("yourfile")   # DGDecode_mpeg2source, FFVideoSource, AviSource, whatever your source requires
#	QTGMC( Preset="Slow" )
#	SelectEven()             # Add this line to keep original frame rate, leave it out for smoother doubled frame rate
#
# Save this script with an ".avs" extension. You can now use it as an AVI source for encoding.
#
# The "Preset" used selects sensible settings for a given encoding speed. Choose a preset from:
#	"Placebo", "Very Slow", "Slower", "Slow", "Medium", "Fast", "Faster", "Very Fast", "Super Fast", "Ultra Fast" & "Draft"
# The default preset is "Slower"
# Don't be obsessed with using slower settings as the differences can be small. HD material benefits little from extreme settings (and will be very slow)
# For much faster speeds read the full documentation, the section on 'Multi-threading'
#
# There are many settings for tweaking the script, full details in the main documentation. You can display settings currently being used with "ShowSettings":
#	QTGMC( Preset="Slow", ShowSettings=true )


function KTGMC( clip Input, string "Preset", int "TR0", int "TR1", int "TR2", int "Rep0", int "Rep1", int "Rep2", string "EdiMode", bool "RepChroma", \
                    int "NNSize", int "NNeurons", int "EdiQual", int "EdiMaxD", string "ChromaEdi", int "EdiThreads", clip "EdiExt", float "Sharpness", \
                    int "SMode", int "SLMode", int "SLRad", int "SOvs", float "SVThin", int "Sbb", int "SrchClipPP", int "SubPel", int "SubPelInterp", \
                    int "BlockSize", int "Overlap", int "Search", int "SearchParam", int "PelSearch", bool "ChromaMotion", bool "TrueMotion", int "Lambda", \
                    int "LSAD", int "PNew", int "PLevel", bool "GlobalMotion", int "DCT", int "ThSAD1", int "ThSAD2", int "ThSCD1", int "ThSCD2", \
                    int "SourceMatch", string "MatchPreset", string "MatchEdi", string "MatchPreset2", string "MatchEdi2", int "MatchTR2", \
                    float "MatchEnhance", int "Lossless", int "NoiseProcess", float "EZDenoise", float "EZKeepGrain", string "NoisePreset", string "Denoiser", \
                    int "FftThreads", bool "DenoiseMC", int "NoiseTR", float "Sigma", bool "ChromaNoise", val "ShowNoise", float "GrainRestore", \
                    float "NoiseRestore", string "NoiseDeint", bool "StabilizeNoise", int "InputType", float "ProgSADMask", int "FPSDivisor", \
                    int "ShutterBlur", float "ShutterAngleSrc", float "ShutterAngleOut", int "SBlurLimit", bool "Border", bool "Precise", string "Tuning", \
                    bool "ShowSettings", string "GlobalNames", string "PrevGlobals", int "ForceTR", \
                    val "BT", val "DetailRestore", val "MotionBlur", val "MBlurLimit", val "NoiseBypass", float "Str", float "Amp", bool "TV_range", bool "useEdiExt", int "DftDither", bool "lsbd", bool "lsb", \
					int "useFlag", int "dev", int "analyzeBatch" )
{
	# EdiMode="RepcYadif"/"cYadif" require the Yadif plugin, which doesn't autoload. Typically the calling script would load it.

	# Temporary Warnings
	Assert( !defined(BT),            "KTGMC: Setting BT has been replaced by setting NoiseTR" )
	Assert( !defined(DetailRestore), "KTGMC: Setting DetailRestore has been renamed to GrainRestore" )
	Assert( !defined(MotionBlur),    "KTGMC: Setting MotionBlur has been renamed to ShutterBlur" )
	Assert( !defined(MBlurLimit),    "KTGMC: Setting MBlurLimit has been renamed to SBlurLimit" )
	Assert( !defined(NoiseBypass),   "KTGMC: Setting NoiseBypass has been renamed to NoiseProcess" )

	#---------------------------------------
	# Presets
    lsbd         = default( lsbd,      false)
    lsb          = default( lsb,       false)
    useEdiExt    = default( useEdiExt, false)
    Str          = default( Str,        1.0 )
    Amp          = default( Amp,     0.0625 )
    TV_range     = default( TV_range,   !(Isrgb(input)))

        sispmt = Findstr(VersionString(), "AviSynth+") != 0 && Findstr(VersionString(), "r1576") == 0
	sispmt ? Assert( !(Input.BitsPerComponent() > 8 && (lsb || lsbd)), "KTGMC: lsb hack is not Compatible with native high bit depth" ) : nop()

	# Select presets / tuning
	Preset = default( Preset, "Slower" )
	pNum = (Preset == "Placebo"   ) ? 0 : \
	       (Preset == "Very Slow" ) ? 1 : \
	       (Preset == "Slower"    ) ? 2 : \
	       (Preset == "Slow"      ) ? 3 : \
	       (Preset == "Medium"    ) ? 4 : \
	       (Preset == "Fast"      ) ? 5 : \
	       (Preset == "Faster"    ) ? 6 : \
	       (Preset == "Very Fast" ) ? 7 : \
	       (Preset == "Super Fast") ? 8 : \
	       (Preset == "Ultra Fast") ? 9 : \
	       (Preset == "Draft"     ) ? 10 : 11
	Assert( pNum  < 11, "'Preset' choice is invalid" )

	mpNum1 = (!defined(MatchPreset))       ? ((pNum + 3 <= 9) ? (pNum + 3) : 9) : \
	         (MatchPreset == "Placebo"   ) ? 0 : \
	         (MatchPreset == "Very Slow" ) ? 1 : \
	         (MatchPreset == "Slower"    ) ? 2 : \
	         (MatchPreset == "Slow"      ) ? 3 : \
	         (MatchPreset == "Medium"    ) ? 4 : \
	         (MatchPreset == "Fast"      ) ? 5 : \
	         (MatchPreset == "Faster"    ) ? 6 : \
	         (MatchPreset == "Very Fast" ) ? 7 : \
	         (MatchPreset == "Super Fast") ? 8 : \
	         (MatchPreset == "Ultra Fast") ? 9 : \
	         (MatchPreset == "Draft"     ) ? 10 : 11
	Assert( mpNum1 < 10, "'MatchPreset' choice is invalid/unsupported" )
	MatchPreset = Select( mpNum1, "Placebo", "Very Slow", "Slower", "Slow", "Medium", "Fast", "Faster", "Very Fast", "Super Fast", "Ultra Fast", "Draft" )

	mpNum2 = (!defined(MatchPreset2))       ? ((mpNum1 + 2 <= 9) ? (mpNum1 + 2) : 9) : \
	         (MatchPreset2 == "Placebo"   ) ? 0 : \
	         (MatchPreset2 == "Very Slow" ) ? 1 : \
	         (MatchPreset2 == "Slower"    ) ? 2 : \
	         (MatchPreset2 == "Slow"      ) ? 3 : \
	         (MatchPreset2 == "Medium"    ) ? 4 : \
	         (MatchPreset2 == "Fast"      ) ? 5 : \
	         (MatchPreset2 == "Faster"    ) ? 6 : \
	         (MatchPreset2 == "Very Fast" ) ? 7 : \
	         (MatchPreset2 == "Super Fast") ? 8 : \
	         (MatchPreset2 == "Ultra Fast") ? 9 : \
	         (MatchPreset2 == "Draft"     ) ? 10 : 11
	Assert( mpNum2 < 10, "'MatchPreset2' choice is invalid/unsupported" )
	MatchPreset2 = Select( mpNum2, "Placebo", "Very Slow", "Slower", "Slow", "Medium", "Fast", "Faster", "Very Fast", "Super Fast", "Ultra Fast", "Draft" )

	NoisePreset = default( NoisePreset, "Fast" )
	npNum = (NoisePreset == "Slower" ) ? 0 : \
	        (NoisePreset == "Slow"   ) ? 1 : \
	        (NoisePreset == "Medium" ) ? 2 : \
	        (NoisePreset == "Fast"   ) ? 3 : \
	        (NoisePreset == "Faster" ) ? 4 : 5
	Assert( npNum < 5, "'NoisePreset' choice is invalid" )

	Tuning = default( Tuning, "None" )
	tNum = (Tuning == "None"  ) ? 0 : \
	       (Tuning == "DV-SD" ) ? 1 : \
	       (Tuning == "DV-HD" ) ? 2 : 3
	Assert( tNum < 3, "'Tuning' choice is invalid" )

	# Tunings only affect blocksize in this version
	bs = Select( tNum,  16, 16, 32 )
	bs2 = (bs >= 16) ? 32 : bs * 2

	#                                                               Very                                                        Very      Super      Ultra
	# Preset groups:                                     Placebo    Slow      Slower    Slow      Medium    Fast      Faster    Fast      Fast       Fast       Draft
	TR0          = default( TR0,          Select( pNum,  2,         2,        2,        2,        2,        2,        1,        1,        1,         1,         0      ) )
	TR1          = default( TR1,          Select( pNum,  2,         2,        2,        1,        1,        1,        1,        1,        1,         1,         1      ) )
	TR2X         = default( TR2,          Select( pNum,  3,         2,        1,        1,        1,        0,        0,        0,        0,         0,         0      ) )
	Rep0         = defined(Rep0) ? Rep0 : TR0 < 1 ? 0 : undefined()
	Rep0         = default( Rep0,         Select( pNum,  4,         4,        4,        4,        3,        3,        0,        0,        0,         0,         0      ) )
	Rep1         = default( Rep1,         Select( pNum,  0,         0,        0,        0,        0,        0,        0,        0,        0,         0,         0      ) )
	Rep2         = default( Rep2,         Select( pNum,  4,         4,        4,        4,        4,        4,        4,        4,        3,         3,         0      ) )
	EdiMode      = default( EdiMode,      Select( pNum, "NNEDI3",  "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3",  "RepYadif","Bob"   ) )
	NNSize       = default( NNSize,       Select( pNum,  1,         1,        1,        1,        5,        5,        4,        4,        4,         4,         4      ) )
	NNeurons     = default( NNeurons,     Select( pNum,  2,         2,        1,        1,        1,        0,        0,        0,        0,         0,         0      ) )
	EdiQual      = default( EdiQual,      Select( pNum,  1,         1,        1,        1,        1,        1,        1,        1,        1,         1,         1      ) )
	EdiMaxD      = default( EdiMaxD,      Select( pNum,  12,        10,       8,        7,        7,        6,        6,        5,        4,         4,         4      ) )
	SMode        = default( SMode,        Select( pNum,  2,         2,        2,        2,        2,        2,        2,        2,        2,         2,         0      ) )
	SLModeX      = default( SLMode,       Select( pNum,  2,         2,        2,        2,        2,        2,        2,        2,        0,         0,         0      ) )
	SLRad        = default( SLRad,        Select( pNum,  3,         1,        1,        1,        1,        1,        1,        1,        1,         1,         1      ) )
	Sbb          = default( Sbb,          Select( pNum,  3,         1,        1,        0,        0,        0,        0,        0,        0,         0,         0      ) )
	SrchClipPP   = default( SrchClipPP,   Select( pNum,  3,         3,        3,        3,        3,        2,        2,        2,        1,         1,         0      ) )
	SubPel       = default( SubPel,       Select( pNum,  2,         2,        2,        2,        1,        1,        1,        1,        1,         1,         1      ) )
	Blocksize    = default( Blocksize,    Select( pNum,  bs,        bs,       bs,       bs,       bs,       bs,       bs2,      bs2,      bs2,       bs2,       bs2    ) )
	bs = Blocksize
	Overlap      = default( Overlap,      Select( pNum,  bs/2,      bs/2,     bs/2,     bs/2,     bs/2,     bs/2,     bs/2,     bs/4,     bs/4,      bs/4,      bs/4   ) )
	Search       = default( Search,       Select( pNum,  5,         4,        4,        4,        4,        4,        4,        4,        0,         0,         0      ) )
	SearchParam  = default( SearchParam,  Select( pNum,  2,         2,        2,        2,        2,        2,        2,        1,        1,         1,         1      ) )
	PelSearch    = default( PelSearch,    Select( pNum,  2,         2,        2,        2,        1,        1,        1,        1,        1,         1,         1      ) )
	ChromaMotion = default( ChromaMotion, Select( pNum,  true,      true,     true,     false,    false,    false,    false,    false,    false,     false,     false  ) )
	Precise      = default( Precise,      Select( pNum,  true,      true,     false,    false,    false,    false,    false,    false,    false,     false,     false  ) )
	ProgSADMask  = default( ProgSADMask,  Select( pNum,  10.0,      10.0,     10.0,     10.0,     10.0,     0.0,      0.0,      0.0,      0.0,       0.0,       0.0    ) )

	# Noise presets                                           Slower     Slow       Medium     Fast       Faster
	Denoiser       = default( Denoiser,       Select( npNum, "dfttest", "dfttest", "dfttest", "fft3df",  "fft3df" ) )
	DenoiseMC      = default( DenoiseMC,      Select( npNum,  true,      true,      false,     false,     false   ) )
	NoiseTR        = default( NoiseTR,        Select( npNum,  2,         1,         1,         1,         0       ) )
	NoiseDeint     = default( NoiseDeint,     Select( npNum, "Generate","Bob",      "",        "",        ""      ) )
	StabilizeNoise = default( StabilizeNoise, Select( npNum,  true,      true,      true,      false,     false   ) )

	# The basic source-match step corrects and re-runs the interpolation of the input clip. So it initialy uses same interpolation settings as the main preset
	SourceMatch   = default( SourceMatch, 0 )
	MatchNNSize   = NNSize
	MatchNNeurons = NNeurons
	MatchEdiMaxD  = EdiMaxD
	MatchEdiQual  = EdiQual

	# However, can use a faster initial interpolation when using source-match allowing the basic source-match step to "correct" it with higher quality settings
	Assert( SourceMatch == 0 || mpNum1 >= pNum, "'MatchPreset' cannot use a slower setting than 'Preset'" )
	#                                                                    Very                                                        Very      Super     Ultra
	# Basic source-match presets                                Placebo  Slow      Slower    Slow      Medium    Fast      Faster    Fast      Fast      Fast
	NNSize   = (SourceMatch == 0) ? NNSize   : Select( mpNum1,  1,       1,        1,        1,        5,        5,        4,        4,        4,        4     )
	NNeurons = (SourceMatch == 0) ? NNeurons : Select( mpNum1,  2,       2,        1,        1,        1,        0,        0,        0,        0,        0     )
	EdiMaxD  = (SourceMatch == 0) ? EdiMaxD  : Select( mpNum1,  12,      10,       8,        7,        7,        6,        6,        5,        4,        4     )
	EdiQual  = (SourceMatch == 0) ? EdiQual  : Select( mpNum1,  1,       1,        1,        1,        1,        1,        1,        1,        1,        1     )
	TempEdi  = EdiMode # Main interpolation is actually done by basic-source match step when enabled, so a little swap and wriggle is needed
	EdiMode  = (SourceMatch == 0) ? EdiMode  : default( MatchEdi, ((mpNum1 < 9) ?  EdiMode : "Yadif") ) # Force Yadif for "Ultra Fast" basic source match
	MatchEdi = TempEdi

	#                                                          Very                                                        Very      Super     Ultra
	# Refined source-match presets                   Placebo   Slow      Slower    Slow      Medium    Fast      Faster    Fast      Fast      Fast
	MatchEdi2 = default( MatchEdi2, Select( mpNum2, "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "NNEDI3", "TDeint",  ""    ) )
	MatchNNSize2                  = Select( mpNum2,  1,        1,        1,        1,        5,        5,        4,        4,        4,        4     )
	MatchNNeurons2                = Select( mpNum2,  2,        2,        1,        1,        1,        0,        0,        0,        0,        0     )
	MatchEdiMaxD2                 = Select( mpNum2,  12,       10,       8,        7,        7,        6,        6,        5,        4,        4     )
	MatchEdiQual2                 = Select( mpNum2,  1,        1,        1,        1,        1,        1,        1,        1,        1,        1     )


	#---------------------------------------
	# Settings

	# Core and Interpolation defaults
	TR2        = (SourceMatch > 0) ? default(TR2, ((TR2X == 0) ? 1 : TR2X)) : TR2X  # ***TR2 defaults always at least 1 when using source-match***
	RepChroma  = default( RepChroma,  true )
	EdiThreads = default( EdiThreads, 0    )
	ChromaEdi  = default( ChromaEdi,  ""   )
	NNeurons   = (EdiMode == "NNEDI2" && NNeurons > 2) ? 2 : NNeurons # Smaller range for NNeurons in NNEDI2 (which calls it nsize)
	EdiQual    = (EdiMode == "EEDI3+NNEDI3" || EdiMode == "NNEDI3") && EdiQual > 2 ? 2 : EdiQual  # Smaller range for EdiQual in NNEDI3

	# Source-match / lossless defaults
	MatchTR1     = TR1
	MatchTR2     = default( MatchTR2,     1   )
	MatchEnhance = default( MatchEnhance, 0.5 )
	Lossless     = default( Lossless,     0   )
	Assert( Lossless <= 2, "Lossless setting only supports mode 1 ('true lossless') and mode 2 ('fake lossless') - see documentation in script and consider source-match settings" )

	# Sharpness defaults. Sharpness default is always 1.0 (0.2 with source-match), but adjusted to give roughly same sharpness for all settings
	SMode      = (defined(Sharpness) && Sharpness == 0.0) ? 0 : SMode
	SLMode     = (SourceMatch > 0) ? default(SLMode, 0) : SLModeX  # ***Sharpness limiting disabled by default for source-match***
	SLMode     = (SLRad <= 0)      ? 0 : SLMode
	spatialSL  = (SLMode == 1 || SLMode == 3)
	temporalSL = (SLMode == 2 || SLMode == 4)
	Sharpness  = default( Sharpness, (SMode == 0) ? 0.0 : ((SourceMatch > 0) ? 0.2 : 1.0) )      # Default sharpness is 1.0, or 0.2 if using source-match
	sharpMul   = (temporalSL) ? 2 : (spatialSL) ? 1.5 : 1                                        # Adjust sharpness based on other settings
	sharpAdj   = Sharpness * (sharpMul * (0.2 + TR1*0.15 + TR2*0.25) + ((SMode == 1) ? 0.1 : 0)) # [This needs a bit more refinement]
	Sbb        = (SMode == 0) ? 0 : Sbb
	SOvs       = default( SOvs,   0   )
	SVThin     = default( SVThin, 0.0 )

	# Noise processing settings
	Assert( !defined(EZDenoise) || EZDenoise <= 0.0 || !defined(EZKeepGrain) || EZKeepGrain <= 0.0, "KTGMC: EZDenoise and EZKeepGrain cannot be used together" )
	NoiseProcess = defined(NoiseProcess) ? NoiseProcess : \
	               (defined(EZDenoise)   && EZDenoise   > 0.0)    ? 1 : \
	               (defined(EZKeepGrain) && EZKeepGrain > 0.0)    ? 2 : \
				   (Preset == "Placebo" || Preset == "Very Slow") ? 2 : 0
	GrainRestore = defined(GrainRestore) ? GrainRestore : \
	               (defined(EZDenoise)   && EZDenoise   > 0.0) ? 0.0 : \
	               (defined(EZKeepGrain) && EZKeepGrain > 0.0) ? 0.3 * sqrt(EZKeepGrain) : \
				                                                Select( NoiseProcess, 0.0, 0.7, 0.3 )
	NoiseRestore = defined(NoiseRestore) ? NoiseRestore : \
	               (defined(EZDenoise)   && EZDenoise   > 0.0) ? 0.0 : \
	               (defined(EZKeepGrain) && EZKeepGrain > 0.0) ? 0.1 * sqrt(EZKeepGrain) : \
				                                                Select( NoiseProcess, 0.0, 0.3, 0.1 )
	Sigma        = defined(Sigma)       ? Sigma : \
	               (defined(EZDenoise)   && EZDenoise   > 0.0) ? EZDenoise : \
	               (defined(EZKeepGrain) && EZKeepGrain > 0.0) ? 4.0 * EZKeepGrain : 2.0
	FftThreads   = default( FftThreads, EdiThreads )
	ChromaNoise  = default( ChromaNoise, false )
	ShowNoise    = default( ShowNoise, 0.0 )
	ShowNoise    = IsBool( ShowNoise ) ? (ShowNoise ? 10.0 : 0.0) : ShowNoise
	NoiseProcess = (ShowNoise > 0.0)   ? 2   : NoiseProcess
	NoiseRestore = (ShowNoise > 0.0)   ? 1.0 : NoiseRestore
	NoiseTR      = (NoiseProcess == 0) ? 0   : NoiseTR
	GrainRestore = (NoiseProcess == 0) ? 0.0 : GrainRestore
	NoiseRestore = (NoiseProcess == 0) ? 0.0 : NoiseRestore
	totalRestore = GrainRestore + NoiseRestore
	StabilizeNoise = (totalRestore <= 0) ? false : StabilizeNoise
	noiseTD  = Select( NoiseTR, 1, 3, 5 )
	noiseCentre = (Denoiser == "dfttest") ? "128" : "128.5"

	# MVTools settings
	SubPelInterp = default( SubPelInterp, 2     )
	TrueMotion   = default( TrueMotion,   false )
	GlobalMotion = default( GlobalMotion, true  )
	Lambda       = default( Lambda, ((TrueMotion) ? 1000 : 100 ) * (BlockSize*BlockSize)/(8*8) )
	LSAD         = default( LSAD,    (TrueMotion) ? 1200 : 400 )
	PNew         = default( PNew,    (TrueMotion) ? 50   : 25  )
	PLevel       = default( PLevel,  (TrueMotion) ? 1    : 0   )
	DCT          = default( DCT,     0          )
	ThSAD1       = default( ThSAD1,  10 * 8*8   )
	ThSAD2       = default( ThSAD2,   4 * 8*8   )
	ThSCD1       = default( ThSCD1,  180        )
	ThSCD2       = default( ThSCD2,  98         )

	# Motion blur settings
	FPSDivisor  = default( FPSDivisor, 1 )
	ShutterBlur = default( ShutterBlur, 0 )
	ShutterAngleSrc = default( ShutterAngleSrc, 180 )
	ShutterAngleOut = default( ShutterAngleOut, 180 )
	SBlurLimit  = default( SBlurLimit, 4 )
	ShutterBlur = (ShutterAngleOut * FPSDivisor == ShutterAngleSrc) ? 0 : ShutterBlur  # If motion blur output is same as input

	# Miscellaneous
	InputType      = default( InputType,     0        )
	Border         = default( Border,        false    )
	ShowSettings   = default( ShowSettings,  false    )
	GlobalNames    = default( GlobalNames,  "KTGMC"   )
	PrevGlobals    = default( PrevGlobals,  "Replace" )
	ForceTR        = default( ForceTR,       0        )
	ReplaceGlobals = (PrevGlobals == "Replace" || PrevGlobals == "Reuse") # If reusing existing globals put them back afterwards - simplifies logic later
	ReuseGlobals   = (PrevGlobals == "Reuse")
	ProgSADMask    = (InputType != 2 && InputType != 3) ? 0.0 : ProgSADMask
	rgBlur         = (Precise) ? 11 : 12
	dev = default(dev, 0)
	analyzeBatch   = default(analyzeBatch, 4)
	useFlag = default(useFlag, 0) # 0: no limitation, 1: use only previous fields, 1: use only next fields

	# Get maximum temporal radius needed
	maxTR = (temporalSL)       ? SLRad : 0
	maxTR = max(MatchTR2, TR1, TR2, NoiseTR, maxTR)
	maxTR = (ProgSADMask > 0.0 || StabilizeNoise || ShutterBlur > 0) ? (maxTR > 1 ? maxTR : 1) : maxTR
	maxTR = max(ForceTR, MaxTR)


	#---------------------------------------
	# Pre-Processing

	w = Input.Width()
	h = Input.Height()
	yuy2 = Input.IsYUY2()
	epsilon = 0.0001

	# Reverse "field" dominance for progressive repair mode 3 (only difference from mode 2)
	compl = (InputType == 3) ? Input.ComplementParity() : Input

	# Pad vertically during processing (to prevent artefacts at top & bottom edges)
	clip = (Border) ? compl.PointResize( w,h+8, 0,-4,0,h+8+epsilon ) : compl
	h = (Border) ? h+8 : h

	# Calculate padding needed for MVTools super clips to avoid crashes [fixed in latest MVTools, but keeping this code for a while]
	hpad = w - (Int((w - Overlap) / (Blocksize - Overlap)) * (Blocksize - Overlap) + Overlap)
	vpad = h - (Int((h - Overlap) / (Blocksize - Overlap)) * (Blocksize - Overlap) + Overlap)
	hpad = (hpad > 8) ? hpad : 8 # But match default padding if possible
	vpad = (vpad > 8) ? vpad : 8


	#---------------------------------------
	# Motion Analysis

	# >>> Planar YUY2 for motion analysis, interleaved whilst blurring search clip
	planarClip = yuy2 ? clip.nonyuy2clipin(true) : clip

	# Bob the input as a starting point for motion search clip
	bobbed = (InputType == 0) ? useEdiExt ? isyuy2(EdiExt) ? EdiExt.nonyuy2clipin(true) : EdiExt : planarClip.KTGMC_Bob( 0,0.5 ) : \
	         (InputType == 1) ? planarClip : \
	                            planarClip.Blur( 0,1 )

	# If required, get any existing global clips with a matching "GlobalNames" setting. Unmatched values get NOP (= 0)
	srchClip  = KTGMC_GetUserGlobal( GlobalNames, "srchClip",  ReuseGlobals )
	srchSuper = KTGMC_GetUserGlobal( GlobalNames, "srchSuper", ReuseGlobals )
	bVec1     = KTGMC_GetUserGlobal( GlobalNames, "bVec1",     ReuseGlobals )
	fVec1     = KTGMC_GetUserGlobal( GlobalNames, "fVec1",     ReuseGlobals )
	bVec2     = KTGMC_GetUserGlobal( GlobalNames, "bVec2",     ReuseGlobals )
	fVec2     = KTGMC_GetUserGlobal( GlobalNames, "fVec2",     ReuseGlobals )
	bVec3     = KTGMC_GetUserGlobal( GlobalNames, "bVec3",     ReuseGlobals )
	fVec3     = KTGMC_GetUserGlobal( GlobalNames, "fVec3",     ReuseGlobals )
	bVec4     = KTGMC_GetUserGlobal( GlobalNames, "bVec4",     ReuseGlobals )
	fVec4     = KTGMC_GetUserGlobal( GlobalNames, "fVec4",     ReuseGlobals )
	bVec5     = KTGMC_GetUserGlobal( GlobalNames, "bVec5",     ReuseGlobals )
	fVec5     = KTGMC_GetUserGlobal( GlobalNames, "fVec5",     ReuseGlobals )

	CMmt = ChromaMotion ? 3   :  1
	CMts = ChromaMotion ? 255 :  0
	CMrg = ChromaMotion ? 12  : -1

	# The bobbed clip will shimmer due to being derived from alternating fields. Temporally smooth over the neighboring frames using a binomial kernel. Binomial
	# kernels give equal weight to even and odd frames and hence average away the shimmer. The two kernels used are [1 2 1] and [1 4 6 4 1] for radius 1 and 2.
	# These kernels are approximately Gaussian kernels, which work well as a prefilter before motion analysis (hence the original name for this script)
	binomial0 = IsClip(srchClip) ? NOP() : \
	TR0 == -1 && defined(EdiExt) ? isyuy2(EdiExt) ? EdiExt.nonyuy2clipin(true) : EdiExt : \
	            (TR0 == 0)       ? bobbed : \
	            (TR0 == 1)       ? bobbed.KBinomialTemporalSoften(1, 28, ChromaMotion) : \
	                               bobbed.KBinomialTemporalSoften(2, 28, ChromaMotion)

	# Remove areas of difference between temporal blurred motion search clip and bob that are not due to bob-shimmer - removes general motion blur
	repair0 = (IsClip(srchClip) || Rep0 == 0) ? binomial0 : binomial0.KTGMC_KeepOnlyBobShimmerFixes( bobbed, Rep0, (RepChroma && ChromaMotion) )

	# Blur image and soften edges to assist in motion matching of edge blocks. Blocks are matched by SAD (sum of absolute differences between blocks), but even
	# a slight change in an edge from frame to frame will give a high SAD due to the higher contrast of edges
	spatialBlur = (IsClip(srchClip) || SrchClipPP == 0) ? NOP() : \
	              (SrchClipPP == 1) ? repair0.BilinearResize( w/2, h/2 ).RemoveGrain( 12,CMrg).BilinearResize( w, h ) : \
	                                  repair0.KRemoveGrain( 12,CMrg).KGaussResize( p=2, chroma=ChromaMotion )

	spatialBlur = (IsClip(spatialBlur) && SrchClipPP > 1) ? (ChromaMotion ? spatialBlur.KMerge( repair0, 0.1 ) : spatialBlur.KMergeLuma( repair0, 0.1 )) : spatialBlur
	srchcheck   = IsClip(srchClip)
	srchClip    = srchcheck         ? srchClip : \
	              (SrchClipPP == 0) ? repair0 : \
	              (SrchClipPP < 3)  ? spatialBlur : \
	                                  repair0.KTGMC_TweakSearchClip( bobbed, spatialBlur, U=CMmt,V=CMmt )
	srchClip    = tv_range && !srchcheck ? srchClip.KTGMC_ToFullRange( U=CMmt,V=CMmt ) : srchClip
	# Calculate forward and backward motion vectors from motion search clip
	srchSuper = IsClip(srchSuper) ? srchSuper : \
	            (maxTR > 0)       ? srchClip.KMSuper( pel=SubPel, sharp=SubPelInterp, hpad=hpad, vpad=vpad, chroma=ChromaMotion ) : NOP()
	# partial super for CPU calculation
	srchSuperP = (maxTR > 0)      ? srchSuper.KMPartialSuper().OnCUDA(1, dev) : NOP()
	bVec5 = IsClip(bVec5) ? bVec5 : \
	        (maxTR > 4)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, true, 5, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	bVec4 = IsClip(bVec4) ? bVec4 : \
	        (maxTR > 3)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, true,  4, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	bVec3 = IsClip(bVec3) ? bVec3 : \
	        (maxTR > 2)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, true,  3, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	bVec2 = IsClip(bVec2) ? bVec2 : \
	        (maxTR > 1)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, true,  2, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	bVec1 = IsClip(bVec1) ? bVec1 : \
	        (maxTR > 0)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, true,  1, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	fVec1 = IsClip(fVec1) ? fVec1 : \
	        (maxTR > 0)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, false, 1, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	fVec2 = IsClip(fVec2) ? fVec2 : \
	        (maxTR > 1)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, false, 2, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	fVec3 = IsClip(fVec3) ? fVec3 : \
	        (maxTR > 2)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, false, 3, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	fVec4 = IsClip(fVec4) ? fVec4 : \
	        (maxTR > 3)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, false, 4, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()
	fVec5 = IsClip(fVec5) ? fVec5 : \
	        (maxTR > 4)   ? srchSuper.KTGMC_MAnalyze( srchSuperP, false, 5, BlockSize, Overlap, Search, SearchParam, \
	                                            PelSearch, TrueMotion, Lambda, LSAD, PNew, PLevel, \
	                                            GlobalMotion, DCT, ChromaMotion, analyzeBatch ) : NOP()

	# Expose search clip, motion search super clip and motion vectors to calling script through globals
	KTGMC_SetUserGlobal( GlobalNames, "srchClip",  srchClip,  ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "srchSuper", srchSuper, ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "bVec1",     bVec1,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "fVec1",     fVec1,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "bVec2",     bVec2,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "fVec2",     fVec2,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "bVec3",     bVec3,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "fVec3",     fVec3,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "bVec4",     bVec4,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "fVec4",     fVec4,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "bVec5",     bVec5,     ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "fVec5",     fVec5,     ReplaceGlobals )

	# replace with the reference vectors for debug
	refvector = false
	bVec1 = refvector ? KMVReplaceWithMV(bVec1.OnCUDA(0, dev), QTGMC_bVec1).OnCPU(0) : bVec1
	fVec1 = refvector ? KMVReplaceWithMV(fVec1.OnCUDA(0, dev), QTGMC_fVec1).OnCPU(0) : fVec1
	#bVec2 = refvector ? KMVReplaceWithMV(bVec2.OnCUDA(0, dev), QTGMC_bVec2).OnCPU(0) : bVec2
	#fVec2 = refvector ? KMVReplaceWithMV(fVec2.OnCUDA(0, dev), QTGMC_fVec2).OnCPU(0) : fVec2
	#bVec3 = refvector ? KMVReplaceWithMV(bVec3.OnCUDA(0, dev), QTGMC_bVec3).OnCPU(0) : bVec3
	#fVec3 = refvector ? KMVReplaceWithMV(fVec3.OnCUDA(0, dev), QTGMC_fVec3).OnCPU(0) : fVec3

	#---------------------------------------
	# Noise Processing

	# >>>> Interleaved YUY2 for denoising, planar whilst pre-motion compensating

	# Expand fields to full frame size before extracting noise (allows use of motion vectors which are frame-sized)
	fullClip  = (NoiseProcess == 0) ? NOP() : \
	            (InputType > 0)     ? clip : \
	                                  clip.KTGMC_Bob( 0,1.0 )
	fullClip  = (yuy2 && NoiseTR > 0) ? fullClip.nonyuy2clipin(true) : fullClip
	fullSuper = (NoiseTR > 0)         ? fullClip.MSuper( pel=SubPel, levels=1, hpad=hpad, vpad=vpad, chroma=ChromaNoise ) : NOP() #TEST chroma OK?

	# Create a motion compensated temporal window around current frame and use to guide denoisers
	noiseWindow = (NoiseProcess == 0) ? NOP() : \
	              (!DenoiseMC)        ? fullClip : \
	              (NoiseTR == 0)      ? fullClip : \
	              (NoiseTR == 1)      ? Interleave( fullClip.MCompensate( fullSuper, fVec1, thSCD1=ThSCD1,thSCD2=ThSCD2 ), \
	                                                fullClip, \
	                                                fullClip.MCompensate( fullSuper, bVec1, thSCD1=ThSCD1,thSCD2=ThSCD2 ) ) : \
	                                    Interleave( fullClip.MCompensate( fullSuper, fVec2, thSCD1=ThSCD1,thSCD2=ThSCD2 ), \
	                                                fullClip.MCompensate( fullSuper, fVec1, thSCD1=ThSCD1,thSCD2=ThSCD2 ), \
	                                                fullClip, \
	                                                fullClip.MCompensate( fullSuper, bVec1, thSCD1=ThSCD1,thSCD2=ThSCD2 ), \
	                                                fullClip.MCompensate( fullSuper, bVec2, thSCD1=ThSCD1,thSCD2=ThSCD2 ) )
	noiseWindow = yuy2 && (Denoiser == "fft3df") && NoiseTR > 0 ? noiseWindow.nonyuy2clipout(true) : noiseWindow
	dnWindow = (NoiseProcess == 0)      ? NOP() : \
	           (Denoiser == "dfttest")  ? noiseWindow.dfttest( Y=true, U=ChromaNoise, V=ChromaNoise, sigma=Sigma*4, tbsize=noiseTD, threads=FftThreads, Dither=DftDither, lsb=lsbd ) : \
	        (Denoiser == "KNLMeansCL")  ? noiseWindow.SMDegrain_KNLMeansCL(Chroma=ChromaNoise, lsb=lsbd, a=2, d=NoiseTR, h=Sigma, device_type="GPU" ) : \
	                                      noiseWindow.FFT3DFilter( plane=(ChromaNoise ? 4 : 0), sigma=Sigma, bt=noiseTD, ncpu=FftThreads )
	dnwindow = (Denoiser == "KNLMeansCL" || Denoiser == "dfttest") && (NoiseProcess != 0) && lsbd ? dnWindow.ditherpost(mode=6, U=ChromaNoise?3:2, V=ChromaNoise?3:2, slice=false) : dnWindow
	dnwindow = (Denoiser == "KNLMeansCL" || Denoiser == "dfttest") && (NoiseProcess != 0) && yuy2 ? dnWindow.nonyuy2clipout(true) : dnWindow

	# Rework denoised clip to match source format - various code paths here: discard the motion compensation window, discard doubled lines (from point resize)
	# Also reweave to get interlaced noise if source was interlaced (could keep the full frame of noise, but it will be poor quality from the point resize)
	denoised = (NoiseProcess == 0) ? NOP() : \
	           (!DenoiseMC)        ? ((InputType > 0) ? dnWindow : dnWindow.SeparateFields().SelectEvery( 4, 0,3 ).Weave()) : \
	           (InputType > 0)     ? ((NoiseTR == 0)  ? dnWindow : dnWindow.SelectEvery( noiseTD, NoiseTR )) : \
	                                 dnWindow.SeparateFields().SelectEvery( noiseTD*4, NoiseTR*2,NoiseTR*6+3 ).Weave()

	# >>>> Switch to planar YUY2 for noise bypass

	CNmt1   = ChromaNoise ? 3 : 1
	CNmt2   = ChromaNoise ? 3 : 2
	CNmt128 = ChromaNoise ? 3 : -128

	# Get actual noise from difference. Then 'deinterlace' where we have weaved noise - create the missing lines of noise in various ways
	planarDenoised = (NoiseProcess == 0) ? NOP() : yuy2 ? denoised.nonyuy2clipin(true) : denoised
	noise = (totalRestore > 0.0)  ? mt_makediff( planarClip, planarDenoised, U=CNmt1,V=CNmt1 ) : NOP()
	deintNoise = (NoiseProcess == 0 || totalRestore == 0.0) ? NOP() : \
	             (InputType != 0)                           ? noise : \
	             (NoiseDeint == "Bob")                      ? noise.KTGMC_Bob( 0,0.5 ) : \
	             (NoiseDeint == "Generate")                 ? noise.KTGMC_Generate2ndFieldNoise( denoised, ChromaNoise ) : \
	                                                          noise.DoubleWeave()
	# Motion-compensated stabilization of generated noise
	noiseSuper = (StabilizeNoise) ? deintNoise.MSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad, chroma=ChromaNoise ) : NOP()
	mcNoise    = (StabilizeNoise) ? deintNoise.MCompensate( noiseSuper, bVec1, thSCD1=ThSCD1,thSCD2=ThSCD2 ) : NOP()
	finalNoise = (StabilizeNoise) ? mt_lutxy( deintNoise, mcNoise, "x range_half - abs y range_half - abs > x y ? 0.6 * x y + 0.2 * +", U=CNmt1,V=CNmt1 ) : deintNoise

	# If NoiseProcess=1 denoise input clip. If NoiseProcess=2 leave noise in the clip and let the temporal blurs "denoise" it for a stronger effect
	innerClip = (NoiseProcess == 1) ? denoised : clip


	#---------------------------------------
	# Interpolation

	# >>>> Interleaved YUY2 for interpolation

	# Support badly deinterlaced progressive content - drop half the fields and reweave to get 1/2fps interlaced stream appropriate for KTGMC processing
	ediInput = (InputType == 2 || InputType == 3) ? innerClip.SeparateFields().SelectEvery(4,0,3).Weave() : innerClip

	# Create interpolated image as starting point for output
	edi1 = defined(EdiExt) ? EdiExt.PointResize( w,h, 0,(EdiExt.Height()-h)/2, -0,h+epsilon ) : \
	                         KTGMC_Interpolate( ediInput, InputType, EdiMode, NNSize, NNeurons, EdiQual, EdiMaxD, EdiThreads, bobbed, ChromaEdi )

	# >>>> Switch to planar YUY2 during next step - remains planar until very end of script except blurring for back blending & SVThin

	edi  = KTGMC_GetUserGlobal( GlobalNames, "edi",  ReuseGlobals )
	ediSuper = KTGMC_GetUserGlobal( GlobalNames, "ediSuper", ReuseGlobals )

	# InputType=2,3: use motion mask to blend luma between original clip & reweaved clip based on ProgSADMask setting. Use chroma from original clip in any case
	inputTypeBlend = (ProgSADMask > 0.0) ? MMask( srchClip, bVec1, kind=1, ml=ProgSADMask ) : NOP()
	edi = IsClip(edi) ? edi : (InputType != 2 && InputType != 3) ? (!yuy2 ? edi1 : edi1.nonyuy2clipin(true)) :\
	      (ProgSADMask <= 0.0)               ? (!yuy2 ? edi1.MergeChroma( innerClip ) : edi1.MergeChroma( innerClip ).nonyuy2clipin(true)) : \
	                                           (!yuy2 ? mt_merge( innerClip, edi1, inputTypeBlend, U=2,V=2 ) : \
											            mt_merge( innerClip.nonyuy2clipin(true), edi1.nonyuy2clipin(true), inputTypeBlend, U=2,V=2 ))

	# Get the max/min value for each pixel over neighboring motion-compensated frames - used for temporal sharpness limiting
	ediSuper = IsClip(ediSuper) ? ediSuper : (TR1 > 0 || temporalSL)   ? edi.KMSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad ) : NOP()

	KTGMC_SetUserGlobal( GlobalNames, "edi",  edi,  ReplaceGlobals )
	KTGMC_SetUserGlobal( GlobalNames, "ediSuper", ediSuper, ReplaceGlobals )
	
	bComp1   = (temporalSL)              ? edi.KMCompensate( ediSuper, bVec1, thSCD1=ThSCD1,thSCD2=ThSCD2              ) : NOP()
	fComp1   = (temporalSL)              ? edi.KMCompensate( ediSuper, fVec1, thSCD1=ThSCD1,thSCD2=ThSCD2              ) : NOP()
	bComp3   = (SLRad > 1 && temporalSL) ? edi.KMCompensate( ediSuper, bVec3, thSCD1=ThSCD1,thSCD2=ThSCD2              ) : NOP()
	fComp3   = (SLRad > 1 && temporalSL) ? edi.KMCompensate( ediSuper, fVec3, thSCD1=ThSCD1,thSCD2=ThSCD2              ) : NOP()
	tMax     = (SLRad > 1 && temporalSL) ? tMax.mt_logic( fComp3, "max", U=3,V=3 ).mt_logic( bComp3, "max", U=3,V=3 )   : NOP()
	tMin     = (SLRad > 1 && temporalSL) ? tMin.mt_logic( fComp3, "min", U=3,V=3 ).mt_logic( bComp3, "min", U=3,V=3 )   : NOP()
	
	#---------------------------------------
	# Create basic output

	# Use motion vectors to blur interpolated image (edi) with motion-compensated previous and next frames. As above, this is done to remove shimmer from
	# alternate frames so the same binomial kernels are used. However, by using motion-compensated smoothing this time we avoid motion blur. The use of
	# MDegrain1 (motion compensated) rather than TemporalSmooth makes the weightings *look* different, but they evaluate to the same values
	binomial1 = (TR1 == 0) ? edi : \
	            (TR1 == 1) ? edi.KMDegrain1( ediSuper, bVec1,fVec1,              thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag) : \
	                         edi.KMDegrain2( ediSuper, bVec1,fVec1, bVec2,fVec2, thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag)

	# Remove areas of difference between smoothed image and interpolated image that are not bob-shimmer fixes: repairs residual motion blur from temporal smooth
	repair1 = (Rep1 == 0) ? binomial1 : binomial1.KTGMC_KeepOnlyBobShimmerFixes( edi, Rep1, RepChroma )

	# Apply source match - use difference between output and source to succesively refine output [extracted to function to clarify main code path]
	match = (SourceMatch == 0) ? repair1 : \
	                             repair1.KTGMC_ApplySourceMatch( InputType, ediInput, bVec1,fVec1, bVec2,fVec2, SubPel, SubPelInterp, hpad, vpad, \
	                                                             ThSAD1, ThSCD1, ThSCD2, SourceMatch, MatchTR1, MatchEdi, MatchNNSize, MatchNNeurons, \
	                                                             MatchEdiQual, MatchEdiMaxD, MatchTR2, MatchEdi2, MatchNNSize2, MatchNNeurons2, MatchEdiQual2, \
	                                                             MatchEdiMaxD2, MatchEnhance, EdiThreads, lsb, useFlag )

	# Lossless=2 - after preparing an interpolated, de-shimmered clip, restore the original source fields into it and clean up any artefacts.
	# This mode will not give a true lossless result because the resharpening and final temporal smooth are still to come, but it will add further detail.
	# However, it can introduce minor combing. This setting is best used together with source-match (it's effectively the final source-match stage).
	lossed1 = (Lossless == 2) ? KTGMC_MakeLossless( match, innerClip, InputType ) : match

	#---------------------------------------
	# Resharpen / retouch output

	# Resharpen to counteract temporal blurs. Little sharpening needed for source-match mode since it has already recovered sharpness from source
	vresharp1 = (SMode == 2) ? KTGMC_VResharpen( lossed1 ,U=3,V=3 ) : NOP()
	vresharp  = (Precise && SMode == 2) ? vresharp1.mt_lutxy( lossed1, "x y < x 1 scalef + x y > x 1 scalef - x ? ?", U=3,V=3 ) : vresharp1 # Precise mode: reduce tiny overshoot
	resharp   = (SMode == 0) ? lossed1 : \
	            (SMode == 1) ? lossed1.KTGMC_Resharpen( lossed1.KRemoveGrain( rgBlur ),  sharpAdj, U=3,V=3 ) : \
	                           lossed1.KTGMC_Resharpen( vresharp.KRemoveGrain( rgBlur ), sharpAdj, U=3,V=3 )

	# Slightly thin down 1-pixel high horizontal edges that have been widened into neigboring field lines by the interpolator
	SVThinSc = SVThin * 6.0
	vertMedD = (SVthin > 0.0) ? mt_lutxy( lossed1, lossed1.VerticalCleaner( mode=1, modeU=-1, modeV=-1 ), "y x - " + string(SVThinSc) + " * range_half +", U=1,V=1 ) : NOP()
	vertMedD = (SVthin > 0.0) ? (vertMedD.Blur( 1,0 )) : NOP()
	neighborD = (SVthin > 0.0) ? mt_lutxy( vertMedD, vertMedD.RemoveGrain( rgBlur,-1 ), "y range_half - abs x range_half - abs > y range_half ?" ) : NOP()
	thin      = (SVthin > 0.0) ? resharp.mt_adddiff( neighborD, U=2,V=2 ) : resharp

	# Back blend the blurred difference between sharpened & unsharpened clip, before (1st) sharpness limiting (Sbb == 1,3). A small fidelity improvement
	backBlend1 = (Sbb != 1 && Sbb != 3) ? thin : \
		                 thin.KMakeDiff( KMakeDiff( thin, lossed1, U=1,V=1 ).KRemoveGrain( 12, -1 ) \
	                         .KGaussResize( p=5, chroma=true ), U=2,V=2 )

	# Limit over-sharpening by clamping to neighboring (spatial or temporal) min/max values in original
	# Occurs here (before final temporal smooth) if SLMode == 1,2. This location will restrict sharpness more, but any artefacts introduced will be smoothed
	sharpLimit1 = (SLMode == 1) ? backBlend1.Repair( ((SLrad <= 1) ? edi : backBlend1.Repair( edi, 12 )), 1 ) : \
	              (SLMode == 2) ? backBlend1.KTGMC_LimitOverSharpen( edi, fComp1,bComp1, Sovs, U=3,V=3 ) : \
	                              backBlend1

	# Back blend the blurred difference between sharpened & unsharpened clip, after (1st) sharpness limiting (Sbb == 2,3). A small fidelity improvement
	backBlend2 = (Sbb < 2) ? sharpLimit1 : \
		                 sharpLimit1.mt_makediff( mt_makediff( sharpLimit1, lossed1, U=1,V=1 ).RemoveGrain( 12, -1 ) \
	                                    .GaussResize( w,h, 0,0, w+epsilon,h+epsilon, p=5 ), U=2,V=2 )

	# Add back any extracted noise, prior to final temporal smooth - this will restore detail that was removed as "noise" without restoring the noise itself
	# Average luma of FFT3DFilter extracted noise is 128.5, so deal with that too
	addNoise1 = (GrainRestore <= 0.0) ? backBlend2 : \
		                                backBlend2.mt_adddiff( finalNoise.mt_lut( "x " + noiseCentre + " scalef - " + string(GrainRestore) + " * range_half +", U=CNmt1,V=CNmt1 ), U=CNmt2,V=CNmt2 )

	# Final light linear temporal smooth for denoising
	stableSuper = (TR2 > 0) ? addNoise1.KMSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad ) : NOP()
	stable  = (TR2 == 0) ? addNoise1 : \
	          (TR2 == 1) ? addNoise1.KMDegrain1( stableSuper, bVec1,fVec1,                                                     thSAD=ThSAD2, thSCD1=ThSCD1,thSCD2=ThSCD2, useFlag=useFlag ) : \
	          (TR2 == 2) ? addNoise1.KMDegrain2( stableSuper, bVec1,fVec1, bVec2,fVec2,                                        thSAD=ThSAD2, thSCD1=ThSCD1,thSCD2=ThSCD2, useFlag=useFlag ) : \
 	          (TR2 == 3) ? addNoise1.MDegrain3( stableSuper, bVec1,fVec1, bVec2,fVec2, bVec3,fVec3,                           thSAD=ThSAD2, thSCD1=ThSCD1,thSCD2=ThSCD2, lsb=lsb ) : \
 	          (TR2 == 4) ? addNoise1.MDegrain4( stableSuper, bVec1,fVec1, bVec2,fVec2, bVec3,fVec3, bVec4,fVec4,              thSAD=ThSAD2, thSCD1=ThSCD1,thSCD2=ThSCD2, lsb=lsb ) : \
 	                       addNoise1.MDegrain5( stableSuper, bVec1,fVec1, bVec2,fVec2, bVec3,fVec3, bVec4,fVec4, bVec5,fVec5, thSAD=ThSAD2, thSCD1=ThSCD1,thSCD2=ThSCD2, lsb=lsb )
	stable  = (TR2 > 0) && lsb ? stable.ditherpost(mode=6, slice=false) : stable

	# Remove areas of difference between final output & basic interpolated image that are not bob-shimmer fixes: repairs motion blur caused by temporal smooth
	repair2 = (Rep2 == 0) ? stable : stable.KTGMC_KeepOnlyBobShimmerFixes( edi, Rep2, RepChroma )

	# Limit over-sharpening by clamping to neighboring (spatial or temporal) min/max values in original
	# Occurs here (after final temporal smooth) if SLMode == 3,4. Allows more sharpening here, but more prone to introducing minor artefacts
	sharpLimit2 = (SLMode == 3) ? repair2.Repair( ((SLrad <= 1) ? edi : repair2.Repair( edi, 12 )), 1 ) : \
	              (SLMode == 4) ? repair2.mt_clamp( tMax,tMin, Sovs,Sovs, U=3,V=3 ) : \
	                              repair2

	# Lossless=1 - inject source fields into result and clean up inevitable artefacts. Provided NoiseRestore=0.0 or 1.0, this mode will make the script result 
	# properly lossless, but this will retain source artefacts and cause some combing (where the smoothed deinterlace doesn't quite match the source)
	lossed2 = (Lossless == 1) ? KTGMC_MakeLossless( sharpLimit2, innerClip, InputType ) : sharpLimit2

	# Add back any extracted noise, after final temporal smooth. This will appear as noise/grain in the output
	# Average luma of FFT3DFilter extracted noise is 128.5, so deal with that too
	addNoise2 = (NoiseRestore <= 0.0) ? lossed2 : \
		                                lossed2.mt_adddiff( finalNoise.mt_lut( "clamp_f x " + noiseCentre + " scalef - " + string(NoiseRestore) + " * range_half +", U=CNmt1,V=CNmt1 ), U=CNmt2,V=CNmt2 )


	#---------------------------------------
	# Post-Processing

	# Shutter motion blur - get level of blur depending on output framerate and blur already in source
	blurLevel = (ShutterAngleOut * FPSDivisor - ShutterAngleSrc) * 100.0 / 360.0
	Assert( blurLevel >= 0, "Cannot reduce motion blur already in source: increase ShutterAngleOut or FPSDivisor" )
	Assert( blurLevel <= 200, "Exceeded maximum motion blur level: decrease ShutterAngleOut or FPSDivisor" )

	# ShutterBlur mode 2,3 - get finer resolution motion vectors to reduce blur "bleeding" into static areas
	rBlockDivide = Select( ShutterBlur, 1, 1, 2, 4 )
	rBlockSize = BlockSize / rBlockDivide
	rOverlap   = Overlap   / rBlockDivide
	rBlockSize = (rBlockSize < 4) ? 4 : rBlockSize
	rOverlap   = (rOverlap   < 2) ? 2 : rOverlap
	rBlockDivide = BlockSize / rBlockSize
	rLambda = Lambda / (rBlockDivide * rBlockDivide)
	sbBVec1 = (ShutterBlur > 1) ? srchSuper.MRecalculate( bVec1, thSAD=ThSAD1, blksize=rBlockSize, overlap=rOverlap, search=Search, searchparam=SearchParam, \
	                                                      truemotion=TrueMotion, lambda=Lambda, pnew=PNew, DCT=DCT, chroma=ChromaMotion ) : bVec1
	sbFVec1 = (ShutterBlur > 1) ? srchSuper.MRecalculate( fVec1, thSAD=ThSAD1, blksize=rBlockSize, overlap=rOverlap, search=Search, searchparam=SearchParam, \
	                                                      truemotion=TrueMotion, lambda=Lambda, pnew=PNew, DCT=DCT, chroma=ChromaMotion ) : fVec1

	# Shutter motion blur - use MFlowBlur to blur along motion vectors
	sblurSuper = (ShutterBlur > 0) ? addNoise2.MSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad ) : NOP()
	sblur =      (ShutterBlur > 0) ? addNoise2.MFlowBlur( sblurSuper, sbBVec1, sbFVec1, blur=blurLevel, thSCD1=ThSCD1,thSCD2=ThSCD2 ) : NOP()

	# Shutter motion blur - use motion mask to reduce blurring in areas of low motion - also helps reduce blur "bleeding" into static areas, then select blur type
	sbMotionMask = (ShutterBlur > 0 && SBlurLimit > 0) ? MMask( srchClip, bVec1, kind=0, ml=SBlurLimit ) : NOP()
	sblurred     = (ShutterBlur == 0) ? addNoise2 : \
	               (SBlurLimit == 0)  ? sblur : \
	                                    mt_merge( addNoise2, sblur, sbMotionMask, U=3,V=3 )
	# Reduce frame rate
	decimated = (FPSDivisor != 1) ? sblurred.SelectEvery( FPSDivisor, 0 ) : sblurred

	# Crop off temporary vertical padding
	cropped = Border ? decimated.Crop( 0, 4, -0, -4 ) : decimated
	h = Border ? h-8 : h

	# Show output of choice + settings
	# >>>> Restore YUY2 to interleaved
	output = (ShowNoise == 0.0) ? cropped : finalNoise.mt_lut( "clamp_f x range_half - " + string(ShowNoise) + " * range_half +", U=CNmt128,V=CNmt128 )
	output = yuy2 ? output.nonyuy2clipout(true) : output
	return (ShowSettings == false) ? output : \
		output.Subtitle( "TR0=" + string(TR0) + " | TR1=" + string(TR1) + " | TR2=" + string(TR2) + " | Rep0=" + string(Rep0) + " | Rep1=" + string(Rep1) + \
		" | Rep2=" + string(Rep2) + " | RepChroma=" + string(RepChroma) + "\nEdiMode='" + EdiMode + "' | NNSize=" + string(NNSize) + " | NNeurons=" + \
		string(NNeurons) + " | EdiQual=" + string(EdiQual) + " | EdiMaxD=" + string(EdiMaxD) + " | ChromaEdi='" + ChromaEdi + "' | EdiThreads=" + \
		string(EdiThreads) + "\nSharpness=" + string(Sharpness, "%.2f") + " | SMode=" + string(SMode) + " | SLMode=" + string(SLMode) + " | SLRad=" + \
		string(SLRad) + " | SOvs=" + string(SOvs) + " | SVThin=" + string(SVThin, "%.2f") + " | Sbb=" + string(Sbb) + "\nSrchClipPP=" + string(SrchClipPP) + \
		" | SubPel=" + string(SubPel) + " | SubPelInterp=" + string(SubPelInterp) + " | BlockSize=" + string(BlockSize) + " | Overlap=" + string(Overlap) + \
		"\nSearch=" + string(Search) + " | SearchParam=" + string(SearchParam) + " | PelSearch=" + string(PelSearch) + " | ChromaMotion=" + \
		string(ChromaMotion) + " | TrueMotion=" + string(TrueMotion) + "\nLambda=" + string(Lambda) + " | LSAD=" + string(LSAD) + " | PNew=" + string(PNew) + \
		" | PLevel=" + string(PLevel) + " | GlobalMotion=" + string(GlobalMotion) + " | DCT=" + string(DCT) + "\nThSAD1=" + string(ThSAD1) + " | ThSAD2=" + \
		string(ThSAD2) + " | ThSCD1=" + string(ThSCD1) + " | ThSCD2=" + string(ThSCD2) + "\nSourceMatch=" + string(SourceMatch) + " | MatchPreset='" + \
		MatchPreset + "' | MatchEdi='" + MatchEdi + "'\nMatchPreset2='" + MatchPreset2 + "' | MatchEdi2='" + MatchEdi2 + "' | MatchTR2=" + string(MatchTR2) + \
		" | MatchEnhance=" + string(MatchEnhance, "%.2f") + " | Lossless=" + string(Lossless) + "\nNoiseProcess=" + string(NoiseProcess) + " | Denoiser='" + \
		Denoiser + "' | FftThreads=" + string(FftThreads) + " | DenoiseMC=" + string(DenoiseMC) + " | NoiseTR=" + string(NoiseTR) + " | Sigma=" + \
		string(Sigma, "%.2f") + "\nChromaNoise=" + string(ChromaNoise) + " | ShowNoise=" + string(ShowNoise, "%.2f") + " | GrainRestore=" + \
		string(GrainRestore, "%.2f") + " | NoiseRestore=" + string(NoiseRestore, "%.2f") + "\nNoiseDeint='" + NoiseDeint + "' | StabilizeNoise=" + \
		string(StabilizeNoise) + " | InputType=" + string(InputType) + " | ProgSADMask=" + string(ProgSADMask, "%.2f") + "\nFPSDivisor=" + \
		string(FPSDivisor) + " | ShutterBlur=" + string(ShutterBlur) + " | ShutterAngleSrc=" + string(ShutterAngleSrc, "%.2f") + " | ShutterAngleOut=" + \
		string(ShutterAngleOut, "%.2f") + " | SBlurLimit=" + string(SBlurLimit) + "\nBorder=" + string(Border) + " | Precise=" + string(Precise) + \
		"\nPreset='" + Preset + "' | Tuning='" + Tuning + "' | GlobalNames='" + GlobalNames + "' | PrevGlobals='" + PrevGlobals + "' | ForceTR=" + \
		string(ForceTR), font="Lucida Console", size=11, lsp=12 )
}


#---------------------------------------
# Helpers

function KTGMC_MAnalyze( clip srchSuper, clip srchSuperP, bool isb, int delta, int blksize, int overlap, \
					int search, int searchparam, int pelsearch, bool truemotion, int lambda, int lsad, int pnew, \
					int plevel, bool global, int dct, bool chroma, int batch)
{
	# partial mv on CPU
	pmv = srchSuperP.KMAnalyse(blksize=blksize, overlap=overlap, search=search, isb=isb, chroma=chroma, \
					delta=delta, lambda=lambda, lsad=lsad, global=global, plevel=plevel, pnew=pnew, \
					meander=false, batch=1).OnCPU(1)

	# main mv on CUDA
	return srchSuper.KMAnalyse(blksize=blksize, overlap=overlap, search=search, isb=isb, chroma=chroma, \
					delta=delta, lambda=lambda, lsad=lsad, global=global, plevel=plevel, pnew=pnew, \
					meander=false, partial=pmv, batch=batch)
}

# Interpolate input clip using method given in EdiMode. Use Fallback or Bob as result if mode not in list. If ChromaEdi string if set then interpolate chroma
# separately with that method (only really useful for EEDIx). The function is used as main algorithm starting point and for first two source-match stages
function KTGMC_Interpolate( clip Input, int InputType, string EdiMode, int NNSize, int NNeurons, int EdiQual, int EdiMaxD, int EdiThreads, clip "Fallback", \
                            string "ChromaEdi" )
{
	# >>>> YUY2 is interleaved here

	ChromaEdi = default( ChromaEdi, "" )
	CEed = (ChromaEdi == "")

	interp = (InputType == 1)            ? Input : \
	         (EdiMode == "NNEDI3")       ? Input.KNNEDI3( field=-2, nsize=NNSize, nns=NNeurons, qual=EdiQual, threads=EdiThreads, U=CEed,V=CEed ) : \
	         (EdiMode == "NNEDI2")       ? Input.NNEDI2( field=-2, nsize=NNeurons, qual=EdiQual, threads=EdiThreads, U=CEed,V=CEed ) : \
	         (EdiMode == "NNEDI")        ? Input.NNEDI( field=-2, U=CEed,V=CEed ) : \
	         (EdiMode == "EEDI3+NNEDI3") ? Input.EEDI3( field=-2, mdis=EdiMaxD, threads=EdiThreads, U=CEed,V=CEed, \
		                                                sclip=Input.NNEDI3( field=-2, nsize=NNSize, nns=NNeurons, qual=EdiQual, threads=EdiThreads, U=CEed,V=CEed ) ) : \
	         (EdiMode == "EEDI3")        ? Input.EEDI3( field=-2, mdis=EdiMaxD, threads=EdiThreads, U=CEed,V=CEed ) : \
	         (EdiMode == "EEDI2")        ? Input.SeparateFields().EEDI2( field=-2, maxd=EdiMaxD ) : \
	         (EdiMode == "Yadif")        ? isyuy2(Input) ? Input.nonyuy2clipin(true).Yadifmod2( mode=3 ).nonyuy2clipout(true) : Input.Yadifmod2( mode=3 ) : \
	         (EdiMode == "cYadif")       ? Input.Yadif( mode=3 ) : \
	         (EdiMode == "TDeint")       ? Input.TDeInt( mode=1 ) : \
	         (EdiMode == "RepYadif")     ? isyuy2(Input) ? Repair( Input.nonyuy2clipin(true).Yadifmod2( mode=3 ), default( Fallback, Input.KTGMC_Bob( 0,0.5 ) ).nonyuy2clipin(true), 2, 0 ).nonyuy2clipout(true) : \
                                                           Input.Yadifmod2( mode=3 ) : \	
	         (EdiMode == "RepcYadif")    ? isyuy2(Input) ? Repair( Input.Yadif( mode=3 ).nonyuy2clipin(true), default( Fallback, Input.KTGMC_Bob( 0,0.5 ) ).nonyuy2clipin(true), 2, 0 ).nonyuy2clipout(true) : \
	         	         	                       Repair( Input.Yadif( mode=3 ), default( Fallback, Input.KTGMC_Bob( 0,0.5 ) ), 2, 0 ) : \
	                                       default( Fallback, Input.KTGMC_Bob( 0,0.5 ) )

	interpuv = (InputType == 1)        ? NOP() : \
	           (ChromaEdi == "NNEDI3") ? Input.KNNEDI3( field=-2, nsize=4, nns=0, qual=1, threads=EdiThreads, Y=false ) : \
	           (ChromaEdi == "Yadif")  ? isyuy2(Input) ? Input.nonyuy2clipin(true).Yadifmod2( mode=3 ).nonyuy2clipout(true) : Input.Yadifmod2( mode=3 ) : \
	           (ChromaEdi == "cYadif") ? Input.Yadif( mode=3 ) : \
	           (ChromaEdi == "Bob")    ? Input.KTGMC_Bob( 0,0.5 ) : \
			                             NOP()

	return (!IsClip(interpuv)) ? interp : interp.KMergeChroma( interpuv )
}


# Functions (from original TGMC) used instead of mt_xxflate with similar operation but a somewhat stronger result. Originally added for speed, they are
# no longer faster due to improvements in masktools. Difference (visual and speed) is small so may be reverted in a later version.
function KTGMC_inflate( clip c, int "Y", int "U", int "V" )
{
	# >>>> YUY2 is planar here
	mtY =default( Y, 3 )
	mtU =default( U, 1 )
	mtV =default( V, 1 )
	rgY = (mtY == 3) ? 20 : -1
	rgU = (mtU == 3) ? 20 : -1
	rgV = (mtV == 3) ? 20 : -1
	KLogic( c, c.KRemoveGrain( rgY, rgU, rgV ), "max", Y=mtY,U=mtU,V=mtV )
}

function KTGMC_deflate( clip c, int "Y", int "U", int "V" )
{
	# >>>> YUY2 is planar here
	mtY =default( Y, 3 )
	mtU =default( U, 1 )
	mtV =default( V, 1 )
	rgY = (mtY == 3) ? 20 : -1
	rgU = (mtU == 3) ? 20 : -1
	rgV = (mtV == 3) ? 20 : -1
	KLogic( c, c.KRemoveGrain( rgY, rgU, rgV ), "min", Y=mtY,U=mtU,V=mtV )
}

# Helper function: Compare processed clip with reference clip: only allow thin, horizontal areas of difference, i.e. bob shimmer fixes
# Rough algorithm: Get difference, deflate vertically by a couple of pixels or so, then inflate again. Thin regions will be removed
#                  by this process. Restore remaining areas of difference back to as they were in reference clip.
function KTGMC_KeepOnlyBobShimmerFixes( clip Input, clip Ref, int Rep, bool Chroma )
{
	# >>>> YUY2 is planar here

	# ed is the erosion distance - how much to deflate then reflate to remove thin areas of interest: 0 = minimum to 6 = maximum
	# od is over-dilation level  - extra inflation to ensure areas to restore back are fully caught:  0 = none to 3 = one full pixel
	# If Rep < 10, then ed = Rep and od = 0, otherwise ed = 10s digit and od = 1s digit (nasty method, but kept for compatibility with original TGMC)
	Rep    = default( Rep,    1    )
	Chroma = default( Chroma, true )
	ed = (Rep < 10) ? Rep : Rep / 10
	od = (Rep < 10) ? 0   : Rep % 10
	RCrg  = Chroma ? 3 : 1
	RCrgo = Chroma ? 3 : 2

	diff = KMakeDiff(Ref, Input, U=3,V=3 )

	# Areas of positive difference                                                                # ed = 0 1 2 3 4 5 6 7
	choke1 =                        diff.  KInpandVerticalX2( U=RCrg,V=RCrg )                     #      x x x x x x x x    1 pixel   \
	choke1 = (ed > 5)             ? choke1.mt_inpand( mode="vertical", U=RCrg,V=RCrg ) : choke1   #      . . . . . . x x    1 pixel   /
	choke1 = (ed % 3 != 0)        ? choke1.KTGMC_deflate( U=RCrg,V=RCrg )              : choke1   #      . x x . x x . x    A bit more deflate & some horizonal effect
	choke1 = (ed == 2 || ed == 5) ? choke1.RemoveGrain( 4              )               : choke1   #      . . x . . x . .    Local median
                                                                                                                      
	choke1 =                        choke1.KExpandVerticalX2( U=RCrg,V=RCrg )                     #      x x x x x x x x    1 pixel  \
	choke1 = (ed > 4)             ? choke1.mt_expand( mode="vertical", U=RCrg,V=RCrg ) : choke1   #      . . . . . x x x    1 pixel  /

	# Over-dilation - extra reflation up to about 1 pixel
	choke1 = (od == 0)            ? choke1 : \
	         (od == 1)            ? choke1.KTGMC_inflate( U=RCrg,V=RCrg ) : \
	         (od == 2)            ? choke1.KTGMC_inflate( U=RCrg,V=RCrg ).KTGMC_inflate( U=RCrg,V=RCrg ) : \
	                                choke1.mt_expand ( U=RCrg,V=RCrg )

	# Areas of negative difference (similar to above)
	choke2 =                        diff.  KExpandVerticalX2( U=RCrg,V=RCrg )         
	choke2 = (ed > 5)             ? choke2.mt_expand( mode="vertical", U=RCrg,V=RCrg ) : choke2
	choke2 = (ed % 3 != 0)        ? choke2.KTGMC_inflate( U=RCrg,V=RCrg )              : choke2
	choke2 = (ed == 2 || ed == 5) ? choke2.RemoveGrain( 4              )               : choke2
	choke2 =                        choke2.KInpandVerticalX2( U=RCrg,V=RCrg )
	choke2 = (ed > 4)             ? choke2.mt_inpand( mode="vertical", U=RCrg,V=RCrg ) : choke2
	choke2 = (od == 0)            ? choke2 : \
	         (od == 1)            ? choke2.KTGMC_deflate( U=RCrg,V=RCrg )  : \
	         (od == 2)            ? choke2.KTGMC_deflate( U=RCrg,V=RCrg ).KTGMC_deflate( U=RCrg,V=RCrg ) : \
	                                choke2.mt_inpand ( U=RCrg,V=RCrg )

	# Combine above areas to find those areas of difference to restore
	return Input.KTGMC_BobShimmerFixesMerge(diff, choke1, choke2, U=RCrgo,V=RCrgo )
}


# Given noise extracted from an interlaced source (i.e. the noise is interlaced), generate "progressive" noise with a new "field" of noise injected. The new
# noise is centered on a weighted local average and uses the difference between local min & max as an estimate of local variance
# YUY2 clip input is planar, but must pass interleaved version of clip to setup noise
function KTGMC_Generate2ndFieldNoise( clip Input, clip InterleavedClip, bool "ChromaNoise" )
{
	# >>>> YUY2 is planar here. Noise is generated (AddGrainC) interleaved, but immediately made planar
	ChromaNoise = default( ChromaNoise, false )
	CNmt1 = ChromaNoise ? 3 : 1
	origNoise = Input.SeparateFields()
	noiseMax  = origNoise.mt_expand( mode="square", U=CNmt1,V=CNmt1 ).mt_expand( mode="horizontal", U=CNmt1,V=CNmt1 )
	noiseMin  = origNoise.mt_inpand( mode="square", U=CNmt1,V=CNmt1 ).mt_inpand( mode="horizontal", U=CNmt1,V=CNmt1 )
	random    = BlankClip( InterleavedClip.SeparateFields(), color_yuv=$808080 ).AddGrainC( var=1800, uvar=ChromaNoise ? 1800 : 0 )
	random    = InterleavedClip.IsYUY2() ? random.nonyuy2clipin(true) : random
	varRandom = mt_makediff( noiseMax, noiseMin, U=CNmt1,V=CNmt1 ).mt_lutxy( random, "clamp_f_i8 x range_half - y * range_size / range_half +", U=CNmt1,V=CNmt1)
	newNoise  = noiseMin.mt_adddiff( varRandom, U=CNmt1,V=CNmt1 )
	return Interleave( origNoise, newNoise ).Weave()
}


# Insert the source lines into the result to create a true lossless output. However, the other lines in the result have had considerable processing and won't
# exactly match source lines. There will be some slight residual combing. Use vertical medians to clean a little of this away
function KTGMC_MakeLossless( clip Input, clip Source, int InputType )
{
	Assert( InputType != 1, "Lossless modes are incompatible with InputType=1" )

	# >>>> YUY2: 'Input' is planar, 'Source' is interleaved (changed to planar here for processing) - returns planar result

	# Weave the source fields and the "new" fields that have generated in the input
	srcFields1 = (InputType == 0) ? Source.SeparateFields() : Source.SeparateFields().SelectEvery( 4, 0,3 )
	srcFields  = Source.IsYUY2() ? srcFields1.nonyuy2clipin(true) : srcFields1
	newFields  = Input.SeparateFields().SelectEvery( 4, 1,2 )
	processed  = Interleave( srcFields, newFields ).SelectEvery(4, 0,1,3,2 ).KWeave()

	# Clean some of the artefacts caused by the above - creating a second version of the "new" fields
	vertMedian  = processed.KVerticalCleaner( mode=1 )
	vertMedDiff = KMakeDiff( processed, vertMedian, U=3,V=3 )
	vmNewDiff1  = vertMedDiff.SeparateFields().SelectEvery( 4, 1,2 ).KCopy()
	vmNewDiff2  = vmNewDiff1.KVerticalCleaner( mode=1 ).KTGMC_LosslessProc( vmNewDiff1, U=3,V=3 )
	vmNewDiff3  = vmNewDiff2.KRepair( vmNewDiff2.KRemoveGrain( 2 ), 1 )

	# Reweave final result
	return Interleave( srcFields, newFields.KCopy().KMakeDiff( vmNewDiff3, U=3,V=3 )).SelectEvery( 4, 0,1,3,2 ).KWeave()
}


# Source-match, a three stage process that takes the difference between deinterlaced input and the original interlaced source, to shift the input more towards
# the source without introducing shimmer. All other arguments defined in main script
function KTGMC_ApplySourceMatch( clip Deinterlace, int InputType, val Source, val bVec1, val fVec1, val bVec2, val fVec2, \
                                 int SubPel, int SubPelInterp, int hpad, int vpad, int ThSAD1, int ThSCD1, int ThSCD2, int SourceMatch, \
                                 int MatchTR1, string MatchEdi, int MatchNNSize, int MatchNNeurons, int MatchEdiQual, int MatchEdiMaxD,\
                                 int MatchTR2, string MatchEdi2, int MatchNNSize2, int MatchNNeurons2, int MatchEdiQual2, int MatchEdiMaxD2, \
                                 float MatchEnhance, int EdiThreads, bool lsb, int useFlag )
{
	# >>>> YUY2: 'Deinterlace' is planar, 'Source' is interleaved (changed to planar here for all processing except interpolation) - returns planar result
	yuy2 = Source.IsYUY2()
	Source = yuy2 ? Source.nonyuy2clipin(true) : Source

	# Basic source-match. Find difference between source clip & equivalent fields in interpolated/smoothed clip (called the "error" in formula below). Ideally
	# there should be no difference, we want the fields in the output to be as close as possible to the source whilst remaining shimmer-free. So adjust the
	# *source* in such a way that smoothing it will give a result closer to the unadjusted source. Then rerun the interpolation (edi) and binomial smooth with
	# this new source. Result will still be shimmer-free and closer to the original source.
	# Formula used for correction is P0' = P0 + (P0-P1)/(k+S(1-k)), where P0 is original image, P1 is the 1st attempt at interpolation/smoothing , P0' is the
	# revised image to use as new source for interpolation/smoothing, k is the weighting given to the current frame in the smooth, and S is a factor indicating
	# "temporal similarity" of the error from frame to frame, i.e. S = average over all pixels of [neighbor frame error / current frame error] . Decreasing
	# S will make the result sharper, sensible range is about -0.25 to 1.0. Empirically, S=0.5 is effective [will do deeper analysis later]
	errorTemporalSimilarity = 0.5  # S in formula described above
	errorAdjust1   = Select( MatchTR1, 1.0, 2.0 / (1.0 + errorTemporalSimilarity), 8.0 / (3.0 + 5.0 * errorTemporalSimilarity) )
	match1Clip     = (SourceMatch < 1 || InputType == 1) ? Deinterlace : Deinterlace.SeparateFields().SelectEvery( 4, 0,3 ).KWeave()
	match1Update   = (SourceMatch < 1 || MatchTR1 == 0) \
						? Source : KTGMC_ErrorAdjust( Source, match1Clip, errorAdjust1, U=3,V=3 )

	noyv16ma1Edi   = MatchEdi =="NNEDI2" || MatchEdi =="NNEDI" || MatchEdi =="EEDI3+NNEDI3" || MatchEdi =="EEDI3" || MatchEdi =="EEDI2" || MatchEdi =="TDeint" || MatchEdi =="cYadif" || MatchEdi =="RepcYadif"
	noyv16ma2Edi   = MatchEdi2=="NNEDI2" || MatchEdi2=="NNEDI" || MatchEdi2=="EEDI3+NNEDI3" || MatchEdi2=="EEDI3" || MatchEdi2=="EEDI2" || MatchEdi2=="TDeint" || MatchEdi2=="cYadif" || MatchEdi2=="RepcYadif"
	match1Edi      = (SourceMatch == 0) ? NOP() : \
	!noyv16ma1Edi || !yuy2 ? match1Update.KTGMC_Interpolate( InputType, MatchEdi, MatchNNSize, MatchNNeurons, MatchEdiQual, MatchEdiMaxD, EdiThreads ) : \
	                         match1Update.nonyuy2clipout(true) \
	                                     .KTGMC_Interpolate( InputType, MatchEdi, MatchNNSize, MatchNNeurons, MatchEdiQual, MatchEdiMaxD, EdiThreads ) \
	                                     .nonyuy2clipin(true)
	match1Super    = (SourceMatch > 0 && MatchTR1 > 0) ? match1Edi.KMSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad ) : NOP()
	match1         = (SourceMatch < 1) ? Deinterlace : \
	                 (MatchTR1 == 0)   ? match1Edi : \
	                 (MatchTR1 == 1)   ? match1Edi.KMDegrain1( match1Super, bVec1,fVec1,              thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag ) : \
	                                     match1Edi.KMDegrain2( match1Super, bVec1,fVec1, bVec2,fVec2, thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag )

	# Enhance effect of source-match stages 2 & 3 by sharpening clip prior to refinement (source-match tends to underestimate so this will leave result sharper)
	match1Shp = (SourceMatch > 1 && MatchEnhance > 0.0) ? match1.KTGMC_Resharpen( match1.KRemoveGrain( 12 ), MatchEnhance, U=3,V=3 ) : match1

	# Source-match refinement. Find difference between source clip & equivalent fields in (updated) interpolated/smoothed clip. Interpolate & binomially smooth
	# this difference then add it back to output. Helps restore differences that the basic match missed. However, as this pass works on a difference rather than
	# the source image it can be prone to occasional artefacts (difference images are not ideal for interpolation). In fact a lower quality interpolation such
	# as a simple bob often performs nearly as well as advanced, slower methods (e.g. NNEDI3)
	match2Clip     = (SourceMatch < 2 || InputType == 1) ? match1Shp : match1Shp.SeparateFields().SelectEvery( 4, 0,3 ).KWeave()
	match2Diff     = (SourceMatch > 1) ? KMakeDiff( Source, match2Clip, U=3,V=3 ) : NOP()
	match2Edi      = (SourceMatch <= 1) ? NOP() : \
	!noyv16ma2Edi || !yuy2 ? match2Diff.KTGMC_Interpolate( InputType, MatchEdi2, MatchNNSize2, MatchNNeurons2, MatchEdiQual2, MatchEdiMaxD2, EdiThreads ) : \
	                         match2Diff.nonyuy2clipout(true) \
	                                   .KTGMC_Interpolate( InputType, MatchEdi2, MatchNNSize2, MatchNNeurons2, MatchEdiQual2, MatchEdiMaxD2, EdiThreads ) \
	                                   .nonyuy2clipin(true)
	match2Super    = (SourceMatch > 1 && MatchTR2 > 0) ? match2Edi.KMSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad ) : NOP()
	match2         = (SourceMatch < 2) ? match1 : \
	                 (MatchTR2 == 0)   ? match2Edi : \
	                 (MatchTR2 == 1)   ? match2Edi.KMDegrain1( match2Super, bVec1,fVec1,              thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag ) : \
	                                     match2Edi.KMDegrain2( match2Super, bVec1,fVec1, bVec2,fVec2, thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag )

	# Source-match second refinement - correct error introduced in the refined difference by temporal smoothing. Similar to error correction from basic step
	errorAdjust2   = Select( MatchTR2, 1.0, 2.0 / (1.0 + errorTemporalSimilarity), 8.0 / (3.0 + 5.0 * errorTemporalSimilarity) )
	match3Update   = (SourceMatch < 3 || MatchTR2 == 0) \
	                     ? match2Edi : KTGMC_ErrorAdjust( match2Edi, match2, errorAdjust2, U=3,V=3 )
	match3Super    = (SourceMatch > 2 && MatchTR2 > 0) ? match3Update.KMSuper( pel=SubPel, sharp=SubPelInterp, levels=1, hpad=hpad, vpad=vpad ) : NOP()
	match3         = (SourceMatch < 3) ? match2 : \
	                 (MatchTR2 == 0)   ? match3Update : \
	                 (MatchTR2 == 1)   ? match3Update.KMDegrain1( match3Super, bVec1,fVec1,              thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag ) : \
	                                     match3Update.KMDegrain2( match3Super, bVec1,fVec1, bVec2,fVec2, thSAD=ThSAD1, thSCD1=ThSCD1,thSCD2=ThSCD2, binomial=True, useFlag=useFlag )

	# Apply difference calculated in source-match refinement
	return (SourceMatch < 2) ? match1 : match1Shp.KAddDiff( match3, U=3,V=3 )
}


# Set global variable called "Prefix_Name" to "Value". Throws exception if global already exists unless Replace=true, in which case the global is overwritten
function KTGMC_SetUserGlobal( string Prefix, string Name, val Value, bool "Replace" )
{
	Replace = default( Replace, false )
	globalName = Prefix + "_" + Name

	# Tricky logic to check global: enter catch block if Replace=true *or* globalName doesn't exist (i.e. need to set the global), the exception is not rethrown
	# Not entering catch block means that Replace=false and global exists - so it throws an exception back to AviSynth
	try { Assert( !Replace && defined(Eval(globalName)) ) }
	catch (e)
	{
		Eval( "global " + globalName + " = Value" )
		Replace = true
	}
	Assert( Replace, """Multiple calls to KTGMC, set PrevGlobals="Replace" or read documentation on 'Multiple KTGMC Calls'""" )
}

# Return value of global variable called "Prefix_Name". Returns NOP() if it doesn't exist or Reuse is false
function KTGMC_GetUserGlobal( string Prefix, string Name, bool "Reuse" )
{
	Reuse = default( Reuse, false )
	globalName = Prefix + "_" + Name

	try       { ret = Reuse ? Eval( globalName ) : NOP() }
	catch (e) { ret = NOP() }
	return ret
}
